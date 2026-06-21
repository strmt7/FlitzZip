#include "app/main_window_impl.hpp"

#include <limits>

namespace superzip::app {

// Purpose: Return a resource-bounded display size for one Queue entry.
// Inputs: `path` is a queued file or folder path that may be inaccessible.
// Outputs: Returns file size immediately, `...` for pending folder size, or the cached folder size.
std::wstring MainWindow::queue_entry_size_text(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        return entry_size_text(path);
    }

    const std::wstring key = path.wstring();
    {
        std::lock_guard lock(folder_size_mutex_);
        if (const auto found = folder_size_cache_.find(key); found != folder_size_cache_.end()) {
            if (found->second.state == FolderSizeCacheEntry::State::Ready) {
                return widen(human_bytes(static_cast<double>(found->second.bytes)));
            }
            if (found->second.state == FolderSizeCacheEntry::State::Failed) {
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
        const std::wstring key = path.wstring();
        if (folder_size_cache_.contains(key)) {
            return;
        }
        folder_size_cache_.emplace(key, FolderSizeCacheEntry{});
        folder_size_queue_.push_back(path);
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
            std::filesystem::path path;
            {
                std::unique_lock lock(folder_size_mutex_);
                folder_size_cv_.wait(lock, [this] { return folder_size_stop_.load() || !folder_size_queue_.empty(); });
                if (folder_size_stop_.load()) {
                    break;
                }
                path = std::move(folder_size_queue_.front());
                folder_size_queue_.pop_front();
            }

            const auto size = calculate_folder_size(path);
            {
                std::lock_guard lock(folder_size_mutex_);
                auto& entry = folder_size_cache_[path.wstring()];
                if (size.has_value()) {
                    entry.state = FolderSizeCacheEntry::State::Ready;
                    entry.bytes = *size;
                } else if (!folder_size_stop_.load()) {
                    entry.state = FolderSizeCacheEntry::State::Failed;
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
// Inputs: `path` is a directory path; `folder_size_stop_` can cancel traversal.
// Outputs: Returns total bytes, or empty when traversal is cancelled or inaccessible.
std::optional<std::uintmax_t> MainWindow::calculate_folder_size(const std::filesystem::path& path) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) {
        return std::nullopt;
    }
    std::uintmax_t total = 0;
    std::filesystem::recursive_directory_iterator iterator(
        path, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec) {
        return std::nullopt;
    }
    std::uint32_t entries_since_yield = 0;
    while (iterator != end) {
        if (folder_size_stop_.load()) {
            return std::nullopt;
        }
        if (iterator->is_regular_file(ec) && !ec) {
            const auto size = iterator->file_size(ec);
            if (!ec) {
                const auto room = std::numeric_limits<std::uintmax_t>::max() - total;
                total += std::min(room, size);
            }
        }
        ec.clear();
        iterator.increment(ec);
        if (ec) {
            ec.clear();
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
