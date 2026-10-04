#include "test_util.hpp"
#include "ZstdCoverWorkGroup.h"

extern "C" {
#include "fault_allocator.h"
}
#include "zstd.h"
#include "zstd_errors.h"

#include <array>
#include <atomic>
#include <limits>
#include <memory>

namespace {
using GroupOwner = std::unique_ptr<COVER_workGroup, decltype(&COVER_releaseWorkGroup)>;
std::size_t contextDestructions = 0;
std::size_t completedBeforeDestruction = 0;

struct WorkerContext {
    std::atomic<unsigned> returned{0};
    COVER_best_t* best = nullptr;
};

// Purpose: Observe matching committed context cleanup after all worker accesses.
// Inputs: Initialized fixture context. Outputs: Counts destruction and observes completed workers.
void destroy_context(void* storage) {
    auto* context = static_cast<WorkerContext*>(storage);
    completedBeforeDestruction = context->returned.load();
    ++contextDestructions;
    context->~WorkerContext();
}

// Purpose: Model a worker that signals logical completion before its final context access.
// Inputs: Group-owned context and initialized synchronized result record.
// Outputs: Marks the actual final access after logical completion without allocating or throwing.
void finish_worker(void* storage) {
    auto* context = static_cast<WorkerContext*>(storage);
    COVER_best_finish(context->best, {}, COVER_dictSelectionError(std::numeric_limits<std::size_t>::max()));
    Sleep(20);
    context->returned.fetch_add(1);
}

// Purpose: Prepare and initialize one fixture context through the actual private owner.
// Inputs: Live group. Outputs: Committed borrowed context or throws on an unexpected failure.
WorkerContext* prepare_context(COVER_workGroup* group) {
    auto* storage = COVER_prepareWorkContext(group, sizeof(WorkerContext), alignof(WorkerContext), destroy_context);
    REQUIRE_TRUE(storage != nullptr);
    auto* context = new (storage) WorkerContext;
    context->best = COVER_workGroupBest(group);
    COVER_commitWorkContext(group);
    return context;
}
}  // namespace

// Purpose: Require actual pool completion before releasing any shared context or result state.
// Inputs: Two real worker threads with deliberately early logical completion signals.
// Outputs: Observes both final accesses before exactly one destruction and zero retained allocations.
TEST_CASE(zstd_work_group_joins_actual_worker_lifetime) {
    REQUIRE_TRUE(sz_fault_reset(0));
    contextDestructions = 0;
    completedBeforeDestruction = 0;
    {
        const GroupOwner group(COVER_createWorkGroup(2), COVER_releaseWorkGroup);
        REQUIRE_TRUE(group != nullptr);
        auto* context = prepare_context(group.get());
        auto* pool = COVER_workGroupPool(group.get());
        REQUIRE_TRUE(pool != nullptr);
        for (unsigned worker = 0; worker < 2; ++worker) {
            COVER_best_start(context->best);
            POOL_add(pool, finish_worker, context);
        }
    }
    REQUIRE_EQ(completedBeforeDestruction, 2U);
    REQUIRE_EQ(contextDestructions, 1U);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
}

// Purpose: Preserve existing context ownership across malformed geometry and allocation failure.
// Inputs: Serial group, committed predecessor and rejected/faulted replacement requests.
// Outputs: Prior context survives each failure and is destroyed exactly once at explicit completion.
TEST_CASE(zstd_work_group_context_transaction_and_failures) {
    REQUIRE_TRUE(sz_fault_reset(1));
    REQUIRE_TRUE(COVER_createWorkGroup(0) == nullptr);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_TRUE(sz_fault_reset(0));
    contextDestructions = 0;
    {
        const GroupOwner group(COVER_createWorkGroup(0), COVER_releaseWorkGroup);
        REQUIRE_TRUE(group != nullptr);
        auto* context = prepare_context(group.get());
        context->returned.store(7);
        for (const auto bytes : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
            REQUIRE_TRUE(COVER_prepareWorkContext(group.get(), bytes, alignof(WorkerContext), destroy_context) ==
                         nullptr);
        }
        for (const auto alignment : {std::size_t{0}, std::size_t{3}, std::numeric_limits<std::size_t>::max()}) {
            REQUIRE_TRUE(COVER_prepareWorkContext(group.get(), sizeof(WorkerContext), alignment, destroy_context) ==
                         nullptr);
        }
        REQUIRE_TRUE(COVER_prepareWorkContext(group.get(), sizeof(WorkerContext), alignof(WorkerContext), nullptr) ==
                     nullptr);
        REQUIRE_TRUE(sz_fault_fail_after(1));
        REQUIRE_TRUE(COVER_prepareWorkContext(group.get(), sizeof(WorkerContext), alignof(WorkerContext),
                                              destroy_context) == nullptr);
        REQUIRE_EQ(contextDestructions, 0U);
        REQUIRE_EQ(context->returned.load(), 7U);
        REQUIRE_EQ(sz_fault_live_allocations(), 2U);
        COVER_finishWorkContext(group.get());
        COVER_finishWorkContext(group.get());
        REQUIRE_EQ(contextDestructions, 1U);
        REQUIRE_EQ(completedBeforeDestruction, 7U);
        REQUIRE_EQ(sz_fault_live_allocations(), 1U);
    }
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
}

// Purpose: Distinguish failed C initialization from committed context ownership.
// Inputs: Prepared storage without a successful initializer, followed by committed replacement.
// Outputs: Uncommitted storage is released without double cleanup; committed storage is cleaned once.
TEST_CASE(zstd_work_group_uncommitted_initialization_rollback) {
    REQUIRE_TRUE(sz_fault_reset(0));
    contextDestructions = 0;
    {
        const GroupOwner group(COVER_createWorkGroup(1), COVER_releaseWorkGroup);
        REQUIRE_TRUE(group != nullptr);
        REQUIRE_TRUE(COVER_workGroupPool(group.get()) == nullptr);
        REQUIRE_TRUE(COVER_prepareWorkContext(group.get(), sizeof(WorkerContext), alignof(WorkerContext),
                                              destroy_context) != nullptr);
        COVER_finishWorkContext(group.get());
        REQUIRE_EQ(contextDestructions, 0U);
        prepare_context(group.get());
    }
    REQUIRE_EQ(contextDestructions, 1U);
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
}

// Purpose: Bound result publication before any output write or unowned source read.
// Inputs: Empty group and deliberately malformed result descriptors restored before release.
// Outputs: Parameter errors leave destination untouched; ordinary empty publication returns zero.
TEST_CASE(zstd_work_group_result_publication_geometry) {
    REQUIRE_TRUE(sz_fault_reset(0));
    const GroupOwner group(COVER_createWorkGroup(0), COVER_releaseWorkGroup);
    REQUIRE_TRUE(group != nullptr);
    std::array<std::byte, 8> output{};
    const auto before = output;
    auto* best = COVER_workGroupBest(group.get());
    REQUIRE_EQ(COVER_copyWorkDictionary(group.get(), nullptr, 0), 0U);
    best->dictSize = output.size() + 1;
    const auto oversized = COVER_copyWorkDictionary(group.get(), output.data(), output.size());
    best->dictSize = 1;
    const auto absent = COVER_copyWorkDictionary(group.get(), output.data(), output.size());
    best->dictSize = 0;
    REQUIRE_TRUE(ZSTD_isError(oversized));
    REQUIRE_TRUE(ZSTD_isError(absent));
    REQUIRE_TRUE(ZSTD_isError(COVER_copyWorkDictionary(group.get(), nullptr, std::numeric_limits<std::size_t>::max())));
    REQUIRE_TRUE(output == before);
    best->dict = sz_fault_malloc(output.size());
    REQUIRE_TRUE(best->dict != nullptr);
    best->dictSize = output.size();
    std::array<std::byte, 8> expected{};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<std::byte>(index * 17U + 3U);
    }
    std::copy(expected.begin(), expected.end(), static_cast<std::byte*>(best->dict));
    REQUIRE_EQ(COVER_copyWorkDictionary(group.get(), output.data(), output.size()), output.size());
    REQUIRE_TRUE(output == expected);
}
