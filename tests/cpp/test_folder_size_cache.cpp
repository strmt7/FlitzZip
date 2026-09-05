#include "app/folder_size_cache.hpp"
#include "test_util.hpp"

using superzip::app::FolderSizeCache;
using superzip::app::FolderSizeTask;

TEST_CASE(folder_size_cache_requeues_changed_folder_after_clear) {
    FolderSizeCache cache;
    REQUIRE_TRUE(cache.enqueue("folder"));
    auto first = cache.take_next();
    REQUIRE_TRUE(cache.complete(first, 10));
    REQUIRE_EQ(cache.find("folder")->bytes, 10U);
    cache.retain({});
    REQUIRE_TRUE(!cache.find("folder"));
    REQUIRE_TRUE(cache.enqueue("folder"));
    auto second = cache.take_next();
    REQUIRE_TRUE(second != first);
    REQUIRE_TRUE(cache.complete(second, 20));
    REQUIRE_EQ(cache.find("folder")->bytes, 20U);
}

TEST_CASE(folder_size_cache_discards_removed_inflight_result_after_readd) {
    FolderSizeCache cache;
    REQUIRE_TRUE(cache.enqueue("folder"));
    auto first = cache.take_next();
    cache.retain({});
    REQUIRE_TRUE(first->cancelled.load());
    REQUIRE_TRUE(cache.enqueue("folder"));
    auto second = cache.take_next();
    REQUIRE_TRUE(!cache.complete(first, 10));
    REQUIRE_TRUE(second->state == FolderSizeTask::State::Pending);
    REQUIRE_TRUE(cache.complete(second, 20));
    REQUIRE_TRUE(!cache.complete(first, std::nullopt));
    REQUIRE_EQ(cache.find("folder")->bytes, 20U);
}

TEST_CASE(folder_size_cache_removes_waiting_work_and_preserves_retained_rows) {
    FolderSizeCache cache;
    REQUIRE_TRUE(cache.enqueue("removed"));
    auto removed = cache.find("removed");
    REQUIRE_TRUE(cache.enqueue("retained"));
    REQUIRE_TRUE(!cache.enqueue("retained"));
    const std::filesystem::path paths[]{"retained"};
    cache.retain(paths);
    REQUIRE_TRUE(removed->cancelled.load());
    REQUIRE_TRUE(cache.has_pending());
    auto task = cache.take_next();
    REQUIRE_EQ(task->path, std::filesystem::path("retained"));
    REQUIRE_TRUE(!task->cancelled.load());
    REQUIRE_TRUE(!cache.has_pending());
    REQUIRE_TRUE(!cache.take_next());
    REQUIRE_TRUE(cache.complete(task, std::nullopt));
    cache.retain(paths);
    REQUIRE_TRUE(cache.find("retained")->state == FolderSizeTask::State::Failed);
    cache.retain({});
    REQUIRE_TRUE(cache.enqueue("retained"));
    REQUIRE_TRUE(cache.complete(cache.take_next(), 0));
    REQUIRE_TRUE(cache.find("retained")->state == FolderSizeTask::State::Ready);
}
