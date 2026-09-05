#include "app/main_window_impl.hpp"

#include <limits>

namespace superzip::app {

// Purpose: Return a resource-bounded display size for one Queue entry.
// Inputs: `path` is a queued file or folder path that may be inaccessible.
// Outputs: Returns cached bytes, `...` while pending, or `--` when unavailable; performs no filesystem query.
std::wstring MainWindow::queue_entry_size_text(const std::filesystem::path& path) {
    const auto metadata = queue_metadata_.get(path);
    if (metadata.kind == QueueFileKind::Pending) {
        return L"...";
    }
    if (metadata.kind != QueueFileKind::Directory) {
        return metadata.bytes ? widen(human_bytes(static_cast<double>(*metadata.bytes))) : L"--";
    }

    {
        std::lock_guard lock(folder_size_mutex_);
        if (const auto found = folder_size_cache_.find(path)) {
            if (found->state == FolderSizeTask::State::Ready) {
                return widen(human_bytes(static_cast<double>(found->bytes)));
            }
            if (found->state == FolderSizeTask::State::Failed) {
                return L"--";
            }
            return L"...";
        }
    }

    enqueue_folder_size_if_needed(path);
    return L"...";
}

// Purpose: Enqueue one folder-size scan when the Queue first renders a folder.
// Inputs: `path` is a queued directory path.
// Outputs: Starts the low-priority folder-size worker if needed and records pending cache state.
void MainWindow::enqueue_folder_size_if_needed(const std::filesystem::path& path) {
    {
        std::lock_guard lock(folder_size_mutex_);
        if (!folder_size_cache_.enqueue(path)) {
            return;
        }
    }
    start_folder_size_worker();
    folder_size_cv_.notify_one();
}

// Purpose: Start the single folder-size background worker.
// Inputs: None.
// Outputs: Creates the worker thread if it is not already running.
void MainWindow::start_folder_size_worker() {
    std::lock_guard lock(folder_size_mutex_);
    if (folder_size_worker_.joinable()) {
        return;
    }
    folder_size_stop_.store(false);
    folder_size_worker_ = std::thread([this] {
        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        (void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        while (!folder_size_stop_.load()) {
            std::shared_ptr<FolderSizeTask> task;
            {
                std::unique_lock lock(folder_size_mutex_);
                folder_size_cv_.wait(lock,
                                     [this] { return folder_size_stop_.load() || folder_size_cache_.has_pending(); });
                if (folder_size_stop_.load()) {
                    break;
                }
                task = folder_size_cache_.take_next();
            }

            const auto size = calculate_folder_size(*task);
            {
                std::lock_guard lock(folder_size_mutex_);
                if (folder_size_stop_.load() || !folder_size_cache_.complete(task, size)) {
                    continue;
                }
            }
            request_repaint();
        }
        (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    });
}

// Purpose: Stop the folder-size background worker.
// Inputs: None.
// Outputs: Requests cooperative stop and joins the thread.
void MainWindow::stop_folder_size_worker() {
    folder_size_stop_.store(true);
    folder_size_cv_.notify_all();
    if (folder_size_worker_.joinable()) {
        folder_size_worker_.join();
    }
}

// Purpose: Calculate one folder's byte size from filesystem metadata only.
// Inputs: `task` owns a directory path and cancellation flag; window shutdown also cancels traversal.
// Outputs: Returns total bytes, or empty when traversal is cancelled or inaccessible.
std::optional<std::uintmax_t> MainWindow::calculate_folder_size(const FolderSizeTask& task) const {
    if (folder_size_stop_.load() || task.cancelled.load()) {
        return std::nullopt;
    }
    const auto& path = task.path;
    if (!is_supported_local_drop_path(path)) {
        return std::nullopt;
    }
    const DWORD root_attributes = GetFileAttributesW(path.c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES || (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::nullopt;
    }
    std::error_code ec;
    std::uintmax_t total = 0;
    std::size_t entry_count = 0U;
    const auto deadline = std::chrono::steady_clock::now() + kMaxFolderSizeDuration;
    std::filesystem::recursive_directory_iterator iterator(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        return std::nullopt;
    }
    std::uint32_t entries_since_yield = 0;
    while (iterator != end) {
        if (folder_size_stop_.load() || task.cancelled.load() || std::chrono::steady_clock::now() >= deadline ||
            entry_count >= kMaxFolderSizeEntries || static_cast<std::size_t>(iterator.depth()) >= kMaxFolderSizeDepth) {
            return std::nullopt;
        }
        ++entry_count;
        const DWORD attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return std::nullopt;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
                iterator.disable_recursion_pending();
            }
        } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
            const auto size = iterator->file_size(ec);
            if (ec || size > std::numeric_limits<std::uintmax_t>::max() - total) {
                return std::nullopt;
            }
            total += size;
        }
        ec.clear();
        iterator.increment(ec);
        if (ec) {
            return std::nullopt;
        }
        ++entries_since_yield;
        if (entries_since_yield >= 512U) {
            entries_since_yield = 0;
            Sleep(1);
        }
    }
    return total;
}

}  // namespace superzip::app
