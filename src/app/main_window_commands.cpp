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

namespace {

template <typename Interface> class ComPtr final {
  public:
    // Purpose: Create an empty COM pointer wrapper.
    // Inputs: None.
    // Outputs: Stores a null interface pointer.
    ComPtr() = default;

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    // Purpose: Release the owned COM interface pointer.
    // Inputs: None.
    // Outputs: Calls `Release` on the stored pointer when present.
    ~ComPtr() {
        reset();
    }

    // Purpose: Prepare this wrapper as an output parameter for COM APIs.
    // Inputs: None.
    // Outputs: Releases any existing pointer and returns storage for a new pointer.
    Interface** put() noexcept {
        reset();
        return &value_;
    }

    // Purpose: Access the owned COM interface pointer.
    // Inputs: None.
    // Outputs: Returns the raw interface pointer without transferring ownership.
    [[nodiscard]] Interface* get() const noexcept {
        return value_;
    }

    // Purpose: Access COM interface members.
    // Inputs: None.
    // Outputs: Returns the raw interface pointer for member calls.
    [[nodiscard]] Interface* operator->() const noexcept {
        return value_;
    }

  private:
    // Purpose: Release the currently stored COM pointer.
    // Inputs: None.
    // Outputs: Clears the wrapper after calling `Release` when needed.
    void reset() noexcept {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

    Interface* value_ = nullptr;
};

class CoTaskMemWideString final {
  public:
    CoTaskMemWideString(const CoTaskMemWideString&) = delete;
    CoTaskMemWideString& operator=(const CoTaskMemWideString&) = delete;

    // Purpose: Create an empty task-allocator string wrapper.
    // Inputs: None.
    // Outputs: Stores a null string pointer.
    CoTaskMemWideString() = default;

    // Purpose: Release a shell string allocated with `CoTaskMemAlloc`.
    // Inputs: None.
    // Outputs: Calls `CoTaskMemFree` for a non-null pointer.
    ~CoTaskMemWideString() {
        CoTaskMemFree(value_);
    }

    // Purpose: Prepare this wrapper as an output parameter for shell APIs.
    // Inputs: None.
    // Outputs: Frees the current string and returns storage for a new pointer.
    PWSTR* put() noexcept {
        CoTaskMemFree(value_);
        value_ = nullptr;
        return &value_;
    }

    // Purpose: Access the stored shell string.
    // Inputs: None.
    // Outputs: Returns the UTF-16 pointer without transferring ownership.
    [[nodiscard]] PWSTR get() const noexcept {
        return value_;
    }

  private:
    PWSTR value_ = nullptr;
};

// Purpose: Format an HRESULT for concise diagnostics.
// Inputs: `result` is the HRESULT returned by a Win32 or COM API.
// Outputs: Returns a lowercase hexadecimal HRESULT string.
std::string hresult_text(HRESULT result) {
    std::ostringstream text;
    text << "0x" << std::hex << static_cast<unsigned long>(result);
    return text.str();
}

// Purpose: Convert a failed HRESULT into an exception with operation context.
// Inputs: `result` is an HRESULT and `operation` names the failed operation.
// Outputs: Throws `std::runtime_error` only when `result` is a failure code.
void require_succeeded(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed (" + hresult_text(result) + ")");
    }
}

// Purpose: Parse GUI smoke file-selection paths from an environment override.
// Inputs: None; reads `SUPERZIP_GUI_SMOKE_FILE_SELECTION` from the current process.
// Outputs: Returns semicolon-separated filesystem paths, or an empty vector when the override is absent.
std::vector<std::filesystem::path> smoke_file_selection_paths() {
    wchar_t smoke_paths[32768]{};
    constexpr DWORD smoke_paths_capacity = static_cast<DWORD>(sizeof(smoke_paths) / sizeof(smoke_paths[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FILE_SELECTION", smoke_paths, smoke_paths_capacity);
    std::vector<std::filesystem::path> paths;
    if (smoke_length == 0 || smoke_length >= smoke_paths_capacity) {
        return paths;
    }
    std::wstring_view encoded(smoke_paths, smoke_length);
    std::size_t start = 0;
    while (start <= encoded.size()) {
        const std::size_t end = encoded.find(L';', start);
        const auto part = encoded.substr(start, end == std::wstring_view::npos ? std::wstring_view::npos : end - start);
        if (!part.empty()) {
            paths.emplace_back(std::wstring(part));
        }
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return paths;
}

// Purpose: Read a single GUI smoke folder-selection path from an environment override.
// Inputs: None; reads `SUPERZIP_GUI_SMOKE_FOLDER_SELECTION` from the current process.
// Outputs: Returns the selected folder path, or an empty path when the override is absent.
std::filesystem::path smoke_folder_selection_path() {
    wchar_t smoke_path[32768]{};
    constexpr DWORD smoke_path_capacity = static_cast<DWORD>(sizeof(smoke_path) / sizeof(smoke_path[0]));
    const DWORD smoke_length =
        GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_FOLDER_SELECTION", smoke_path, smoke_path_capacity);
    if (smoke_length == 0 || smoke_length >= smoke_path_capacity) {
        return {};
    }
    return std::filesystem::path(smoke_path);
}

// Purpose: Return the filesystem path represented by one shell item.
// Inputs: `item` is a filesystem shell item returned by `IFileOpenDialog`.
// Outputs: Returns the absolute filesystem path or throws when the shell cannot provide one.
std::filesystem::path shell_item_filesystem_path(IShellItem* item) {
    if (item == nullptr) {
        return {};
    }
    CoTaskMemWideString path;
    require_succeeded(item->GetDisplayName(SIGDN_FILESYSPATH, path.put()), "Shell item path lookup");
    return path.get() == nullptr ? std::filesystem::path{} : std::filesystem::path(path.get());
}

// Purpose: Open the modern Windows shell picker for multi-file selection.
// Inputs: `owner` is the SuperZip window owner for modality.
// Outputs: Returns every selected filesystem file path, or an empty vector on user cancel.
std::vector<std::filesystem::path> selected_files_from_dialog(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    require_succeeded(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())),
                      "File picker creation");
    DWORD options = 0;
    require_succeeded(dialog->GetOptions(&options), "File picker option query");
    require_succeeded(dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                         FOS_PATHMUSTEXIST),
                      "File picker option update");
    dialog->SetTitle(L"Add files to SuperZip");
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    require_succeeded(shown, "File picker display");

    ComPtr<IShellItemArray> items;
    require_succeeded(dialog->GetResults(items.put()), "File picker result query");
    DWORD count = 0;
    require_succeeded(items->GetCount(&count), "File picker result count");
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        ComPtr<IShellItem> item;
        require_succeeded(items->GetItemAt(index, item.put()), "File picker item lookup");
        auto path = shell_item_filesystem_path(item.get());
        if (!path.empty()) {
            paths.emplace_back(std::move(path));
        }
    }
    return paths;
}

// Purpose: Open the modern Windows shell picker for folder selection.
// Inputs: `owner` is the SuperZip window owner for modality and `title` is the dialog caption.
// Outputs: Returns the selected filesystem folder path, or an empty path on user cancel.
std::filesystem::path selected_folder_from_dialog(HWND owner, PCWSTR title) {
    ComPtr<IFileOpenDialog> dialog;
    require_succeeded(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())),
                      "Folder picker creation");
    DWORD options = 0;
    require_succeeded(dialog->GetOptions(&options), "Folder picker option query");
    require_succeeded(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST),
                      "Folder picker option update");
    dialog->SetTitle(title);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    require_succeeded(shown, "Folder picker display");

    ComPtr<IShellItem> item;
    require_succeeded(dialog->GetResult(item.put()), "Folder picker result query");
    return shell_item_filesystem_path(item.get());
}

}  // namespace

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
// Inputs: None; user selection comes from the shell picker or GUI smoke-test environment.
// Outputs: Mutates queued paths and queues a repaint when files are selected.
void MainWindow::add_files() {
    auto smoke_paths = smoke_file_selection_paths();
    if (!smoke_paths.empty()) {
        (void)append_queued_paths(std::move(smoke_paths), "Smoke files added");
        return;
    }

    try {
        auto selected = selected_files_from_dialog(hwnd_);
        if (!selected.empty()) {
            (void)append_queued_paths(std::move(selected), "Files added");
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Add files failed: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "File picker failed");
    }
}

// Purpose: Open the Windows folder picker and append a selected folder to the queue.
// Inputs: None; user selection comes from the shell picker or GUI smoke-test environment.
// Outputs: Mutates queued paths and queues a repaint when a folder is selected.
void MainWindow::add_folder() {
    auto smoke_path = smoke_folder_selection_path();
    if (!smoke_path.empty()) {
        std::vector<std::filesystem::path> paths;
        paths.emplace_back(std::move(smoke_path));
        (void)append_queued_paths(std::move(paths), "Smoke folder added");
        return;
    }

    try {
        auto selected = selected_folder_from_dialog(hwnd_, L"Add folder to SuperZip");
        if (!selected.empty()) {
            std::vector<std::filesystem::path> paths;
            paths.emplace_back(std::move(selected));
            (void)append_queued_paths(std::move(paths), "Folder added");
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Add folder failed: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Folder picker failed");
    }
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
        state_.selected_history_index = -1;
        history_scroll_first_row_ = 0;
        history_wheel_delta_remainder_ = 0;
    }
    request_repaint();
}

// Purpose: Open the Windows folder picker for destination selection.
// Inputs: None; user selection comes from the shell picker or smoke-test environment override.
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

    try {
        auto selected = selected_folder_from_dialog(hwnd_, L"Choose SuperZip destination");
        if (!selected.empty()) {
            std::lock_guard lock(mutex_);
            state_.destination_directory = std::move(selected);
            state_.status = "Destination selected";
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Destination selection failed: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Destination picker failed");
    }
    request_repaint();
}

// Purpose: Open Windows Explorer with the current user's SuperZip log file selected.
// Inputs: None; resolves and creates the per-user log file path.
// Outputs: Launches Explorer or records a warning status when the shell action fails.
void MainWindow::open_log_file_location() {
    try {
        ensure_app_storage();
        const auto path = log_file_path();
        wchar_t suppress_shell_open[8]{};
        constexpr DWORD suppress_capacity =
            static_cast<DWORD>(sizeof(suppress_shell_open) / sizeof(suppress_shell_open[0]));
        const DWORD suppress_length =
            GetEnvironmentVariableW(L"SUPERZIP_GUI_SMOKE_SUPPRESS_SHELL_OPEN", suppress_shell_open, suppress_capacity);
        if (!(suppress_length == 1 && suppress_shell_open[0] == L'1')) {
            const std::wstring arguments = L"/select,\"" + path.wstring() + L"\"";
            const auto result = reinterpret_cast<INT_PTR>(
                ShellExecuteW(hwnd_, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                throw ArchiveError("Explorer could not open the SuperZip log file location");
            }
        }
        {
            std::lock_guard lock(mutex_);
            state_.status = "Log file location opened";
        }
        append_log_entry(LogSeverity::Debug, "Log file location opened");
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            state_.status = std::string("Open log file failed: ") + error.what();
        }
        append_log_entry(LogSeverity::Warning, "Open log file failed");
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
            append_history_entry("Compress", output.filename().string(), output.string(), line.str(), true);
            if (integrity) {
                const auto hash = hash_path(output, IntegrityMode::Sha256);
                append_history_entry("Security", output.filename().string(), output.string(),
                                     integrity_history_status("Archive", hash), true);
            }
            if (defender) {
                const auto scan = scan_with_windows_defender(output, DefenderScanMode::FullPath);
                append_history_entry("Security", output.filename().string(), output.string(),
                                     defender_history_status("Defender", scan), !scan.attempted || scan.clean);
            }
        },
        "Compressing");
}

// Purpose: Detect whether Ask-before-overwriting needs user confirmation.
// Inputs: `destination` is the extraction root.
// Outputs: Returns true when the destination exists and is non-empty or cannot be proven empty.

}  // namespace superzip::app
