#include "app/main_window_impl.hpp"

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

namespace superzip::app {

// Purpose: Detect whether Ask-before-overwriting needs user confirmation.
// Inputs: `destination` is the extraction root.
// Outputs: Returns true when the destination exists and is non-empty or cannot be proven empty.
bool MainWindow::extract_overwrite_prompt_needed(const std::filesystem::path& destination) const {
    std::error_code error;
    if (!std::filesystem::exists(destination, error) && !error) {
        return false;
    }
    if (!error && std::filesystem::is_directory(destination, error) && !error) {
        std::filesystem::directory_iterator iterator(destination,
                                                     std::filesystem::directory_options::skip_permission_denied, error);
        if (!error && iterator == std::filesystem::directory_iterator{}) {
            return false;
        }
    }
    return true;
}

// Purpose: Open the SuperZip-owned overwrite confirmation modal for a pending extract job.
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
// Outputs: Starts the worker, updates progress/history/status, and performs pre/post security scans.
void MainWindow::launch_extract_job(ExtractJobRequest request) {
    run_job(
        [this, request = std::move(request)] {
            const auto& archive = request.archive;
            const auto& output = request.output;
            if (request.integrity) {
                const auto hash = hash_file(archive, IntegrityMode::Sha256);
                append_history_entry("Security", archive.filename().string(), "SHA-256 " + hash.hex_digest, true);
            }
            if (request.defender) {
                const auto pre_scan = scan_with_windows_defender(archive, DefenderScanMode::FullPath);
                append_history_entry("Security", archive.filename().string(),
                                     defender_history_status("Defender archive", pre_scan),
                                     !pre_scan.attempted || pre_scan.clean);
                if (pre_scan.attempted && !pre_scan.clean) {
                    throw SecurityError("Microsoft Defender did not report the archive as clean: " + archive.string());
                }
            }
            auto progress_callback = [this](const ProgressSnapshot& snapshot) { publish_progress_snapshot(snapshot); };
            const auto archive_format = detect_archive_format(archive);
            const auto stats = extract_detected_archive(archive_format, archive, output, request.gpu_required,
                                                        request.overwrite, progress_callback);
            std::ostringstream line;
            line << "Extracted " << archive_format_info(archive_format).key << " to " << output.string() << " in "
                 << stats.seconds << "s";
            append_history_entry("Extract", archive.filename().string(), line.str(), true);
            if (request.defender) {
                const auto post_scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
                append_history_entry("Security", output.filename().string(),
                                     defender_history_status("Defender output", post_scan),
                                     !post_scan.attempted || post_scan.clean);
            }
        },
        "Extracting");
}

// Purpose: Start a background extraction job from the current GUI queue.
// Inputs: None; reads queued archive, destination, security, and GPU options from synchronized UI state.
// Outputs: Launches a worker job, updates progress/history/status, blocks unclean pre-scans, or requests repaint when
// no archive is selected.
void MainWindow::start_extract() {
    std::vector<std::filesystem::path> sources;
    ExtractJobRequest request;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        for (std::size_t i = 0; i < state_.queued_paths.size(); ++i) {
            if (state_.queued_enabled[i]) {
                sources.push_back(state_.queued_paths[i]);
            }
        }
        request.gpu_required = state_.gpu_required;
        request.overwrite = state_.overwrite;
        request.integrity = state_.integrity_hash_opt_in;
        request.defender = state_.defender_scan_opt_in;
        request.output = extraction_output_path_for(state_);
    }
    if (sources.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Select an archive before starting extraction";
        }
        request_repaint();
        return;
    }
    request.archive = sources.front();
    if (!request.overwrite && extract_overwrite_prompt_needed(request.output)) {
        show_extract_overwrite_prompt(std::move(request));
        return;
    }
    launch_extract_job(std::move(request));
}

// Purpose: Run one long operation on the background worker thread.
// Inputs: `job` performs the operation and `label` is the visible busy status.
// Outputs: Updates status, progress, history failure rows, and repaint state when the worker completes.
void MainWindow::run_job(std::function<void()> job, std::string label) {
    if (worker_running_.exchange(true)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::string failure_operation = operation_for_job_label(label);
    {
        std::lock_guard lock(mutex_);
        state_.status = label;
        state_.progress = {};
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    worker_ = std::thread([this, job = std::move(job), failure_operation] {
        try {
            job();
            std::lock_guard lock(mutex_);
            state_.status = "Ready";
            retain_progress_after_stop_locked(true);
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Error: ") + error.what();
            retain_progress_after_stop_locked(false);
            state_.history.push_back(HistoryEntry{
                .operation = failure_operation,
                .subject = state_.status,
                .detail = error.what(),
                .success = false,
            });
        }
        worker_running_ = false;
        request_repaint();
    });
    request_repaint();
}

// Purpose: Publish one active operation progress snapshot to the UI.
// Inputs: `snapshot` is an immutable worker progress sample.
// Outputs: Replaces visible progress, cancels any completed-progress hold timer, and queues repaint.
void MainWindow::publish_progress_snapshot(const ProgressSnapshot& snapshot) {
    {
        std::lock_guard lock(mutex_);
        state_.progress = snapshot;
        state_.progress_visible_until = {};
    }
    KillTimer(hwnd_, kProgressHoldTimer);
    request_repaint();
}

// Purpose: Keep the last progress sample visible after an operation stops.
// Inputs: `mark_complete` fills known totals for successful work; caller must hold `mutex_`.
// Outputs: Arms the 15-second clear timer or clears idle progress.
void MainWindow::retain_progress_after_stop_locked(bool mark_complete) {
    if (state_.progress.operation == OperationKind::Idle) {
        state_.progress_visible_until = {};
        return;
    }
    if (mark_complete) {
        if (state_.progress.total_bytes > 0U) {
            state_.progress.processed_bytes = state_.progress.total_bytes;
        }
        if (state_.progress.total_entries > 0U) {
            state_.progress.completed_entries = state_.progress.total_entries;
        }
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
            should_kill_timer = true;
        }
    }
    if (should_kill_timer) {
        KillTimer(hwnd_, kProgressHoldTimer);
        request_repaint();
    }
}

// Purpose: Append one bounded in-memory log entry.
// Inputs: `severity` is the visible category and `message` is safe session text.
// Outputs: Adds a timestamped row, prunes expired/over-capacity rows, and queues repaint.
void MainWindow::append_log_entry(LogSeverity severity, std::string message) {
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::system_clock::now();
        prune_log_entries(state_.logs, now, state_.log_retention_index, kMaxLogEntries);
        state_.logs.push_back(LogEntry{.severity = severity, .message = std::move(message), .timestamp = now});
        prune_log_entries(state_.logs, now, state_.log_retention_index, kMaxLogEntries);
    }
    request_repaint();
}

// Purpose: Convert a legacy history text line into a structured session row.
// Inputs: `line` is the existing caller text.
// Outputs: Appends a classified history entry.
void MainWindow::append_history(const std::string& line) {
    const bool success = !line.starts_with("Error");
    std::string operation = "Security";
    if (line.starts_with("Compressed")) {
        operation = "Compress";
    } else if (line.starts_with("Extracted")) {
        operation = "Extract";
    } else if (line.starts_with("Error")) {
        operation = "Failure";
    }
    append_history_entry(operation, line, line, success);
}

// Purpose: Add a structured operation result to session history.
// Inputs: `operation`, `subject`, `detail`, and `success` describe the visible history row.
// Outputs: Appends one row to in-memory UI history.
void MainWindow::append_history_entry(std::string operation, std::string subject, std::string detail, bool success) {
    std::lock_guard lock(mutex_);
    state_.history.push_back(HistoryEntry{
        .operation = std::move(operation),
        .subject = std::move(subject),
        .detail = std::move(detail),
        .success = success,
    });
}

}  // namespace superzip::app
