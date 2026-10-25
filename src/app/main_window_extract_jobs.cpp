#include "app/main_window_impl.hpp"

#include "app/extraction_destinations.hpp"
#include "app/log_policy.hpp"
#include "app/history_publication.hpp"
#include "core/path_text.hpp"

#include <cmath>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif

namespace flitzzip::app {

// Purpose: Open the FlitzZip-owned overwrite confirmation modal for a pending extract job.
// Inputs: `request` is the fully captured job request to run if the user continues.
// Outputs: Stores the pending job, updates modal UI state, clears progress, and queues repaint.
void MainWindow::show_extract_overwrite_prompt(ExtractJobRequest request) {
    {
        std::lock_guard lock(mutex_);
        pending_extract_job_ = std::move(request);
        pending_extract_job_active_ = true;
        modal_focus_index_ = 1;
        state_.active_dropdown = DropdownId::None;
        state_.extract_overwrite_prompt_visible = true;
        state_.extract_overwrite_prompt_destination = pending_extract_job_.output.wstring();
        state_.status = "Confirm overwrite policy";
        state_.progress = {};
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    request_repaint();
}

// Purpose: Cancel the pending overwrite confirmation.
// Inputs: None.
// Outputs: Clears modal and pending job state without starting extraction.
void MainWindow::cancel_extract_overwrite_prompt() {
    {
        std::lock_guard lock(mutex_);
        pending_extract_job_ = {};
        pending_extract_job_active_ = false;
        state_.extract_overwrite_prompt_visible = false;
        state_.extract_overwrite_prompt_destination.clear();
        state_.status = "Extraction cancelled before overwriting";
        state_.progress = {};
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    request_repaint();
}

// Purpose: Continue the pending extract job with overwrite enabled for this job only.
// Inputs: None.
// Outputs: Clears modal state and starts the captured extraction job.
void MainWindow::continue_extract_overwrite_prompt() {
    ExtractJobRequest request;
    {
        std::lock_guard lock(mutex_);
        if (!pending_extract_job_active_) {
            return;
        }
        request = pending_extract_job_;
        request.overwrite = true;
        pending_extract_job_ = {};
        pending_extract_job_active_ = false;
        state_.extract_overwrite_prompt_visible = false;
        state_.extract_overwrite_prompt_destination.clear();
        state_.status = "Overwrite approved for this extraction";
    }
    request_repaint();
    launch_extract_job(std::move(request));
}

// Purpose: Launch a captured extraction job on the background worker.
// Inputs: `request` contains archive, destination, GPU, security, and overwrite choices.
// Outputs: Starts the worker, captures publication policy, updates progress/history/status, and runs requested scans.
void MainWindow::launch_extract_job(ExtractJobRequest request) {
    const auto destination_to_open =
        operation_destination_path(OperationKind::Extract, request.output, false, request.open_destination);
    run_job(
        [this, request = std::move(request)] {
            const auto outputs = extraction_outputs_for_archives(request.archives, request.output);
            for (std::size_t index = 0; index < request.archives.size(); ++index) {
                const auto& archive = request.archives[index];
                const auto& output = outputs[index];
                const auto archive_source = pin_source_file(archive);
                if (request.integrity) {
                    const auto hash = hash_path(archive_source.path(), IntegrityMode::Sha256);
                    append_history_entry("Security", path_diagnostic_utf8(archive.filename()),
                                         path_diagnostic_utf8(archive), integrity_history_status("Archive", hash),
                                         true);
                }
                if (request.defender) {
                    const auto pre_scan = scan_with_windows_defender(archive_source.path(), DefenderScanMode::FullPath);
                    append_history_entry(
                        "Security", path_diagnostic_utf8(archive.filename()), path_diagnostic_utf8(archive),
                        defender_history_status("Defender archive", pre_scan), defender_scan_passed(pre_scan));
                    require_clean_defender_scan(pre_scan, archive);
                }
                auto progress_callback = [this](const ProgressSnapshot& snapshot) {
                    publish_progress_snapshot_or_cancel(snapshot);
                };
                const auto archive_format = detect_archive_format(archive_source.path());
                OperationStats stats;
                const auto extraction_start = std::chrono::steady_clock::now();
                std::function<void(const std::filesystem::path&)> inspect_output;
                if (request.defender) {
                    inspect_output = [this, &output](const auto& directory) {
                        const auto post_scan = scan_with_windows_defender(directory, DefenderScanMode::FullPath);
                        append_history_entry(
                            "Security", path_diagnostic_utf8(output.filename()), path_diagnostic_utf8(output),
                            defender_history_status("Defender output", post_scan), defender_scan_passed(post_scan));
                        require_clean_defender_scan(post_scan, output);
                    };
                }
                extract_with_publication(
                    {.destination = output,
                     .overwrite = request.overwrite,
                     .validate_before_publish = request.validate_before_publish},
                    [&](const auto& directory, bool overwrite) {
                        stats = extract_detected_archive(archive_format, archive_source.path(), directory,
                                                         request.gpu_required, overwrite, progress_callback,
                                                         request.name_encoding);
                    },
                    inspect_output,
                    [this] {
                        if (operation_cancel_requested_.load())
                            throw ArchiveError("operation cancelled");
                    });
                stats.seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - extraction_start).count();
                std::ostringstream line;
                line << "Extracted " << archive_format_info(archive_format).key << " to "
                     << path_diagnostic_utf8(output) << " in " << stats.seconds << "s";
                append_history_entry("Extract", path_diagnostic_utf8(archive.filename()), path_diagnostic_utf8(output),
                                     line.str(), true);
                if (request.integrity) {
                    const auto hash = hash_path(output, IntegrityMode::Sha256);
                    append_history_entry("Security", path_diagnostic_utf8(output.filename()),
                                         path_diagnostic_utf8(output), integrity_history_status("Output", hash), true);
                }
            }
        },
        "Extracting", OperationKind::Extract, destination_to_open);
}

// Purpose: Start a background extraction job from the current GUI queue.
// Inputs: None; captures archive, destination, publication, security, and GPU options from synchronized UI state.
// Outputs: Launches a worker job, updates progress/history/status, blocks unclean pre-scans, or requests repaint when
// no archive is selected. Native default-folder discovery runs after unlocking and before any job starts.
void MainWindow::start_extract() {
    ExtractJobRequest request;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        request.archives = selected_extract_archive_paths(state_);
        request.gpu_required = state_.gpu_required;
        request.overwrite = state_.overwrite;
        request.name_encoding = selected_name_encoding(state_).encoding;
        request.integrity = state_.integrity_hash_opt_in;
        request.defender = state_.defender_scan_opt_in;
        request.validate_before_publish = state_.validate_before_publish;
        request.open_destination =
            applied_settings_.open_destination_after_operation || state_.open_destination_after_extract;
        request.output = state_.destination_directory;
    }
    if (request.archives.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Select an archive before starting extraction";
        }
        request_repaint();
        return;
    }
    if (request.output.empty()) {
        request.output = default_extraction_output_directory();
    }
    if (!request.overwrite && extraction_destination_needs_confirmation(request.output)) {
        show_extract_overwrite_prompt(std::move(request));
        return;
    }
    launch_extract_job(std::move(request));
}

// Purpose: Run one long operation with transactional admission and supervised completion.
// Inputs: job, busy label, operation kind and optional destination are captured for this operation.
// Outputs: Starts at most one worker; preparation, launch and completion failures restore idle admission.
void MainWindow::run_job(std::function<void()> job, std::string label, OperationKind operation,
                         std::filesystem::path destination_to_open) {
    if (window_close_requested_ || worker_.running()) {
        return;
    }
    consume_worker_failure();
    // Prepare emergency error text before taking admission; reporting must remain possible if allocation fails.
    std::string failure_status = "Error: operation worker could not prepare or publish its result";
    worker_failure_status_.swap(failure_status);
    try {
        const std::string failure_operation = operation_for_job_label(label);
        const bool show_summary = applied_settings_.show_operation_summary;
        const auto result = worker_.start(
            [&] {
                operation_cancel_requested_.store(false);
                {
                    std::lock_guard lock(mutex_);
                    operation_summary_.begin(state_.history.size());
                    operation_destination_.begin(std::move(destination_to_open));
                    state_.status = label;
                    state_.progress = {};
                    state_.progress.operation = operation;
                    state_.active_operation = operation;
                    state_.operation_cancel_requested = false;
                    state_.smoothed_time_remaining_seconds = -1.0;
                    state_.progress_visible_until = {};
                    if (operation == OperationKind::Verify) {
                        state_.security_archive_paths = SecurityCheckState::Running;
                        state_.security_payload_integrity = SecurityCheckState::Running;
                        state_.security_sha256 =
                            state_.integrity_hash_opt_in ? SecurityCheckState::Running : SecurityCheckState::NotRun;
                        state_.security_defender =
                            state_.defender_scan_opt_in ? SecurityCheckState::Running : SecurityCheckState::NotRun;
                    }
                }
                append_operation_log(LogSeverity::Information, failure_operation, "started");
                KillTimer(hwnd_, kProgressHoldTimer);
            },
            [this, job = std::move(job), failure_operation, show_summary] {
                execute_job(job, failure_operation, show_summary);
            },
            [this]() noexcept { request_repaint(); });
        if (result == BackgroundJob::StartResult::Failed) {
            consume_worker_failure();
        }
    } catch (...) {
        report_worker_failure(std::current_exception());
    }
    request_repaint();
}

// Purpose: Execute and publish an admitted archive operation while the worker owns admission.
// Inputs: job and category belong to the worker; show_summary is the captured applied preference.
// Outputs: Publishes ordinary success/failure; unexpected reporting failures propagate to BackgroundJob supervision.
void MainWindow::execute_job(const std::function<void()>& job, const std::string& failure_operation,
                             bool show_summary) {
    bool completed = false;
    bool cancelled = false;
    std::string failure_text;
    try {
        job();
        if (operation_cancel_requested_.load()) {
            throw ArchiveError("operation cancelled");
        }
        completed = true;
    } catch (const std::exception& error) {
        cancelled = operation_cancel_requested_.load();
        failure_text = error.what();
    } catch (...) {
        cancelled = operation_cancel_requested_.load();
        failure_text = "unknown operation failure";
    }
    {
        std::lock_guard lock(mutex_);
        if (completed) {
            state_.status = "Ready";
            state_.smoothed_time_remaining_seconds = 0.0;
            retain_progress_after_stop_locked(true);
        } else {
            state_.status = cancelled ? "Operation stopped" : std::string("Error: ") + failure_text;
            retain_progress_after_stop_locked(false);
            publish_history_result(
                state_, HistoryEntry{
                            .operation = failure_operation,
                            .archive_name = cancelled ? failure_operation + " stopped" : failure_operation + " failure",
                            .archive_path = state_.progress.current_entry.empty() ? std::string("-")
                                                                                  : state_.progress.current_entry,
                            .detail = cancelled ? std::string("Operation stopped by the user") : failure_text,
                            .success = false,
                        });
        }
        state_.active_operation = OperationKind::Idle;
        state_.operation_cancel_requested = false;
    }
    operation_cancel_requested_.store(false);
    append_operation_log(completed ? LogSeverity::Information : LogSeverity::Warning, failure_operation,
                         completed ? "completed" : (cancelled ? "stopped" : "failed"));
    {
        std::lock_guard lock(mutex_);
        operation_summary_.complete(state_.history, failure_operation, show_summary);
        operation_destination_.complete(completed);
    }
}

// Purpose: Present a supervised preparation or completion failure without retrying archive work.
// Inputs: failure is the original exception; worker_failure_status_ was allocated before admission.
// Outputs: Restores idle state, marks unfinished checks incomplete, and presents exact or prepared error text.
void MainWindow::report_worker_failure(std::exception_ptr failure) {
    std::string diagnostic;
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        try {
            diagnostic = std::string("Error: ") + error.what();
        } catch (...) {
            // Error presentation itself may be the failed allocation. Preserve prepared nonallocating text.
        }
    } catch (...) {
    }
    {
        std::lock_guard lock(mutex_);
        state_.status.swap(diagnostic.empty() ? worker_failure_status_ : diagnostic);
        for (auto* check : {&state_.security_archive_paths, &state_.security_payload_integrity, &state_.security_sha256,
                            &state_.security_defender}) {
            if (*check == SecurityCheckState::Running) {
                *check = SecurityCheckState::Incomplete;
            }
        }
        state_.active_operation = OperationKind::Idle;
        state_.operation_cancel_requested = false;
        operation_summary_.begin(state_.history.size());
        operation_destination_.begin({});
        retain_progress_after_stop_locked(false);
    }
    operation_cancel_requested_.store(false);
    request_repaint();
}

// Purpose: Consume terminal worker exceptions on the controller thread after actual thread cleanup.
// Inputs: None; active work is not interrupted. Outputs: Presents each retained failure once.
void MainWindow::consume_worker_failure() {
    if (const auto failure = worker_.take_failure()) {
        report_worker_failure(failure);
    }
}

// Purpose: Request cooperative cancellation while preserving the native message pump during shutdown.
// Inputs: UI thread; the current worker may still own callbacks and native notifications.
// Outputs: Rejects later job starts, requests cancellation and closes immediately only when work is terminal.
void MainWindow::request_window_close() {
    window_close_requested_ = true;
    operation_cancel_requested_.store(true);
    if (worker_.running()) {
        request_stop_operation();
    }
    static_cast<void>(complete_window_close());
}

// Purpose: Release the notification window only after its archive worker has finished.
// Inputs: UI thread; completion notifications and the normal clock keep the pump responsive while work drains.
// Outputs: Joins actual closure cleanup and destroys the window for a pending terminal close; otherwise returns false.
bool MainWindow::complete_window_close() {
    if (!window_close_requested_ || worker_.running()) {
        return false;
    }
    worker_.join();
    if (DestroyWindow(hwnd_)) {
        return true;
    }
    const auto error = GetLastError();
    window_close_requested_ = false;
    {
        std::lock_guard lock(mutex_);
        state_.status = "Window close failed (Windows error " + std::to_string(error) + ")";
    }
    request_repaint();
    return false;
}

// Purpose: Present a completed job's result through the existing History detail surface.
// Inputs: None; called only by the UI thread's coalesced repaint message handler.
// Outputs: Consumes one pending selection, resets filters/focus, reveals the selected row, and queues repaint.
void MainWindow::show_pending_operation_summary() {
    std::size_t row_count = 0;
    int selected = -1;
    std::string operation;
    {
        std::lock_guard lock(mutex_);
        if (state_.active_operation != OperationKind::Idle) {
            return;
        }
        const auto pending = operation_summary_.take();
        if (!pending) {
            return;
        }
        selected = *pending;
        row_count = state_.history.size();
        if (selected < 0 || static_cast<std::size_t>(selected) >= row_count) {
            return;
        }
        state_.selected_history_index = selected;
        state_.history_operation_filter_index = 0;
        state_.history_status_filter_index = 0;
        operation = state_.history[static_cast<std::size_t>(selected)].operation;
    }
    set_page(Page::History);
    reset_history_details_scroll_state();
    const auto table = history_layout(content_rect()).table;
    history_scroll_first_row_ = std::min(selected, history_max_scroll_offset(table, row_count));
    append_log_entry(LogSeverity::Information, "Operation summary shown: " + operation);
}

// Purpose: Request cooperative cancellation of the active background operation.
// Inputs: None; reads the synchronized active operation and worker flag.
// Outputs: Marks cancellation state, updates visible status, logs the request, and queues repaint.
void MainWindow::request_stop_operation() {
    std::string operation = "Operation";
    bool requested = false;
    {
        std::lock_guard lock(mutex_);
        if (state_.active_operation == OperationKind::Idle || !worker_.running()) {
            return;
        }
        if (state_.active_operation == OperationKind::Compress) {
            operation = "Compress";
        } else if (state_.active_operation == OperationKind::Extract) {
            operation = "Extract";
        } else if (state_.active_operation == OperationKind::Verify) {
            operation = "Security";
        }
        operation_cancel_requested_.store(true);
        state_.operation_cancel_requested = true;
        state_.progress.cancel_requested = true;
        state_.status = "Stopping...";
        requested = true;
    }
    if (requested) {
        append_operation_log(LogSeverity::Warning, operation, "stop requested");
        request_repaint();
    }
}

// Purpose: Publish one active operation progress snapshot to the UI.
// Inputs: `snapshot` is an immutable worker progress sample.
// Outputs: Replaces visible progress, cancels any completed-progress hold timer, and queues repaint.
void MainWindow::publish_progress_snapshot(const ProgressSnapshot& snapshot) {
    ProgressSnapshot visible = snapshot;
    if (operation_cancel_requested_.load()) {
        visible.cancel_requested = true;
    }
    double raw_remaining_seconds = -1.0;
    if (progress_ratio(visible) >= 1.0) {
        raw_remaining_seconds = 0.0;
    } else if (visible.total_bytes > visible.processed_bytes && visible.throughput_bytes_per_second > 0.0) {
        raw_remaining_seconds =
            static_cast<double>(visible.total_bytes - visible.processed_bytes) / visible.throughput_bytes_per_second;
    }
    {
        std::lock_guard lock(mutex_);
        if (state_.active_operation == OperationKind::Idle) {
            state_.active_operation = visible.operation;
        }
        if (raw_remaining_seconds < 0.0 || !std::isfinite(raw_remaining_seconds)) {
            state_.smoothed_time_remaining_seconds = -1.0;
        } else if (state_.smoothed_time_remaining_seconds < 0.0 || visible.operation != state_.progress.operation ||
                   raw_remaining_seconds == 0.0) {
            state_.smoothed_time_remaining_seconds = raw_remaining_seconds;
        } else {
            constexpr double kEtaSmoothingWeight = 0.22;
            state_.smoothed_time_remaining_seconds =
                (state_.smoothed_time_remaining_seconds * (1.0 - kEtaSmoothingWeight)) +
                (raw_remaining_seconds * kEtaSmoothingWeight);
        }
        state_.progress = visible;
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    request_repaint();
}

// Purpose: Publish a progress snapshot and fail cooperatively after a stop request.
// Inputs: `snapshot` is a worker progress sample.
// Outputs: Updates UI progress and throws `ArchiveError` when the user has requested cancellation.
void MainWindow::publish_progress_snapshot_or_cancel(const ProgressSnapshot& snapshot) {
    publish_progress_snapshot(snapshot);
    if (operation_cancel_requested_.load()) {
        throw ArchiveError("operation cancelled");
    }
}

// Purpose: Keep the last progress sample visible after an operation stops.
// Inputs: `mark_complete` fills known totals for successful work; caller must hold `mutex_`.
// Outputs: Arms the 15-second clear timer or clears idle progress.
void MainWindow::retain_progress_after_stop_locked(bool mark_complete) {
    if (state_.progress.operation == OperationKind::Idle) {
        state_.progress_visible_until = {};
        state_.smoothed_time_remaining_seconds = -1.0;
        return;
    }
    if (mark_complete) {
        if (state_.progress.total_bytes > 0U) {
            state_.progress.processed_bytes = state_.progress.total_bytes;
        }
        if (state_.progress.total_entries > 0U) {
            state_.progress.completed_entries = state_.progress.total_entries;
        }
        state_.smoothed_time_remaining_seconds = 0.0;
    }
    state_.progress_visible_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kProgressHoldMs);
    if (hwnd_ != nullptr) {
        SetTimer(hwnd_, kProgressHoldTimer, kProgressHoldMs, nullptr);
    }
}

// Purpose: Clear retained progress after its hold interval expires.
// Inputs: None; reads the synchronized progress hold timestamp.
// Outputs: Clears visible progress and queues repaint when the deadline has elapsed.
void MainWindow::clear_expired_progress() {
    bool should_kill_timer = false;
    {
        std::lock_guard lock(mutex_);
        if (state_.progress_visible_until == std::chrono::steady_clock::time_point{}) {
            should_kill_timer = true;
        } else if (std::chrono::steady_clock::now() < state_.progress_visible_until) {
            return;
        } else {
            state_.progress = {};
            state_.progress_visible_until = {};
            state_.smoothed_time_remaining_seconds = -1.0;
            should_kill_timer = true;
        }
    }
    if (should_kill_timer) {
        KillTimer(hwnd_, kProgressHoldTimer);
        request_repaint();
    }
}

// Purpose: Append one bounded log entry when enabled by the successfully applied logging policy.
// Inputs: `severity` is the visible category and `message` is safe session text; drafts never change runtime policy.
// Outputs: Adds a timestamped row, persists it to the per-user log file, prunes rows, and queues repaint.
void MainWindow::append_log_entry(LogSeverity severity, std::string message) {
    std::chrono::system_clock::time_point timestamp{};
    int log_level = 0;
    {
        std::lock_guard lock(mutex_);
        log_level = applied_settings_.log_level_index;
        if (!log_level_allows(log_level, severity)) {
            return;
        }
        timestamp = std::chrono::system_clock::now();
        prune_log_entries(state_.logs, timestamp, applied_settings_.log_retention_index, kMaxLogEntries);
        state_.logs.push_back(LogEntry{.severity = severity, .message = message, .timestamp = timestamp});
        prune_log_entries(state_.logs, timestamp, applied_settings_.log_retention_index, kMaxLogEntries);
    }
    try {
        append_log_file_entry(log_file_path(), severity, message, timestamp);
    } catch (...) {
        std::lock_guard lock(mutex_);
        state_.status = "Log file write failed";
    }
    request_repaint();
}

// Purpose: Append a consistently formatted operation-scoped log entry.
// Inputs: `severity`, `operation`, and `message` are safe UI/log strings.
// Outputs: Routes the formatted text through the configured log-level and retention policy.
void MainWindow::append_operation_log(LogSeverity severity, std::string_view operation, std::string_view message) {
    append_log_entry(severity, std::string(operation) + ": " + std::string(message));
}

// Purpose: Add a structured operation result to session history.
// Inputs: `operation`, `archive_name`, `archive_path`, `detail`, and `success` describe the visible history row.
// Outputs: Publishes/selects one row and requests a window-thread detail reset and repaint.
void MainWindow::append_history_entry(std::string operation, std::string archive_name, std::string archive_path,
                                      std::string detail, bool success) {
    {
        std::lock_guard lock(mutex_);
        publish_history_result(state_, HistoryEntry{
                                           .operation = std::move(operation),
                                           .archive_name = std::move(archive_name),
                                           .archive_path = std::move(archive_path),
                                           .detail = std::move(detail),
                                           .success = success,
                                       });
    }
    request_repaint();
}

}  // namespace flitzzip::app
