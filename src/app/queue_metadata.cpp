#include "app/queue_metadata.hpp"
#include "core/archive_format.hpp"

#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace superzip::app {

// Purpose: Query metadata without opening archive content; inputs: local queued path; outputs: type and optional size.
QueueFileMetadata read_queue_file_metadata(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        return {QueueFileKind::Missing, {}};
    }
    if (ec) {
        return {QueueFileKind::Unavailable, {}};
    }
    if (std::filesystem::is_directory(status)) {
        return {QueueFileKind::Directory, {}};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {QueueFileKind::Unavailable, {}};
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    return {QueueFileKind::File, ec ? std::nullopt : std::optional<std::uintmax_t>(bytes)};
}

// Purpose: Store worker callbacks; inputs: nonempty reader and optional notifier; outputs: lazy, idle cache.
QueueMetadataCache::QueueMetadataCache(Reader reader, Notify notify)
    : reader_(std::move(reader)), notify_(std::move(notify)) {
    if (!reader_) {
        throw std::invalid_argument("Queue metadata requires a reader");
    }
}

// Purpose: Release owned work; inputs: none; outputs: no callback can outlive the cache.
QueueMetadataCache::~QueueMetadataCache() {
    stop();
}

// Purpose: Retain only current row lifetimes; inputs: bounded queue paths; outputs: drops removed and pending work.
void QueueMetadataCache::synchronize(std::span<const std::filesystem::path> paths) {
    if (paths.size() > kMaxQueueItems) {
        throw std::length_error("Queue metadata capacity exceeded");
    }
    std::lock_guard lock(mutex_);
    if (stopped_) {
        return;
    }
    decltype(entries_) next;
    next.reserve(paths.size());
    for (const auto& path : paths) {
        if (const auto old = entries_.find(path); old != entries_.end()) {
            next.emplace(path, old->second);
        } else if (!next.contains(path)) {
            auto entry = std::make_shared<Entry>();
            entry->path = path;
            next.emplace(path, std::move(entry));
        }
    }
    entries_.swap(next);
    std::erase_if(pending_, [this](const auto& entry) { return !entries_.contains(entry->path); });
}

// Purpose: Return cached metadata and schedule expired reads; inputs: queued path/time; outputs: never reads disk here.
QueueFileMetadata QueueMetadataCache::get(const std::filesystem::path& path, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(path);
    if (stopped_ || found == entries_.end()) {
        return {};
    }
    auto& entry = found->second;
    constexpr auto refresh_interval = std::chrono::seconds(2);
    if (!entry->pending &&
        (entry->value.kind == QueueFileKind::Pending || now - entry->completed >= refresh_interval)) {
        if (!worker_.joinable()) {
            worker_ = std::thread([this] { run(); });
        }
        pending_.push_back(entry);
        entry->pending = true;
        ready_.notify_one();
    }
    return entry->value;
}

// Purpose: Cancel pending reads and join the sole worker; inputs: owner thread; outputs: prevents later callbacks.
void QueueMetadataCache::stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        pending_.clear();
    }
    ready_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

// Purpose: Resolve display metadata off-thread; inputs: pending entries; outputs: current lifetime results and repaint.
void QueueMetadataCache::run() {
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        std::shared_ptr<Entry> entry;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
            if (stopped_) {
                break;
            }
            entry = std::move(pending_.front());
            pending_.pop_front();
        }
        QueueFileMetadata value{QueueFileKind::Unavailable, {}};
        try {
            value = reader_(entry->path);
        } catch (...) {
            // A failed display read must not terminate the application or authorize a job.
        }
        bool kind_changed = false;
        {
            std::lock_guard lock(mutex_);
            const auto current = entries_.find(entry->path);
            if (stopped_ || current == entries_.end() || current->second != entry) {
                continue;
            }
            kind_changed = entry->value.kind != value.kind;
            entry->value = value;
            entry->completed = Clock::now();
            entry->pending = false;
        }
        if (notify_) {
            notify_(entry->path, kind_changed);
        }
    }
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

// Purpose: Filter UI candidates by tick, extension, and cached type without blocking on the filesystem.
// Inputs: paths and enabled are a stable UI snapshot; cache is display-only and may return stale or pending metadata.
// Outputs: Returns cached regular-file candidates in order; callers must independently open and validate job inputs.
std::vector<std::filesystem::path> queue_archive_candidates(std::span<const std::filesystem::path> paths,
                                                            const std::vector<bool>& enabled,
                                                            QueueMetadataCache& cache) {
    std::vector<std::filesystem::path> archives;
    archives.reserve(paths.size());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        if (index < enabled.size() && !enabled[index]) {
            continue;
        }
        const auto& path = paths[index];
        if (archive_format_info(detect_archive_format_by_extension(path)).can_extract &&
            cache.get(path).kind == QueueFileKind::File) {
            archives.push_back(path);
        }
    }
    return archives;
}

}  // namespace superzip::app
