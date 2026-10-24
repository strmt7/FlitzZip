#include "app/main_window_impl.hpp"

#include "app/folder_size.hpp"

namespace flitzzip::app {

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

    return enqueue_folder_size_if_needed(path) ? L"..." : L"--";
}

// Purpose: Enqueue one folder-size scan when the Queue first renders a folder.
// Inputs: `path` is a queued directory path.
// Outputs: True for admitted work; startup failure retires its queued task as Failed until normal invalidation.
bool MainWindow::enqueue_folder_size_if_needed(const std::filesystem::path& path) {
    std::shared_ptr<FolderSizeTask> task;
    try {
        {
            std::lock_guard lock(folder_size_mutex_);
            if (!folder_size_cache_.enqueue(path)) {
                return folder_size_cache_.find(path)->state != FolderSizeTask::State::Failed;
            }
            task = folder_size_cache_.find(path);
        }
        start_folder_size_worker();
    } catch (...) {
        // Optional display work must not escape through native drawing. Existing storage records startup refusal;
        // failed allocation before admission has no task to retain and may recover on a later display request.
        std::lock_guard lock(folder_size_mutex_);
        static_cast<void>(folder_size_cache_.fail_queued(task));
        return false;
    }
    folder_size_cv_.notify_one();
    return true;
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
    folder_size_worker_ = thread_launcher_.launch([this] {
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

            std::optional<std::uintmax_t> size;
            try {
                size = measure_folder_size(task->path, folder_size_stop_, task->cancelled);
            } catch (const std::exception&) {
                // Optional display metadata must not terminate the process if traversal allocation fails.
                size = std::nullopt;
            }
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

}  // namespace flitzzip::app
