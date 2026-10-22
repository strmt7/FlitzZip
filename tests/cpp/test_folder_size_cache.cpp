#include "app/folder_size_cache.hpp"
#include "test_util.hpp"

#include <new>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using flitzzip::app::FolderSizeCache;
using flitzzip::app::FolderSizeTask;

namespace {

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool refuse_cache_allocations = false;
_CRT_ALLOC_HOOK previous_cache_allocation_hook = nullptr;

// Purpose: Refuse only this test thread's allocations without exhausting memory.
// Inputs: Native CRT allocation notification; the serial test owns hook registration.
// Outputs: Refusal for allocation/reallocation while armed; otherwise preserves the preceding hook.
int __cdecl cache_allocation_hook(int kind, void* data, std::size_t bytes, int block, long request,
                                  const unsigned char* file, int line) {
    if (refuse_cache_allocations && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)) {
        return FALSE;
    }
    return previous_cache_allocation_hook
               ? previous_cache_allocation_hook(kind, data, bytes, block, request, file, line)
               : TRUE;
}

class CacheAllocationRefusal final {
  public:
    // Purpose: Arm a bounded failure control; inputs: none; outputs: only this thread rejects new heap requests.
    CacheAllocationRefusal() {
        previous_cache_allocation_hook = _CrtSetAllocHook(cache_allocation_hook);
        refuse_cache_allocations = true;
    }
    CacheAllocationRefusal(const CacheAllocationRefusal&) = delete;
    CacheAllocationRefusal& operator=(const CacheAllocationRefusal&) = delete;

    // Purpose: Restore normal allocation before assertions; inputs: owned hook; outputs: prior hook restored.
    ~CacheAllocationRefusal() {
        refuse_cache_allocations = false;
        _CrtSetAllocHook(previous_cache_allocation_hook);
        previous_cache_allocation_hook = nullptr;
    }
};
#endif

enum class CacheAction { Find, Complete, Invalidate, Duplicate };

// Purpose: Exercise the display worker's actual cache operations with pre-existing storage.
// Inputs: One operation over a long key; Debug MSVC independently calibrates and arms real CRT allocation refusal.
// Outputs: Requires correct state without allocating; normal Release consumers retain the same behavior assertions.
void require_cache_operation_without_allocation(CacheAction action) {
    const auto path = std::filesystem::path("folder-cache") / std::string(200, 'q');
    FolderSizeCache cache;
    REQUIRE_TRUE(cache.enqueue(path));
    const auto task = cache.take_next();
#if defined(_MSC_VER) && defined(_DEBUG)
    bool calibrated = false;
    {
        CacheAllocationRefusal refusal;
        try {
            const auto canary = path.native();
            (void)canary;
        } catch (const std::bad_alloc&) {
            calibrated = true;
        }
    }
    REQUIRE_TRUE(calibrated);
#endif
    bool allocated = false;
    bool correct = false;
    {
#if defined(_MSC_VER) && defined(_DEBUG)
        CacheAllocationRefusal refusal;
#endif
        try {
            switch (action) {
            case CacheAction::Find:
                correct = cache.find(path) == task;
                break;
            case CacheAction::Complete:
                correct = cache.complete(task, 147U) && task->bytes == 147U;
                break;
            case CacheAction::Invalidate:
                cache.invalidate(path);
                correct = task->cancelled.load() && !cache.find(path);
                break;
            case CacheAction::Duplicate:
                correct = !cache.enqueue(path);
                break;
            }
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
    }
    REQUIRE_TRUE(!allocated);
    REQUIRE_TRUE(correct);
}

}  // namespace

// Purpose: Keep long-key display lookup nonthrowing; inputs: calibrated heap refusal; outputs: original task found.
TEST_CASE(folder_size_cache_lookup_borrows_the_native_key_under_allocation_failure) {
    require_cache_operation_without_allocation(CacheAction::Find);
}

// Purpose: Publish after traversal without a second allocation failure; inputs: ready task; outputs: exact byte count.
TEST_CASE(folder_size_cache_completion_does_not_allocate_after_traversal) {
    require_cache_operation_without_allocation(CacheAction::Complete);
}

// Purpose: Preserve the nonthrowing metadata callback; inputs: issued task; outputs: cancellation and removal.
TEST_CASE(folder_size_cache_invalidation_does_not_allocate_in_metadata_callbacks) {
    require_cache_operation_without_allocation(CacheAction::Invalidate);
}

// Purpose: Avoid redundant allocation for a queued path; inputs: existing long key; outputs: no duplicate admission.
TEST_CASE(folder_size_cache_duplicate_admission_does_not_allocate) {
    require_cache_operation_without_allocation(CacheAction::Duplicate);
}

TEST_CASE(folder_size_cache_invalidates_changed_kind_without_publishing_its_old_scan) {
    FolderSizeCache cache;
    REQUIRE_TRUE(cache.enqueue("changed"));
    const auto old = cache.take_next();
    REQUIRE_TRUE(cache.enqueue("unchanged"));
    cache.invalidate("changed");
    REQUIRE_TRUE(old->cancelled.load());
    REQUIRE_TRUE(!cache.complete(old, 99));
    REQUIRE_TRUE(cache.enqueue("changed"));
    REQUIRE_EQ(cache.take_next()->path, std::filesystem::path("unchanged"));
    REQUIRE_TRUE(cache.complete(cache.take_next(), 20));
    REQUIRE_EQ(cache.find("changed")->bytes, 20U);
    cache.invalidate("absent");
}

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
