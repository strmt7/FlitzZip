#include "test_util.hpp"
#include "ZstdLegacyBuffers.h"
#include "fault_allocator.h"

#include <array>
#include <limits>
#include <memory>

namespace {
struct CallbackState {
    std::size_t acquisitions{};
    std::size_t releases{};
};

// Purpose: Exercise custom allocator identity and deterministic failure.
// Inputs: Live callback state and byte request. Outputs: Tracked allocation or null.
void* allocate_custom(void* opaque, std::size_t bytes) {
    ++static_cast<CallbackState*>(opaque)->acquisitions;
    return sz_fault_malloc(bytes);
}

// Purpose: Check release routing through the same caller-provided state.
// Inputs: Live callback state and owned allocation. Outputs: One tracked release.
void release_custom(void* opaque, void* data) {
    ++static_cast<CallbackState*>(opaque)->releases;
    sz_fault_free(data);
}

using Owner = std::unique_ptr<ZBUFF_ownedBuffers, decltype(&ZBUFF_releaseOwnedBuffers)>;

// Purpose: Assert an allocation failure retains actual identities, capacities and initialized bytes.
// Inputs: Exclusive owner with filled storage and the requested failure site.
// Outputs: Requires strong rollback and clean partial-allocation release.
void check_growth_failure(ZBUFF_ownedBuffers* owner, std::size_t failure) {
    const auto before = ZBUFF_viewOwnedBuffers(owner);
    std::fill_n(before.input, before.inputCapacity, 'i');
    std::fill_n(before.output, before.outputCapacity, 'o');
    const auto live = sz_fault_live_allocations();
    REQUIRE_TRUE(sz_fault_fail_after(failure));
    REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner, 64, 96), ZBUFF_buffer_allocation_failure);
    const auto after = ZBUFF_viewOwnedBuffers(owner);
    REQUIRE_EQ(after.input, before.input);
    REQUIRE_EQ(after.inputCapacity, before.inputCapacity);
    REQUIRE_EQ(after.output, before.output);
    REQUIRE_EQ(after.outputCapacity, before.outputCapacity);
    REQUIRE_TRUE(std::all_of(after.input, after.input + after.inputCapacity, [](char byte) { return byte == 'i'; }));
    REQUIRE_TRUE(std::all_of(after.output, after.output + after.outputCapacity, [](char byte) { return byte == 'o'; }));
    REQUIRE_EQ(sz_fault_live_allocations(), live);
}
}  // namespace

// Purpose: Cover owner construction and every array acquisition failure for both allocator contracts.
// Inputs: Default and custom callbacks, initial/growth allocation sites.
// Outputs: Requires full rollback, successful retry and matching leak-free destruction.
TEST_CASE(zstd_legacy_owned_buffers_allocation_failures) {
    for (const bool custom : {false, true}) {
        CallbackState state;
        const ZBUFF_bufferAllocator allocator =
            custom ? ZBUFF_bufferAllocator{allocate_custom, release_custom, &state} : ZBUFF_bufferAllocator{};
        REQUIRE_TRUE(sz_fault_reset(1));
        REQUIRE_TRUE(ZBUFF_createOwnedBuffers(allocator) == nullptr);
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        for (const auto failure : {1U, 2U}) {
            REQUIRE_TRUE(sz_fault_reset(0));
            {
                Owner owner(ZBUFF_createOwnedBuffers(allocator), ZBUFF_releaseOwnedBuffers);
                REQUIRE_TRUE(owner != nullptr);
                REQUIRE_TRUE(sz_fault_fail_after(failure));
                REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner.get(), 16, 24), ZBUFF_buffer_allocation_failure);
                const auto empty = ZBUFF_viewOwnedBuffers(owner.get());
                REQUIRE_EQ(empty.input, nullptr);
                REQUIRE_EQ(empty.output, nullptr);
                REQUIRE_EQ(empty.inputCapacity, 0U);
                REQUIRE_EQ(empty.outputCapacity, 0U);
                REQUIRE_EQ(sz_fault_live_allocations(), 1U);
                REQUIRE_TRUE(sz_fault_fail_after(0));
                REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner.get(), 16, 24), ZBUFF_buffer_ok);
                check_growth_failure(owner.get(), failure);
                REQUIRE_TRUE(sz_fault_fail_after(0));
                REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner.get(), 64, 96), ZBUFF_buffer_ok);
                const auto grown = ZBUFF_viewOwnedBuffers(owner.get());
                REQUIRE_EQ(grown.inputCapacity, 64U);
                REQUIRE_EQ(grown.outputCapacity, 96U);
                REQUIRE_EQ(sz_fault_live_allocations(), 3U);
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
        if (custom) {
            // Failed acquisitions return no owner and therefore require no release.
            REQUIRE_EQ(state.acquisitions - state.releases, 5U);
        }
    }
}

// Purpose: Reject incomplete callbacks and unrepresentable owner geometry before allocation.
// Inputs: Null owner, either missing callback, and capacities above PTRDIFF_MAX.
// Outputs: Requires ordinary rejection without changed ownership or invalid releases.
TEST_CASE(zstd_legacy_owned_buffers_invalid_geometry) {
    REQUIRE_TRUE(sz_fault_reset(0));
    CallbackState state;
    REQUIRE_TRUE(ZBUFF_createOwnedBuffers({allocate_custom, nullptr, &state}) == nullptr);
    REQUIRE_TRUE(ZBUFF_createOwnedBuffers({nullptr, release_custom, &state}) == nullptr);
    REQUIRE_EQ(state.acquisitions, 0U);
    REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(nullptr, 1, 1), ZBUFF_buffer_invalid);
    {
        Owner owner(ZBUFF_createOwnedBuffers({}), ZBUFF_releaseOwnedBuffers);
        REQUIRE_TRUE(owner != nullptr);
        const auto excess = static_cast<std::size_t>(PTRDIFF_MAX) + 1;
        REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner.get(), excess, 1), ZBUFF_buffer_invalid);
        REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner.get(), 1, excess), ZBUFF_buffer_invalid);
        REQUIRE_EQ(sz_fault_live_allocations(), 1U);
        REQUIRE_EQ(ZBUFF_reserveOwnedBuffers(owner.get(), 0, 0), ZBUFF_buffer_ok);
    }
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
}

// Purpose: Compare every bounded overlap direction and offset with an independent immutable-input oracle.
// Inputs: One canary-filled extent, all source/destination offsets and valid copy counts.
// Outputs: Requires exact output and untouched surrounding bytes for every transfer.
TEST_CASE(zstd_legacy_checked_copy_overlap_oracle) {
    std::array<char, 34> original{};
    for (std::size_t index = 0; index < original.size(); ++index)
        original[index] = static_cast<char>(index + 1);
    for (std::size_t source = 0; source <= 32; ++source) {
        for (std::size_t destination = 0; destination <= 32; ++destination) {
            for (std::size_t count = 0; count <= std::min(32 - source, 32 - destination); ++count) {
                auto actual = original;
                auto expected = original;
                for (std::size_t index = 0; index < count; ++index)
                    expected[1 + destination + index] = original[1 + source + index];
                REQUIRE_EQ(ZBUFF_copyBytes(actual.data() + 1, 32, destination, actual.data() + 1, 32, source, count),
                           ZBUFF_buffer_ok);
                REQUIRE_EQ(actual, expected);
            }
        }
    }
}

// Purpose: Reject invalid complete geometries without writes or unsafe pointer formation.
// Inputs: Out-of-range offsets/counts, null nonempty extents and unsigned extremes.
// Outputs: Requires unchanged canaries and correct empty-null behavior.
TEST_CASE(zstd_legacy_checked_copy_invalid_geometry) {
    std::array<char, 16> destination{};
    destination.fill('d');
    const auto before = destination;
    const std::array<char, 16> source{};
    const auto excess = static_cast<std::size_t>(PTRDIFF_MAX) + 1;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    for (const auto offset : {17U, 32U}) {
        REQUIRE_EQ(ZBUFF_copyBytes(destination.data(), destination.size(), offset, source.data(), source.size(), 0, 1),
                   ZBUFF_buffer_invalid);
        REQUIRE_EQ(ZBUFF_copyBytes(destination.data(), destination.size(), 0, source.data(), source.size(), offset, 1),
                   ZBUFF_buffer_invalid);
    }
    for (const auto count : {std::size_t{17}, excess, maximum}) {
        REQUIRE_EQ(ZBUFF_copyBytes(destination.data(), destination.size(), 0, source.data(), source.size(), 0, count),
                   ZBUFF_buffer_invalid);
    }
    REQUIRE_EQ(ZBUFF_copyBytes(nullptr, 1, 0, source.data(), source.size(), 0, 0), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_copyBytes(destination.data(), destination.size(), 0, nullptr, 1, 0, 0), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_copyBytes(destination.data(), excess, 0, source.data(), source.size(), 0, 0),
               ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_copyBytes(destination.data(), destination.size(), 0, source.data(), excess, 0, 0),
               ZBUFF_buffer_invalid);
    REQUIRE_EQ(
        ZBUFF_copyBytes(destination.data(), destination.size(), maximum, source.data(), source.size(), maximum, 0),
        ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_copyBytes(nullptr, 0, 0, nullptr, 0, 0, 0), ZBUFF_buffer_ok);
    REQUIRE_EQ(destination, before);
}
