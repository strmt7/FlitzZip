#include "app/main_window_impl.hpp"

#include "app/log_policy.hpp"

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
namespace {

// Purpose: Replace filesystem-hostile characters in a derived folder name.
// Inputs: `value` is a filename stem chosen from a user-selected archive path.
// Outputs: Returns a valid local folder component, never empty.
std::wstring sanitize_extract_folder_component(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' || ch == L'\\' ||
            ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) {
        value.pop_back();
    }
    if (value.empty()) {
        return L"archive";
    }
    return value;
}

// Purpose: Return a lowercase ASCII-only copy for registered extension suffix matching.
// Inputs: `value` is a Windows filename or registered extension string.
// Outputs: Returns `value` with only ASCII `A-Z` folded to lowercase.
std::wstring ascii_lower_wide(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return value;
}

// Purpose: Derive a display-safe output folder base from an archive filename.
// Inputs: `archive` is a selected archive path with a registered extension.
// Outputs: Returns the filename with the full registered extension removed, sanitized for a local folder component.
std::wstring extract_folder_base_name(const std::filesystem::path& archive) {
    auto filename = archive.filename().wstring();
    const auto format = detect_archive_format_by_extension(archive);
    const auto extension = widen(archive_format_extension_info_for_path(format, archive).extension);
    const auto lowered_filename = ascii_lower_wide(filename);
    const auto lowered_extension = ascii_lower_wide(extension);
    if (!extension.empty() && lowered_filename.size() > lowered_extension.size() &&
        lowered_filename.compare(lowered_filename.size() - lowered_extension.size(), lowered_extension.size(),
                                 lowered_extension) == 0) {
        filename.resize(filename.size() - extension.size());
    } else {
        filename = archive.stem().wstring();
    }
    return sanitize_extract_folder_component(std::move(filename));
}

// Purpose: Derive the per-archive output folder for multi-archive extraction.
// Inputs: `archive` is the selected archive path and `existing` contains names already assigned in this job.
// Outputs: Returns a unique folder name under the common extraction root and records the chosen name.
std::filesystem::path unique_extract_folder_name(const std::filesystem::path& archive,
                                                 std::vector<std::wstring>& existing) {
    const auto base = extract_folder_base_name(archive);
    std::wstring candidate = base;
    int suffix = 2;
    while (std::ranges::find(existing, candidate) != existing.end()) {
        candidate = base + L" (" + std::to_wstring(suffix) + L")";
        ++suffix;
    }
    existing.push_back(candidate);
    return std::filesystem::path(candidate);
}

// Purpose: Build extraction destinations for one or many selected archives.
// Inputs: `archives` is non-empty and `root` is the selected extraction destination.
// Outputs: Returns one destination per archive; single-archive behavior preserves `root` exactly.
std::vector<std::filesystem::path> extraction_outputs_for_archives(const std::vector<std::filesystem::path>& archives,
                                                                   const std::filesystem::path& root) {
    if (archives.size() <= 1U) {
        return {root};
    }
    std::vector<std::filesystem::path> outputs;
    std::vector<std::wstring> names;
    outputs.reserve(archives.size());
    names.reserve(archives.size());
    for (const auto& archive : archives) {
        outputs.push_back(root / unique_extract_folder_name(archive, names));
    }
    return outputs;
}

}  // namespace

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
            const auto outputs = extraction_outputs_for_archives(request.archives, request.output);
            for (std::size_t index = 0; index < request.archives.size(); ++index) {
                const auto& archive = request.archives[index];
                const auto& output = outputs[index];
                if (request.integrity) {
                    const auto hash = hash_path(archive, IntegrityMode::Sha256);
                    append_history_entry("Security", archive.filename().string(), archive.string(),
                                         integrity_history_status("Archive", hash), true);
                }
                if (request.defender) {
                    const auto pre_scan = scan_with_windows_defender(archive, DefenderScanMode::FullPath);
                    append_history_entry("Security", archive.filename().string(), archive.string(),
                                         defender_history_status("Defender archive", pre_scan),
                                         !pre_scan.attempted || pre_scan.clean);
                    if (pre_scan.attempted && !pre_scan.clean) {
                        throw SecurityError("Microsoft Defender did not report the archive as clean: " +
                                            archive.string());
                    }
                }
                auto progress_callback = [this](const ProgressSnapshot& snapshot) {
                    publish_progress_snapshot(snapshot);
                };
                const auto archive_format = detect_archive_format(archive);
                const auto stats = extract_detected_archive(archive_format, archive, output, request.gpu_required,
                                                            request.overwrite, progress_callback);
                std::ostringstream line;
                line << "Extracted " << archive_format_info(archive_format).key << " to " << output.string() << " in "
                     << stats.seconds << "s";
                append_history_entry("Extract", archive.filename().string(), output.string(), line.str(), true);
                if (request.integrity) {
                    const auto hash = hash_path(output, IntegrityMode::Sha256);
                    append_history_entry("Security", output.filename().string(), output.string(),
                                         integrity_history_status("Output", hash), true);
                }
                if (request.defender) {
                    const auto post_scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
                    append_history_entry("Security", output.filename().string(), output.string(),
                                         defender_history_status("Defender output", post_scan),
                                         !post_scan.attempted || post_scan.clean);
                }
            }
        },
        "Extracting");
}

// Purpose: Start a background extraction job from the current GUI queue.
// Inputs: None; reads queued archive, destination, security, and GPU options from synchronized UI state.
// Outputs: Launches a worker job, updates progress/history/status, blocks unclean pre-scans, or requests repaint when
// no archive is selected.
void MainWindow::start_extract() {
    ExtractJobRequest request;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        request.archives = selected_extract_archive_paths(state_);
        request.gpu_required = state_.gpu_required;
        request.overwrite = state_.overwrite;
        request.integrity = state_.integrity_hash_opt_in;
        request.defender = state_.defender_scan_opt_in;
        request.output = extraction_output_path_for(state_);
    }
    if (request.archives.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Select an archive before starting extraction";
        }
        request_repaint();
        return;
    }
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
                .archive_name = failure_operation + " failure",
                .archive_path =
                    state_.progress.current_entry.empty() ? std::string("-") : state_.progress.current_entry,
                .detail = error.what(),
                .success = false,
            });
            state_.selected_history_index = static_cast<int>(state_.history.size()) - 1;
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

// Purpose: Append one bounded log entry when enabled by the active log level.
// Inputs: `severity` is the visible category and `message` is safe session text.
// Outputs: Adds a timestamped row, persists it to the per-user log file, prunes rows, and queues repaint.
void MainWindow::append_log_entry(LogSeverity severity, std::string message) {
    std::chrono::system_clock::time_point timestamp{};
    int log_level = 0;
    {
        std::lock_guard lock(mutex_);
        log_level = state_.log_level_index;
        if (!log_level_allows(log_level, severity)) {
            return;
        }
        timestamp = std::chrono::system_clock::now();
        prune_log_entries(state_.logs, timestamp, state_.log_retention_index, kMaxLogEntries);
        state_.logs.push_back(LogEntry{.severity = severity, .message = message, .timestamp = timestamp});
        prune_log_entries(state_.logs, timestamp, state_.log_retention_index, kMaxLogEntries);
    }
    try {
        append_log_file_entry(log_file_path(), severity, message, timestamp);
    } catch (...) {
        std::lock_guard lock(mutex_);
        state_.status = "Log file write failed";
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
    append_history_entry(operation, line, "-", line, success);
}

// Purpose: Add a structured operation result to session history.
// Inputs: `operation`, `archive_name`, `archive_path`, `detail`, and `success` describe the visible history row.
// Outputs: Appends one row to in-memory UI history and selects it for details.
void MainWindow::append_history_entry(std::string operation, std::string archive_name, std::string archive_path,
                                      std::string detail, bool success) {
    std::lock_guard lock(mutex_);
    state_.history.push_back(HistoryEntry{
        .operation = std::move(operation),
        .archive_name = std::move(archive_name),
        .archive_path = std::move(archive_path),
        .detail = std::move(detail),
        .success = success,
    });
    state_.selected_history_index = static_cast<int>(state_.history.size()) - 1;
}

}  // namespace superzip::app
