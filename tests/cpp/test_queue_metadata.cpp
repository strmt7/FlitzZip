#include "app/queue_metadata.hpp"
#include "test_util.hpp"

#include <atomic>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <new>
#include <system_error>

using flitzzip::app::QueueFileKind;
using flitzzip::app::QueueFileMetadata;
using flitzzip::app::QueueMetadataCache;
using namespace std::chrono_literals;

namespace {
// Purpose: Await async evidence; inputs: nonblocking predicate; outputs: fails if still false after five seconds.
void await_metadata(const std::function<bool()>& predicate) {
    const auto deadline = QueueMetadataCache::Clock::now() + 5s;
    while (!predicate()) {
        REQUIRE_TRUE(QueueMetadataCache::Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

struct ReadGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;

    // Purpose: Hold a fake metadata read; inputs: none; outputs: releases on request or bounded test timeout.
    void wait() {
        std::unique_lock lock(mutex);
        (void)changed.wait_for(lock, 10s, [this] { return released; });
    }

    // Purpose: Release the fake reader; inputs: none; outputs: wakes its worker.
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};

struct ReleaseReadGate {
    ReadGate& gate;
    // Purpose: Unblock a fake reader on assertion failure; inputs: borrowed gate; outputs: permits worker cleanup.
    ~ReleaseReadGate() {
        gate.release();
    }
};
}  // namespace

TEST_CASE(queue_metadata_reader_reports_file_directory_size_and_missing_transitions) {
    const auto root = test_temp_dir("queue-metadata");
    const auto path = root / "input.txt";
    using flitzzip::app::read_queue_file_metadata;
    REQUIRE_TRUE(read_queue_file_metadata(root).kind == QueueFileKind::Directory);
    REQUIRE_TRUE(read_queue_file_metadata(path).kind == QueueFileKind::Missing);
    std::ofstream(path, std::ios::binary) << "payload";
    REQUIRE_TRUE(read_queue_file_metadata(path).kind == QueueFileKind::File);
    REQUIRE_EQ(*read_queue_file_metadata(path).bytes, 7U);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "x";
    REQUIRE_EQ(*read_queue_file_metadata(path).bytes, 1U);
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    REQUIRE_TRUE(read_queue_file_metadata(path).kind == QueueFileKind::Directory);
    REQUIRE_TRUE(!read_queue_file_metadata(path).bytes);
    const auto device = read_queue_file_metadata(L"\\\\.\\NUL");
    REQUIRE_TRUE(device.kind == QueueFileKind::Unavailable);
    REQUIRE_TRUE(!device.bytes);
    std::filesystem::remove_all(root);
}

// Purpose: Reject a cache without an authoritative metadata reader before it can start a worker.
// Inputs: An empty callback object. Outputs: Construction throws and owns no thread or queued work.
TEST_CASE(queue_metadata_cache_requires_reader) {
    bool rejected = false;
    try {
        const QueueMetadataCache cache(QueueMetadataCache::Reader{});
    } catch (const std::invalid_argument& error) {
        REQUIRE_EQ(std::string_view(error.what()), "Queue metadata requires a reader");
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

TEST_CASE(queue_metadata_hot_reads_are_deduplicated_and_run_on_one_other_thread) {
    std::atomic_int reads = 0;
    const auto caller = std::this_thread::get_id();
    std::atomic_bool other_thread = false;
    QueueMetadataCache cache([&](const auto&) {
        ++reads;
        other_thread.store(std::this_thread::get_id() != caller);
        return QueueFileMetadata{QueueFileKind::File, 42};
    });
    const std::filesystem::path paths[]{"input", "input"};
    cache.synchronize(paths);
    REQUIRE_EQ(reads.load(), 0);
    await_metadata([&] { return cache.get("input").kind == QueueFileKind::File; });
    const auto snapshot_time = QueueMetadataCache::Clock::now();
    for (int i = 0; i < 1000; ++i) {
        REQUIRE_EQ(*cache.get("input", snapshot_time).bytes, 42U);
    }
    REQUIRE_EQ(reads.load(), 1);
    REQUIRE_TRUE(other_thread.load());
    REQUIRE_TRUE(cache.get("unqueued").kind == QueueFileKind::Pending);
    REQUIRE_EQ(reads.load(), 1);
}

TEST_CASE(queue_metadata_blocked_read_does_not_block_snapshots_or_revive_removed_rows) {
    ReadGate gate;
    std::atomic_int reads = 0;
    std::atomic_int notifications = 0;
    QueueMetadataCache cache(
        [&](const auto&) {
            const auto sequence = ++reads;
            if (sequence == 1) {
                gate.wait();
            }
            return QueueFileMetadata{QueueFileKind::File, static_cast<std::uintmax_t>(sequence * 10)};
        },
        [&](const auto&, bool) { ++notifications; });
    ReleaseReadGate release_on_exit{gate};
    const std::filesystem::path paths[]{"same-path", "waiting"};
    cache.synchronize(paths);
    REQUIRE_TRUE(cache.get(paths[0]).kind == QueueFileKind::Pending);
    await_metadata([&] { return reads.load() == 1; });
    REQUIRE_TRUE(cache.get(paths[1]).kind == QueueFileKind::Pending);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE_TRUE(cache.get(paths[0]).kind == QueueFileKind::Pending);
    }
    cache.synchronize({});
    REQUIRE_TRUE(cache.get(paths[0]).kind == QueueFileKind::Pending);
    cache.synchronize(std::span(paths, 1));
    REQUIRE_TRUE(cache.get(paths[0]).kind == QueueFileKind::Pending);
    gate.release();
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::File; });
    cache.stop();
    REQUIRE_EQ(reads.load(), 2);
    REQUIRE_EQ(notifications.load(), 1);
}

TEST_CASE(queue_metadata_expiry_refreshes_size_and_type_without_clearing_the_display_snapshot) {
    std::atomic_int phase = 0;
    std::atomic_int notifications = 0;
    std::atomic_int kind_changes = 0;
    QueueMetadataCache cache(
        [&](const auto&) {
            if (phase.load() == 2) {
                return QueueFileMetadata{QueueFileKind::Directory, {}};
            }
            return QueueFileMetadata{QueueFileKind::File, phase.load() == 0 ? 10U : 20U};
        },
        [&](const auto&, bool kind_changed) {
            ++notifications;
            if (kind_changed) {
                ++kind_changes;
            }
        });
    const std::filesystem::path paths[]{"input"};
    cache.synchronize(paths);
    await_metadata([&] { return cache.get(paths[0]).bytes == 10U; });
    phase.store(1);
    REQUIRE_EQ(*cache.get(paths[0], QueueMetadataCache::Clock::now() + 30s).bytes, 10U);
    await_metadata([&] { return cache.get(paths[0]).bytes == 20U; });
    phase.store(2);
    REQUIRE_TRUE(cache.get(paths[0], QueueMetadataCache::Clock::now() + 30s).kind == QueueFileKind::File);
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::Directory; });
    cache.stop();
    REQUIRE_EQ(notifications.load(), 3);
    REQUIRE_EQ(kind_changes.load(), 2);
}

TEST_CASE(queue_metadata_reader_failure_is_cached_and_retried_after_expiry) {
    std::atomic_int reads = 0;
    QueueMetadataCache cache([&](const auto&) {
        if (++reads == 1) {
            throw std::runtime_error("metadata unavailable");
        }
        return QueueFileMetadata{QueueFileKind::File, 0};
    });
    const std::filesystem::path paths[]{"input"};
    cache.synchronize(paths);
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::Unavailable; });
    REQUIRE_EQ(reads.load(), 1);
    REQUIRE_TRUE(cache.get(paths[0], QueueMetadataCache::Clock::now() + 30s).kind == QueueFileKind::Unavailable);
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::File; });
    REQUIRE_EQ(*cache.get(paths[0]).bytes, 0U);
    REQUIRE_EQ(reads.load(), 2);
}

TEST_CASE(queue_metadata_membership_is_bounded_and_stop_is_final_and_idempotent) {
    std::atomic_int reads = 0;
    QueueMetadataCache cache([&](const auto&) {
        ++reads;
        return QueueFileMetadata{QueueFileKind::File, 1};
    });
    const std::filesystem::path paths[]{"retained"};
    cache.synchronize(paths);
    std::vector<std::filesystem::path> too_many(flitzzip::app::kMaxQueueItems + 1, "input");
    bool rejected = false;
    try {
        cache.synchronize(too_many);
    } catch (const std::length_error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::File; });
    cache.stop();
    cache.stop();
    cache.synchronize(paths);
    REQUIRE_TRUE(cache.get(paths[0]).kind == QueueFileKind::Pending);
    REQUIRE_EQ(reads.load(), 1);
}

TEST_CASE(queue_metadata_notifies_outside_the_cache_lock) {
    QueueMetadataCache* target = nullptr;
    std::atomic_bool notified = false;
    QueueMetadataCache cache(
        [](const auto&) { return QueueFileMetadata{QueueFileKind::Missing, {}}; },
        [&](const auto& path, bool) { notified.store(target->get(path).kind == QueueFileKind::Missing); });
    target = &cache;
    const std::filesystem::path paths[]{"missing"};
    cache.synchronize(paths);
    (void)cache.get(paths[0]);
    await_metadata([&] { return notified.load(); });
}

TEST_CASE(queue_metadata_archive_candidates_preserve_ticks_order_and_regular_file_requirement) {
    std::atomic_int reads = 0;
    QueueMetadataCache cache([&](const auto& path) {
        ++reads;
        return QueueFileMetadata{path == "directory.zip" ? QueueFileKind::Directory : QueueFileKind::File, 1};
    });
    const std::filesystem::path paths[]{"second.flzip", "ignored.txt", "unchecked.zip", "directory.zip", "first.zip"};
    const std::vector<bool> enabled{true, true, false, true};
    cache.synchronize(paths);
    using flitzzip::app::queue_archive_candidates;
    await_metadata([&] { return queue_archive_candidates(paths, enabled, cache).size() == 2; });
    const auto actual = queue_archive_candidates(paths, enabled, cache);
    REQUIRE_EQ(actual[0], paths[0]);
    REQUIRE_EQ(actual[1], paths[4]);
    cache.stop();
    REQUIRE_EQ(reads.load(), 3);
}

TEST_CASE(queue_metadata_refresh_observes_real_resize_delete_and_recreation) {
    const auto root = test_temp_dir("queue-metadata-refresh");
    const std::filesystem::path paths[]{root / "input.zip"};
    std::ofstream(paths[0], std::ios::binary) << "original";
    QueueMetadataCache cache(flitzzip::app::read_queue_file_metadata);
    cache.synchronize(paths);
    await_metadata([&] { return cache.get(paths[0]).bytes == 8U; });
    std::ofstream(paths[0], std::ios::binary | std::ios::trunc) << "new";
    (void)cache.get(paths[0], QueueMetadataCache::Clock::now() + 30s);
    await_metadata([&] { return cache.get(paths[0]).bytes == 3U; });
    std::filesystem::remove(paths[0]);
    (void)cache.get(paths[0], QueueMetadataCache::Clock::now() + 30s);
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::Missing; });
    std::filesystem::create_directory(paths[0]);
    (void)cache.get(paths[0], QueueMetadataCache::Clock::now() + 30s);
    await_metadata([&] { return cache.get(paths[0]).kind == QueueFileKind::Directory; });
    cache.stop();
    std::filesystem::remove_all(root);
}

// Purpose: Prevent notification borrowers from outliving the controller's owned native destination.
// Inputs: Real filesystem metadata, a held notification callback, and asynchronous stop.
// Outputs: Stop joins the callback before returning, is idempotent, and cannot admit a later notification.
TEST_CASE(queue_metadata_stop_joins_inflight_notification_before_returning) {
    const auto directory = test_temp_dir("metadata-notification-owner");
    const auto path = directory / L"source.txt";
    std::ofstream(path, std::ios::binary) << "metadata";
    ReadGate callback_gate;
    std::atomic_bool entered = false;
    std::atomic_bool exited = false;
    std::atomic_bool stopping = false;
    std::atomic_uint calls = 0;
    QueueMetadataCache cache(flitzzip::app::read_queue_file_metadata, [&](const auto&, bool) {
        ++calls;
        entered.store(true);
        callback_gate.wait();
        exited.store(true);
    });
    ReleaseReadGate release_on_setup_failure{callback_gate};
    const std::filesystem::path paths[]{path};
    cache.synchronize(paths);
    REQUIRE_TRUE(cache.get(path).kind == QueueFileKind::Pending);
    await_metadata([&] { return entered.load(); });
    auto stopped = std::async(std::launch::async, [&] {
        stopping.store(true);
        cache.stop();
    });
    // Release before the future's blocking destructor if an assertion below fails.
    ReleaseReadGate release_before_future_cleanup{callback_gate};
    await_metadata([&] { return stopping.load(); });
    REQUIRE_TRUE(stopped.wait_for(20ms) == std::future_status::timeout);
    REQUIRE_TRUE(!exited.load());
    callback_gate.release();
    REQUIRE_TRUE(stopped.wait_for(5s) == std::future_status::ready);
    stopped.get();
    REQUIRE_TRUE(exited.load());
    cache.stop();
    cache.synchronize(paths);
    REQUIRE_TRUE(cache.get(path).kind == QueueFileKind::Pending);
    REQUIRE_EQ(calls.load(), 1U);
}

namespace {

// Purpose: Own a native synchronization event without windows, polling sleeps or process-wide controls.
// Inputs: None. Outputs: Manual-reset event released after every cache and borrowed worker have joined.
class MetadataAdmissionEvent final {
  public:
    // Purpose: Acquire the event. Inputs: None. Outputs: Owned native event or assertion failure.
    MetadataAdmissionEvent() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle) {
        REQUIRE_TRUE(event_ != nullptr);
    }
    // Purpose: Publish a worker observation. Inputs: Live owned event. Outputs: Native signal, without allocation.
    void signal() const noexcept {
        static_cast<void>(SetEvent(event_.get()));
    }
    // Purpose: Await actual worker evidence. Inputs: Bounded milliseconds. Outputs: True only for a native signal.
    bool wait(DWORD milliseconds) const noexcept {
        return WaitForSingleObject(event_.get(), milliseconds) == WAIT_OBJECT_0;
    }

  private:
    std::unique_ptr<void, decltype(&CloseHandle)> event_;
};

// Purpose: Refuse this cache's first admission and preserve native successful execution and lifetime evidence.
// Inputs: Failure kind and owned tasks; the launcher outlives its cache. Outputs: One refusal or a real thread.
class MetadataAdmissionLauncher final : public flitzzip::app::JobThreadLauncher {
  public:
    // Purpose: Select startup refusal. Inputs: Allocation versus OS admission. Outputs: Idle per-instance control.
    explicit MetadataAdmissionLauncher(bool allocation_failure) : allocation_failure_(allocation_failure) {}

    mutable std::atomic_uint attempts{0};
    mutable MetadataAdmissionEvent entered;
    mutable MetadataAdmissionEvent returned;

    // Purpose: Control only the external admission boundary. Inputs: Owned production closure. Outputs: Native owner.
    std::thread launch(std::function<void()> task) const override {
        if (attempts.fetch_add(1) == 0U) {
            if (allocation_failure_) {
                throw std::bad_alloc();
            }
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return JobThreadLauncher::launch([this, task = std::move(task)] {
            entered.signal();
            task();
            returned.signal();
        });
    }

  private:
    bool allocation_failure_;
};

}  // namespace

// Purpose: Contain real cache-thread startup refusal without retrying the same row on paint or expiry checks.
// Inputs: Independent 17-byte file, OS/allocation admission refusals and removal/re-addition of its membership.
// Outputs: Unavailable without Reader/Notify work, actual native read after re-add, exact metadata and final join.
TEST_CASE(queue_metadata_thread_admission_failure_is_unavailable_until_readd_and_joins_real_worker) {
    for (const bool allocation_failure : {false, true}) {
        const auto root = test_temp_dir("queue-metadata-admission");
        const auto path = root / "payload.txt";
        {
            std::ofstream output(path, std::ios::binary);
            output << std::string(17, 'a');
        }
        MetadataAdmissionLauncher launcher(allocation_failure);
        MetadataAdmissionEvent published;
        std::atomic_uint reads{0};
        std::atomic_uint notifications{0};
        {
            QueueMetadataCache cache(
                [&](const auto& source) {
                    ++reads;
                    return flitzzip::app::read_queue_file_metadata(source);
                },
                [&](const auto&, bool) noexcept {
                    ++notifications;
                    published.signal();
                },
                launcher);
            const std::filesystem::path paths[]{path};
            cache.synchronize(paths);
            const auto requested = QueueMetadataCache::Clock::now();
            const auto unavailable = cache.get(path, requested);
            REQUIRE_TRUE(unavailable.kind == QueueFileKind::Unavailable);
            REQUIRE_TRUE(!unavailable.bytes);
            REQUIRE_EQ(reads.load(), 0U);
            REQUIRE_EQ(notifications.load(), 0U);
            REQUIRE_TRUE(!launcher.entered.wait(0));
            REQUIRE_TRUE(!published.wait(0));
            for (unsigned request = 0; request < 3U; ++request) {
                cache.synchronize(paths);
                REQUIRE_TRUE(cache.get(path, requested + 30s).kind == QueueFileKind::Unavailable);
            }
            REQUIRE_EQ(launcher.attempts.load(), 1U);
            cache.synchronize({});
            cache.synchronize(paths);
            const auto renewed = QueueMetadataCache::Clock::now();
            REQUIRE_TRUE(cache.get(path, renewed).kind == QueueFileKind::Pending);
            REQUIRE_TRUE(launcher.entered.wait(5000));
            REQUIRE_TRUE(published.wait(5000));
            const auto ready = cache.get(path, renewed);
            REQUIRE_TRUE(ready.kind == QueueFileKind::File);
            REQUIRE_EQ(ready.bytes.value(), 17U);
            REQUIRE_EQ(reads.load(), 1U);
            REQUIRE_EQ(notifications.load(), 1U);
            REQUIRE_EQ(launcher.attempts.load(), 2U);
            cache.stop();
            REQUIRE_TRUE(launcher.returned.wait(0));
            cache.stop();
        }
        std::ifstream input(path, std::ios::binary);
        REQUIRE_EQ(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()),
                   std::string(17, 'a'));
    }
}

// Purpose: Keep failed admission local to its row and prove it did not leave phantom work for a later real worker.
// Inputs: Two real files, first-row startup refusal and successful second-row native admission without removing first.
// Outputs: Only the second path is read/notified, failed metadata remains Unavailable and the actual worker joins.
TEST_CASE(queue_metadata_admission_failure_preserves_other_rows_without_phantom_work) {
    const auto root = test_temp_dir("queue-metadata-admission-isolation");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    {
        std::ofstream(first, std::ios::binary) << std::string(17, 'a');
        std::ofstream(second, std::ios::binary) << std::string(23, 'b');
    }
    MetadataAdmissionLauncher launcher(false);
    MetadataAdmissionEvent published_second;
    std::atomic_uint first_reads{0}, second_reads{0}, first_notifications{0}, second_notifications{0};
    QueueMetadataCache cache(
        [&](const auto& path) {
            if (path == first) {
                ++first_reads;
            } else {
                ++second_reads;
            }
            return flitzzip::app::read_queue_file_metadata(path);
        },
        [&](const auto& path, bool) noexcept {
            if (path == first) {
                ++first_notifications;
            } else {
                ++second_notifications;
                published_second.signal();
            }
        },
        launcher);
    const std::filesystem::path paths[]{first, second};
    cache.synchronize(paths);
    const auto requested = QueueMetadataCache::Clock::now();
    REQUIRE_TRUE(cache.get(first, requested).kind == QueueFileKind::Unavailable);
    REQUIRE_TRUE(cache.get(second, requested).kind == QueueFileKind::Pending);
    REQUIRE_TRUE(launcher.entered.wait(5000));
    REQUIRE_TRUE(published_second.wait(5000));
    const auto ready = cache.get(second, requested);
    REQUIRE_TRUE(ready.kind == QueueFileKind::File);
    REQUIRE_EQ(ready.bytes.value(), 23U);
    REQUIRE_TRUE(cache.get(first, requested + 30s).kind == QueueFileKind::Unavailable);
    REQUIRE_EQ(first_reads.load(), 0U);
    REQUIRE_EQ(first_notifications.load(), 0U);
    REQUIRE_EQ(second_reads.load(), 1U);
    REQUIRE_EQ(second_notifications.load(), 1U);
    REQUIRE_EQ(launcher.attempts.load(), 2U);
    cache.stop();
    REQUIRE_TRUE(launcher.returned.wait(0));
}

namespace {

// Purpose: Fail one instance's real pending-queue allocation while forwarding all successful native storage.
// Inputs: Arm operations may race worker deallocation; this resource outlives every borrowing cache and its storage.
// Outputs: One bad_alloc on demand, actual upstream allocation/free and independent balanced ownership counts.
class MetadataPendingResource final : public std::pmr::memory_resource {
  public:
    // Purpose: Select native allocation. Inputs: None. Outputs: Disarmed per-instance resource without global state.
    MetadataPendingResource() : upstream_(std::pmr::new_delete_resource()) {}
    // Purpose: Refuse the next actual queue allocation. Inputs: Any control thread. Outputs: One atomic arm.
    void arm() noexcept {
        refuse_next_.store(true);
    }
    // Purpose: End an attempted calibration. Inputs: Any control thread. Outputs: No remaining refusal.
    void disarm() noexcept {
        refuse_next_.store(false);
    }
    // Purpose: Count actual refusals. Inputs: None. Outputs: Thread-safe lifetime refusal count.
    std::size_t refusals() const noexcept {
        return refusals_.load();
    }
    // Purpose: Check full container teardown, not merely worker stop. Inputs: Cache already destroyed.
    // Outputs: True only when every successful allocation was freed and no native bytes remain owned.
    bool balanced() const noexcept {
        return live_bytes_.load() == 0U && allocations_.load() == deallocations_.load();
    }

  private:
    // Purpose: Acquire real storage or reject one admission. Inputs: Native byte/alignment request.
    // Outputs: Actual upstream allocation; accounting changes only after success, or a counted bad_alloc.
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (refuse_next_.exchange(false)) {
            ++refusals_;
            throw std::bad_alloc();
        }
        void* storage = upstream_->allocate(bytes, alignment);
        ++allocations_;
        live_bytes_.fetch_add(bytes);
        return storage;
    }
    // Purpose: Release actual queue storage without fault injection. Inputs: Matching native allocation and extent.
    // Outputs: Forwards native deallocation and records final ownership release; never changes the arm state.
    void do_deallocate(void* storage, std::size_t bytes, std::size_t alignment) noexcept override {
        upstream_->deallocate(storage, bytes, alignment);
        ++deallocations_;
        live_bytes_.fetch_sub(bytes);
    }
    // Purpose: Keep allocator identity bound to this instance. Inputs: Another borrowed resource.
    // Outputs: Equality only for this exact owner; unrelated resources cannot exchange its retained storage.
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::atomic_bool refuse_next_{false};
    std::atomic_size_t refusals_{0}, allocations_{0}, deallocations_{0}, live_bytes_{0};
};

// Purpose: Acquire the real worker before failing its caller's next instance-owned queue allocation.
// Inputs: Resource outlives this launcher and cache. Outputs: Actual native thread and independent lifetime events.
class MetadataPublicationLauncher final : public flitzzip::app::JobThreadLauncher {
  public:
    // Purpose: Borrow one queue resource. Inputs: Live resource. Outputs: Per-instance launcher without global state.
    explicit MetadataPublicationLauncher(MetadataPendingResource& resource) : resource_(resource) {}
    mutable MetadataAdmissionEvent entered;
    mutable MetadataAdmissionEvent returned;
    mutable std::atomic_uint attempts{0};

    // Purpose: Preserve successful native acquisition while controlling only subsequent queue publication.
    // Inputs: Owned production closure. Outputs: Native owner; only nonthrowing operations follow the resource arm.
    std::thread launch(std::function<void()> task) const override {
        ++attempts;
        auto owner = JobThreadLauncher::launch([this, task = std::move(task)] {
            entered.signal();
            task();
            returned.signal();
        });
        resource_.arm();
        return owner;
    }

  private:
    MetadataPendingResource& resource_;
};

}  // namespace

// Purpose: Contain queue publication allocation refusal after actual native worker acquisition in every build.
// Inputs: Two real files and a per-instance forwarding/failing resource armed only after successful native launch.
// Outputs: Unavailable without phantom work, native idle owner serves another row, joins and releases all storage.
TEST_CASE(queue_metadata_publication_allocation_failure_retains_idle_worker_without_phantom_work) {
    const auto root = test_temp_dir("queue-metadata-publication-refusal");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    {
        std::ofstream(first, std::ios::binary) << std::string(17, 'a');
        std::ofstream(second, std::ios::binary) << std::string(23, 'b');
    }
    MetadataPendingResource resource;
    MetadataPublicationLauncher launcher(resource);
    MetadataAdmissionEvent published;
    std::atomic_uint first_reads{0}, second_reads{0}, notifications{0};
    {
        QueueMetadataCache cache(
            [&](const auto& path) {
                if (path == first) {
                    ++first_reads;
                } else {
                    ++second_reads;
                }
                return flitzzip::app::read_queue_file_metadata(path);
            },
            [&](const auto&, bool) noexcept {
                ++notifications;
                published.signal();
            },
            launcher, resource);
        const std::filesystem::path paths[]{first, second};
        cache.synchronize(paths);
        const auto requested = QueueMetadataCache::Clock::now();
        const auto unavailable = cache.get(first, requested);
        resource.disarm();
        REQUIRE_TRUE(resource.refusals() > 0U);
        REQUIRE_TRUE(unavailable.kind == QueueFileKind::Unavailable && !unavailable.bytes);
        REQUIRE_TRUE(launcher.entered.wait(5000));
        REQUIRE_TRUE(!launcher.returned.wait(0));
        REQUIRE_EQ(first_reads.load(), 0U);
        REQUIRE_EQ(notifications.load(), 0U);
        REQUIRE_TRUE(cache.get(first, requested + 30s).kind == QueueFileKind::Unavailable);
        REQUIRE_TRUE(cache.get(second, requested).kind == QueueFileKind::Pending);
        REQUIRE_TRUE(published.wait(5000));
        const auto ready = cache.get(second, requested);
        REQUIRE_TRUE(ready.kind == QueueFileKind::File);
        REQUIRE_EQ(ready.bytes.value(), 23U);
        REQUIRE_EQ(first_reads.load(), 0U);
        REQUIRE_EQ(second_reads.load(), 1U);
        REQUIRE_EQ(notifications.load(), 1U);
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        cache.stop();
        REQUIRE_TRUE(launcher.returned.wait(0));
    }
    REQUIRE_TRUE(resource.balanced());
}

namespace {

// Purpose: Refuse publication while an actual reader remains in flight, proving unrelated FIFO storage survives.
// Inputs: Real cache/resource, existing 17-byte snapshot and controlled reader gate; all owners remain live.
// Outputs: Unavailable without stale bytes, preserved 23-byte owner, exact missing-path FIFO and joined native work.
void exercise_metadata_refresh_publication_failure(QueueMetadataCache& cache, MetadataPendingResource& resource,
                                                   const std::vector<std::filesystem::path>& paths, ReadGate& gate,
                                                   MetadataAdmissionEvent& gate_published,
                                                   MetadataAdmissionEvent& pending_completed) {
    const auto requested = QueueMetadataCache::Clock::now();
    const auto& snapshot = paths.front();
    resource.arm();
    const auto unavailable = cache.get(snapshot, requested + 30s);
    resource.disarm();
    REQUIRE_TRUE(resource.refusals() > 0U);
    REQUIRE_TRUE(unavailable.kind == QueueFileKind::Unavailable && !unavailable.bytes);
    REQUIRE_TRUE(cache.get(snapshot, requested + 60s).kind == QueueFileKind::Unavailable);
    gate.release();
    REQUIRE_TRUE(gate_published.wait(5000));
    REQUIRE_EQ(cache.get(paths[1], requested).bytes.value(), 23U);
    REQUIRE_TRUE(pending_completed.wait(5000));
    for (std::size_t index = 2; index < paths.size(); ++index) {
        REQUIRE_TRUE(cache.get(paths[index], requested).kind == QueueFileKind::Missing);
    }
    cache.stop();
}

}  // namespace

// Purpose: Clear previously published metadata on refresh publication refusal without damaging pending siblings.
// Inputs: Real 17-byte snapshot, controlled in-flight reader, 64 bounded pending paths and an instance-owned resource.
// Outputs: Failed snapshot becomes Unavailable, all sibling reads preserve FIFO, stop joins and destruction frees
// storage.
TEST_CASE(queue_metadata_refresh_publication_failure_does_not_present_stale_snapshot) {
    const auto root = test_temp_dir("queue-metadata-refresh-refusal");
    const auto snapshot = root / "snapshot.txt";
    const auto blocked = root / "blocked.txt";
    {
        std::ofstream(snapshot, std::ios::binary) << std::string(17, 'a');
        std::ofstream(blocked, std::ios::binary) << std::string(23, 'b');
    }
    std::vector<std::filesystem::path> paths{snapshot, blocked};
    for (unsigned index = 0; index < 64U; ++index) {
        paths.push_back(root / ("waiting-" + std::to_string(index)));
    }
    MetadataPendingResource resource;
    ReadGate gate;
    MetadataAdmissionEvent first_published, gate_entered, gate_published, pending_completed;
    std::atomic_uint snapshot_reads{0};
    std::vector<std::filesystem::path> visited;
    visited.reserve(paths.size());
    {
        QueueMetadataCache cache(
            [&](const auto& path) {
                visited.push_back(path);
                if (path == blocked) {
                    gate_entered.signal();
                    gate.wait();
                }
                if (path == snapshot) {
                    ++snapshot_reads;
                }
                return flitzzip::app::read_queue_file_metadata(path);
            },
            [&](const auto& path, bool) noexcept {
                if (path == snapshot) {
                    first_published.signal();
                } else if (path == blocked) {
                    gate_published.signal();
                } else if (path == paths.back()) {
                    pending_completed.signal();
                }
            },
            flitzzip::app::JobThreadLauncher::native(), resource);
        ReleaseReadGate release_before_cache_join{gate};
        cache.synchronize(paths);
        const auto requested = QueueMetadataCache::Clock::now();
        REQUIRE_TRUE(cache.get(snapshot, requested).kind == QueueFileKind::Pending);
        REQUIRE_TRUE(first_published.wait(5000));
        REQUIRE_EQ(cache.get(snapshot, requested).bytes.value(), 17U);
        REQUIRE_TRUE(cache.get(blocked, requested).kind == QueueFileKind::Pending);
        REQUIRE_TRUE(gate_entered.wait(5000));
        for (std::size_t index = 2; index < paths.size(); ++index) {
            REQUIRE_TRUE(cache.get(paths[index], requested).kind == QueueFileKind::Pending);
        }
        exercise_metadata_refresh_publication_failure(cache, resource, paths, gate, gate_published, pending_completed);
    }
    REQUIRE_EQ(visited, paths);
    REQUIRE_EQ(snapshot_reads.load(), 1U);
    REQUIRE_TRUE(resource.balanced());
}
