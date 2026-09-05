#include "app/main_window_impl.hpp"

namespace superzip::app {

// Purpose: Format a queued row's type without I/O; inputs: queued path; outputs: cached metadata plus extension label.
std::wstring MainWindow::queue_entry_type_text(const std::filesystem::path& path) const {
    switch (queue_metadata_.get(path).kind) {
    case QueueFileKind::Directory:
        return L"Folder";
    case QueueFileKind::File:
        if (archive_format_info(detect_archive_format_by_extension(path)).can_extract) {
            return L"Archive";
        }
        return L"File";
    case QueueFileKind::Missing:
        return L"Missing";
    case QueueFileKind::Unavailable:
        return L"Unavailable";
    case QueueFileKind::Pending:
        return L"...";
    }
    return L"Unavailable";
}

// Purpose: Collect selected candidates without filesystem I/O on the UI thread.
// Inputs: state is a stable queue snapshot; metadata may be stale or awaiting its first asynchronous query.
// Outputs: Returns cached regular-file paths in queue order; archive jobs must independently open and validate inputs.
std::vector<std::filesystem::path> MainWindow::selected_extract_archive_paths(const UiState& state) const {
    return queue_archive_candidates(state.queued_paths, state.queued_enabled, queue_metadata_);
}

}  // namespace superzip::app
