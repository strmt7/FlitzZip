#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace flitzzip::app {

// Purpose: Bind one folder-size result to one queue lifetime, including an in-flight scan.
// Inputs: The owner serializes state/bytes access; workers may read cancelled concurrently.
// Outputs: Retains the path and cooperative cancellation flag until all task holders release it.
struct FolderSizeTask {
    enum class State { Pending, Ready, Failed };
    // Purpose: Acquire the final path before publishing a task or mutating cache ownership.
    // Inputs: Borrowed folder path, copied into this task.
    // Outputs: Fully initialized task; allocation failure propagates without default-path noexcept construction.
    explicit FolderSizeTask(const std::filesystem::path& folder) : path(folder) {}

    std::filesystem::path path;
    std::atomic_bool cancelled = false;
    State state = State::Pending;
    std::uintmax_t bytes = 0;
};

// Purpose: Own queued folder metadata without retaining removed rows or publishing obsolete results.
// Inputs: The caller holds one mutex for every method and for result-state reads.
// Outputs: Issues task handles whose cancellation flag remains usable outside that mutex.
class FolderSizeCache {
    using PathKey = std::filesystem::path::string_type;

  public:
    // Purpose: Find a row without allocating; inputs: borrowed native path key; outputs: shared handle or null.
    [[nodiscard]] std::shared_ptr<FolderSizeTask> find(const std::filesystem::path& path) const noexcept {
        const auto found = entries_.find(path.native());
        return found == entries_.end() ? nullptr : found->second;
    }

    // Purpose: Queue an uncached path; inputs: directory path; outputs: true only for a new task.
    bool enqueue(const std::filesystem::path& path) {
        const auto& key = path.native();
        if (entries_.contains(key)) {
            return false;
        }
        auto task = std::make_shared<FolderSizeTask>(path);
        pending_.push_back(task);
        try {
            entries_.emplace(key, task);
        } catch (...) {
            pending_.pop_back();
            throw;
        }
        return true;
    }

    // Purpose: Query work availability; inputs: none; outputs: whether a task is waiting.
    [[nodiscard]] bool has_pending() const {
        return !pending_.empty();
    }

    // Purpose: Take the next scan; inputs: none; outputs: task handle or null for an empty queue.
    [[nodiscard]] std::shared_ptr<FolderSizeTask> take_next() {
        if (pending_.empty()) {
            return nullptr;
        }
        auto task = std::move(pending_.front());
        pending_.pop_front();
        return task;
    }

    // Purpose: Publish without allocating; inputs: issued task and optional byte count; outputs: false if removed.
    bool complete(const std::shared_ptr<FolderSizeTask>& task, std::optional<std::uintmax_t> bytes) noexcept {
        if (!task || task->cancelled.load() || find(task->path) != task) {
            return false;
        }
        task->state = bytes ? FolderSizeTask::State::Ready : FolderSizeTask::State::Failed;
        task->bytes = bytes.value_or(0);
        return true;
    }

    // Purpose: Retire work whose native worker admission failed without scanning the normal completion path.
    // Inputs: Exact current queued task, with no active worker holding it; caller holds the cache mutex.
    // Outputs: Removes only that pending owner and records Failed without allocation; rejects issued/stale tasks.
    bool fail_queued(const std::shared_ptr<FolderSizeTask>& task) noexcept {
        if (!task || task->cancelled.load() || find(task->path) != task ||
            task->state != FolderSizeTask::State::Pending) {
            return false;
        }
        const auto queued = std::find(pending_.begin(), pending_.end(), task);
        if (queued == pending_.end()) {
            return false;
        }
        pending_.erase(queued);
        task->state = FolderSizeTask::State::Failed;
        task->bytes = 0U;
        return true;
    }

    // Purpose: Invalidate without allocating; inputs: former folder key; outputs: cancels pending/in-flight stale
    // sizes.
    void invalidate(const std::filesystem::path& path) noexcept {
        if (const auto task = find(path)) {
            task->cancelled.store(true);
            entries_.erase(path.native());
            std::erase_if(pending_, [](const auto& pending) { return pending->cancelled.load(); });
        }
    }

    // Purpose: Drop removed rows and their pending scans; inputs: current queue paths; outputs: cancels stale tasks.
    void retain(std::span<const std::filesystem::path> paths) {
        std::unordered_set<PathKey> retained;
        retained.reserve(paths.size());
        for (const auto& path : paths) {
            retained.insert(path.native());
        }
        std::erase_if(entries_, [&retained](const auto& entry) {
            if (retained.contains(entry.first)) {
                return false;
            }
            entry.second->cancelled.store(true);
            return true;
        });
        std::erase_if(pending_, [](const auto& task) { return task->cancelled.load(); });
    }

  private:
    std::unordered_map<PathKey, std::shared_ptr<FolderSizeTask>> entries_;
    std::deque<std::shared_ptr<FolderSizeTask>> pending_;
};

}  // namespace flitzzip::app
