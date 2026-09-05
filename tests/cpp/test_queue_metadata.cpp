#include "app/queue_metadata.hpp"
#include "test_util.hpp"

#include <atomic>
#include <fstream>

using superzip::app::QueueFileKind;
using superzip::app::QueueFileMetadata;
using superzip::app::QueueMetadataCache;
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
    using superzip::app::read_queue_file_metadata;
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
    std::filesystem::remove_all(root);
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
    std::vector<std::filesystem::path> too_many(superzip::app::kMaxQueueItems + 1, "input");
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
    const std::filesystem::path paths[]{"second.suzip", "ignored.txt", "unchecked.zip", "directory.zip", "first.zip"};
    const std::vector<bool> enabled{true, true, false, true};
    cache.synchronize(paths);
    using superzip::app::queue_archive_candidates;
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
    QueueMetadataCache cache(superzip::app::read_queue_file_metadata);
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
