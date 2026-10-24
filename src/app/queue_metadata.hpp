#pragma once

#include "app/input_limits.hpp"
#include "app/background_job.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flitzzip::app {

// Purpose: Describe display metadata, never authority to open or extract a queued source.
// Inputs: Classification comes from a worker query, not an archive-content probe.
// Outputs: Distinguishes pending, missing, inaccessible, regular-file, and directory rows.
enum class QueueFileKind { Pending, Missing, Unavailable, File, Directory };

// Purpose: Hold one display snapshot; inputs: observed type and optional byte size; outputs: no file ownership or
// authority.
struct QueueFileMetadata {
    QueueFileKind kind = QueueFileKind::Pending;
    std::optional<std::uintmax_t> bytes;
};

// Purpose: Read type and size on a worker; inputs: queued path; outputs: metadata only, with explicit errors.
QueueFileMetadata read_queue_file_metadata(const std::filesystem::path& path);

// Purpose: Keep display queries off the UI thread with one lazy worker and queue-lifetime ownership.
// Inputs: Reader may block or throw; nonthrowing Notify runs outside the lock after a current result is published.
// Outputs: Caches at most kMaxQueueItems paths; stale values remain visible while their refresh is pending.
class QueueMetadataCache final {
  public:
    using Clock = std::chrono::steady_clock;
    using Reader = std::function<QueueFileMetadata(const std::filesystem::path&)>;
    using Notify = std::function<void(const std::filesystem::path&, bool)>;

    // Purpose: Configure a lazy cache without changing reader or notification semantics.
    // Inputs: Reader, optional Notify, and borrowed launcher/resource that outlive cache destruction and join.
    // Outputs: No I/O; refused optional admission stays Unavailable until removal/re-addition.
    explicit QueueMetadataCache(Reader reader, Notify notify = {},
                                const JobThreadLauncher& launcher = JobThreadLauncher::native(),
                                std::pmr::memory_resource& pending_resource = *std::pmr::new_delete_resource());
    QueueMetadataCache(const QueueMetadataCache&) = delete;
    QueueMetadataCache& operator=(const QueueMetadataCache&) = delete;

    // Purpose: Stop owned work; inputs: none; outputs: joins the worker, including any current OS metadata query.
    ~QueueMetadataCache();

    // Purpose: Match queue membership; inputs: at most kMaxQueueItems paths; outputs: cancels obsolete publications.
    void synchronize(std::span<const std::filesystem::path> paths);

    // Purpose: Read cached metadata and admit optional refresh without filesystem I/O on the caller.
    // Inputs: Queued path and monotonic time. Outputs: Snapshot; failed admission is Unavailable until re-add.
    [[nodiscard]] QueueFileMetadata get(const std::filesystem::path& path, Clock::time_point now = Clock::now());

    // Purpose: End the worker before its callback owner is destroyed; inputs: none; outputs: idempotent owner-thread
    // join.
    void stop();

  private:
    struct Entry {
        std::filesystem::path path;
        QueueFileMetadata value;
        Clock::time_point completed{};
        bool pending = false;
        bool admission_failed = false;
    };

    // Purpose: Consume queued metadata reads; inputs: owned synchronized queue; outputs: publishes current results
    // only.
    void run();

    const JobThreadLauncher& launcher_;
    Reader reader_;
    Notify notify_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::unordered_map<std::filesystem::path, std::shared_ptr<Entry>> entries_;
    std::pmr::deque<std::shared_ptr<Entry>> pending_;
    std::thread worker_;
    bool stopped_ = false;
};

// Purpose: Select archive candidates without I/O; inputs: paths, ticks, display cache; outputs: cached file paths in
// order.
std::vector<std::filesystem::path> queue_archive_candidates(std::span<const std::filesystem::path> paths,
                                                            const std::vector<bool>& enabled,
                                                            QueueMetadataCache& cache);

}  // namespace flitzzip::app
