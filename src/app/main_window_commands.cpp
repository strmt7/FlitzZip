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

// Purpose: Switch the active page and reset page-local interaction state.
// Inputs: `page` is the requested page.
// Outputs: Mutates UI page state, reverts unapplied Settings edits when leaving Settings, and queues repaint.
void MainWindow::set_page(Page page) {
    bool revert_unapplied_settings = false;
    {
        std::lock_guard lock(mutex_);
        const Page previous = state_.page;
        if (previous == page) {
            return;
        }
        state_.page = page;
        state_.active_dropdown = DropdownId::None;
        dropdown_keyboard_index_ = -1;
        keyboard_focus_index_ = 0;
        text_tooltip_cell_active_ = false;
        text_tooltip_visible_ = false;
        text_tooltip_text_.clear();
        KillTimer(hwnd_, kTextTooltipTimer);
        revert_unapplied_settings = previous == Page::Settings && page != Page::Settings;
    }
    if (revert_unapplied_settings) {
        revert_settings_draft();
    }
    request_repaint();
}

// Purpose: Open the Windows file picker and append selected files to the queue.
// Inputs: None; user selection comes from the common dialog or GUI smoke-test environment.
// Outputs: Mutates queued paths and queues a repaint when files are selected.
void MainWindow::add_files() {
    wchar_t smoke_paths[32768]{};
    constexpr DWORD smoke_paths_capacity = static_cast<DWORD>(sizeof(smoke_paths) / sizeof(smoke_paths[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FILE_SELECTION", smoke_paths, smoke_paths_capacity);
    if (smoke_length > 0 && smoke_length < smoke_paths_capacity) {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        std::wstring_view paths(smoke_paths, smoke_length);
        std::size_t start = 0;
        while (start <= paths.size()) {
            const std::size_t end = paths.find(L';', start);
            const auto part =
                paths.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
            if (!part.empty()) {
                state_.queued_paths.emplace_back(std::wstring(part));
                state_.queued_enabled.push_back(true);
            }
            if (end == std::wstring_view::npos) {
                break;
            }
            start = end + 1;
        }
        if (was_empty && !state_.queued_paths.empty()) {
            state_.selected_queue_index = 0;
            queue_scroll_first_row_ = 0;
        }
        normalize_queue_selection_locked();
        state_.status = "Smoke files added";
        request_repaint();
        return;
    }

    OPENFILENAMEW ofn{};
    wchar_t files[8192]{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = files;
    ofn.nMaxFile = 8192;
    ofn.lpstrTitle = L"Add files to SuperZip";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    std::lock_guard lock(mutex_);
    std::filesystem::path dir(files);
    wchar_t* cursor = files + dir.wstring().size() + 1;
    const bool was_empty = state_.queued_paths.empty();
    if (*cursor == L'\0') {
        state_.queued_paths.push_back(dir);
        state_.queued_enabled.push_back(true);
    } else {
        while (*cursor != L'\0') {
            std::filesystem::path name(cursor);
            state_.queued_paths.push_back(dir / name);
            state_.queued_enabled.push_back(true);
            cursor += name.wstring().size() + 1;
        }
    }
    if (was_empty && !state_.queued_paths.empty()) {
        state_.selected_queue_index = 0;
        queue_scroll_first_row_ = 0;
    }
    normalize_queue_selection_locked();
    request_repaint();
}

// Purpose: Open the Windows folder picker and append a selected folder to the queue.
// Inputs: None; user selection comes from the shell folder picker or GUI smoke-test environment.
// Outputs: Mutates queued paths and queues a repaint when a folder is selected.
void MainWindow::add_folder() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", smoke_path, smoke_path_capacity);
    if (smoke_length > 0 && smoke_length < smoke_path_capacity) {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        state_.queued_paths.emplace_back(smoke_path);
        state_.queued_enabled.push_back(true);
        if (was_empty) {
            state_.selected_queue_index = 0;
            queue_scroll_first_row_ = 0;
        }
        normalize_queue_selection_locked();
        state_.status = "Smoke folder added";
        request_repaint();
        return;
    }

    BROWSEINFOW browse{};
    browse.hwndOwner = hwnd_;
    browse.lpszTitle = L"Add folder to SuperZip";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browse);
    if (pidl == nullptr) {
        return;
    }
    wchar_t path[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok || path[0] == L'\0') {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        const bool was_empty = state_.queued_paths.empty();
        state_.queued_paths.emplace_back(path);
        state_.queued_enabled.push_back(true);
        if (was_empty) {
            state_.selected_queue_index = 0;
            queue_scroll_first_row_ = 0;
        }
        normalize_queue_selection_locked();
    }
    request_repaint();
}

// Purpose: Remove every queued path and reset row selection state.
// Inputs: None.
// Outputs: Mutates queue state and queues a repaint.
void MainWindow::clear_queue() {
    {
        std::lock_guard lock(mutex_);
        state_.queued_paths.clear();
        state_.queued_enabled.clear();
        state_.selected_queue_index = -1;
        queue_scroll_first_row_ = 0;
        queue_wheel_delta_remainder_ = 0;
    }
    request_repaint();
}

void MainWindow::clear_history() {
    {
        std::lock_guard lock(mutex_);
        state_.history.clear();
    }
    request_repaint();
}

// Purpose: Open the Windows folder picker for destination selection.
// Inputs: None; user selection comes from the shell dialog or smoke-test environment override.
// Outputs: Updates the destination directory and queues a repaint when selected.
void MainWindow::choose_destination() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_DESTINATION", smoke_path, smoke_path_capacity);
    if (smoke_length > 0 && smoke_length < smoke_path_capacity) {
        std::lock_guard lock(mutex_);
        state_.destination_directory = smoke_path;
        state_.status = "Destination selected";
        request_repaint();
        return;
    }

    BROWSEINFOW browse{};
    browse.hwndOwner = hwnd_;
    browse.lpszTitle = L"Choose SuperZip destination";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&browse);
    if (pidl == nullptr) {
        return;
    }
    wchar_t path[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok || path[0] == L'\0') {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        state_.destination_directory = path;
        state_.status = "Destination selected";
    }
    request_repaint();
}

// Purpose: Advance the compression level selection by one visible option.
// Inputs: None.
// Outputs: Updates the synchronized UI state and queues repaint.
void MainWindow::cycle_compression_level() {
    {
        std::lock_guard lock(mutex_);
        state_.compression_level_index = (state_.compression_level_index + 1) % 5;
        state_.status = "Compression level changed";
    }
    request_repaint();
}

// Purpose: Start a background compression job from the current GUI queue.
// Inputs: None; reads queued paths and compression options from synchronized UI state.
// Outputs: Launches a worker job, updates progress/history/status, or requests repaint when the queue is empty.
void MainWindow::start_compress() {
    std::vector<std::filesystem::path> sources;
    bool gpu_required = true;
    bool integrity = false;
    bool defender = false;
    bool verify_after_write = false;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
    ArchiveFormat archive_format = ArchiveFormat::SuperZip;
    std::filesystem::path output;
    {
        std::lock_guard lock(mutex_);
        normalize_queue_selection_locked();
        for (std::size_t i = 0; i < state_.queued_paths.size(); ++i) {
            if (state_.queued_enabled[i]) {
                sources.push_back(state_.queued_paths[i]);
            }
        }
        gpu_required = state_.gpu_required;
        integrity = state_.integrity_hash_opt_in;
        defender = state_.defender_scan_opt_in;
        verify_after_write = state_.verify_after_write_opt_in;
        block_size = compression_block_size_bytes(state_.compression_block_size_index);
        compression_level = compression_level_value(state_.compression_level_index);
        archive_format = compression_format_value(state_.compression_format_index);
        output = compression_output_path_for(state_);
    }
    if (sources.empty()) {
        {
            std::lock_guard lock(mutex_);
            state_.status = "Add files or folders before starting compression";
        }
        request_repaint();
        return;
    }
    run_job(
        [this, sources, output, archive_format, gpu_required, integrity, defender, verify_after_write, block_size,
         compression_level] {
            auto progress_callback = [this](const ProgressSnapshot& snapshot) { publish_progress_snapshot(snapshot); };
            const auto stats = compress_gui_archive(sources, output, archive_format, gpu_required, verify_after_write,
                                                    block_size, compression_level, progress_callback);
            std::ostringstream line;
            line << "Compressed " << archive_format_info(archive_format).key << " to " << output.string() << " in "
                 << stats.seconds << "s";
            append_history_entry("Compress", output.filename().string(), line.str(), true);
            if (integrity) {
                const auto hash = hash_file(output, IntegrityMode::Sha256);
                append_history_entry("Security", output.filename().string(), "SHA-256 " + hash.hex_digest, true);
            }
            if (defender) {
                const auto scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
                append_history_entry("Security", output.filename().string(), defender_history_status("Defender", scan),
                                     !scan.attempted || scan.clean);
            }
        },
        "Compressing");
}

// Purpose: Detect whether Ask-before-overwriting needs user confirmation.
// Inputs: `destination` is the extraction root.
// Outputs: Returns true when the destination exists and is non-empty or cannot be proven empty.

}  // namespace superzip::app
