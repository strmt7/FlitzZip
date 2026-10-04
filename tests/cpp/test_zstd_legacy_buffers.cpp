#include "test_util.hpp"
#include "ZstdLegacyBuffers.h"
#include "fault_allocator.h"

#include <array>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

namespace {
struct CallbackState {
    std::size_t acquisitions{};
    std::size_t releases{};
};

struct MisalignedState {
    alignas(std::max_align_t) std::array<char, 1024> bytes{};
    std::size_t acquisitions{};
    std::size_t releases{};
    bool matching{true};
};

// Purpose: Supply deliberately invalid record alignment without pressure or undefined test allocation.
// Inputs: Fixed aligned fixture storage and bounded acquisition request. Outputs: An interior misaligned address.
void* allocate_misaligned(void* opaque, std::size_t bytes) {
    auto& state = *static_cast<MisalignedState*>(opaque);
    ++state.acquisitions;
    return bytes < state.bytes.size() ? state.bytes.data() + 1 : nullptr;
}

// Purpose: Verify rollback returns the exact address acquired from a malformed allocator.
// Inputs: Fixture state and rejected record storage. Outputs: Release count and matching-address evidence.
void release_misaligned(void* opaque, void* address) {
    auto& state = *static_cast<MisalignedState*>(opaque);
    ++state.releases;
    state.matching = state.matching && address == state.bytes.data() + 1;
}

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

// Purpose: Reject malformed custom record alignment before any placement construction.
// Inputs: Deliberately misaligned callbacks for every ownership record kind.
// Outputs: Requires null ownership and matching rollback of every acquired address.
TEST_CASE(zstd_legacy_owned_records_reject_misaligned_allocators) {
    MisalignedState state;
    const ZBUFF_bufferAllocator allocator{allocate_misaligned, release_misaligned, &state};
    REQUIRE_TRUE(ZBUFF_createOwnedBuffers(allocator) == nullptr);
    REQUIRE_TRUE(ZBUFF_createOwnedHistory(allocator) == nullptr);
    REQUIRE_TRUE(ZBUFF_createDecoderOwner(allocator, 64, alignof(std::max_align_t)) == nullptr);
    REQUIRE_EQ(state.acquisitions, 3U);
    REQUIRE_EQ(state.releases, state.acquisitions);
    REQUIRE_TRUE(state.matching);
}

// Purpose: Check bounded ring history against an independent sequence model across wrap, growth and truncation.
// Inputs: Every small window and varied initialized source range; source bytes are overwritten after capture.
// Outputs: Requires exact logical suffixes, initialized extents and unchanged output canaries.
TEST_CASE(zstd_legacy_owned_history_independent_oracle) {
    using History = std::unique_ptr<ZBUFF_ownedHistory, decltype(&ZBUFF_releaseOwnedHistory)>;
    REQUIRE_TRUE(sz_fault_reset(0));
    for (const std::size_t limit : {0U, 1U, 2U, 3U, 7U, 16U, 31U, 64U, 129U}) {
        History owner(ZBUFF_createOwnedHistory({}), ZBUFF_releaseOwnedHistory);
        REQUIRE_TRUE(owner != nullptr);
        REQUIRE_EQ(ZBUFF_resetOwnedHistory(owner.get(), limit), ZBUFF_buffer_ok);
        std::deque<char> expected;
        auto active_limit = limit;
        std::array<char, 260> input{};
        for (std::size_t iteration = 0; iteration < 40; ++iteration) {
            active_limit = iteration % 3 == 1 ? limit / 2 : limit;
            REQUIRE_EQ(ZBUFF_limitOwnedHistory(owner.get(), active_limit), ZBUFF_buffer_ok);
            while (expected.size() > active_limit) {
                expected.pop_front();
            }
            for (std::size_t index = 0; index < input.size(); ++index) {
                input[index] = static_cast<char>((iteration * 31 + index) % 127);
            }
            const std::size_t count = (iteration * 37) % 131;
            REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), input.data(), input.size(), 3, count), ZBUFF_buffer_ok);
            expected.insert(expected.end(), input.begin() + 3, input.begin() + 3 + count);
            while (expected.size() > active_limit) {
                expected.pop_front();
            }
            input.fill('#');
            REQUIRE_EQ(ZBUFF_ownedHistorySize(owner.get()), expected.size());
            for (std::size_t distance = 1; distance <= expected.size(); ++distance) {
                const auto selected = std::min<std::size_t>(5, distance);
                std::vector<char> output(selected + 2, '!');
                REQUIRE_EQ(ZBUFF_copyOwnedHistory(owner.get(), output.data(), output.size(), 1, distance, selected),
                           ZBUFF_buffer_ok);
                REQUIRE_TRUE(std::equal(output.begin() + 1, output.end() - 1, expected.end() - distance));
                REQUIRE_EQ(output.front(), '!');
                REQUIRE_EQ(output.back(), '!');
            }
        }
        REQUIRE_EQ(ZBUFF_limitOwnedHistory(owner.get(), 1), ZBUFF_buffer_ok);
        REQUIRE_EQ(ZBUFF_ownedHistorySize(owner.get()), std::min<std::size_t>(1, expected.size()));
        if (!expected.empty()) {
            char last = '!';
            REQUIRE_EQ(ZBUFF_copyOwnedHistory(owner.get(), &last, 1, 0, 1, 1), ZBUFF_buffer_ok);
            REQUIRE_EQ(last, expected.back());
        }
        REQUIRE_EQ(ZBUFF_resetOwnedHistory(owner.get(), limit), ZBUFF_buffer_ok);
        REQUIRE_EQ(ZBUFF_ownedHistorySize(owner.get()), 0U);
    }
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
}

// Purpose: Verify history acquisition, growth and clone failures preserve initialized bytes and allocator identity.
// Inputs: Default and custom allocation contracts, every acquisition boundary and independent source/destination
// owners. Outputs: Requires rollback, successful retry, independent clone storage and exactly matching release.
TEST_CASE(zstd_legacy_owned_history_allocation_failures) {
    using History = std::unique_ptr<ZBUFF_ownedHistory, decltype(&ZBUFF_releaseOwnedHistory)>;
    for (const bool custom : {false, true}) {
        CallbackState state;
        const ZBUFF_bufferAllocator allocator =
            custom ? ZBUFF_bufferAllocator{allocate_custom, release_custom, &state} : ZBUFF_bufferAllocator{};
        REQUIRE_TRUE(sz_fault_reset(1));
        REQUIRE_TRUE(ZBUFF_createOwnedHistory(allocator) == nullptr);
        REQUIRE_TRUE(sz_fault_reset(0));
        {
            History owner(ZBUFF_createOwnedHistory(allocator), ZBUFF_releaseOwnedHistory);
            History clone(ZBUFF_createOwnedHistory(allocator), ZBUFF_releaseOwnedHistory);
            REQUIRE_TRUE(owner != nullptr && clone != nullptr);
            REQUIRE_EQ(ZBUFF_resetOwnedHistory(owner.get(), 64), ZBUFF_buffer_ok);
            const std::array<char, 6> initial{'a', 'b', 'c', 'd', 'e', 'f'};
            REQUIRE_TRUE(sz_fault_fail_after(1));
            REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), initial.data(), initial.size(), 0, initial.size()),
                       ZBUFF_buffer_allocation_failure);
            REQUIRE_EQ(ZBUFF_ownedHistorySize(owner.get()), 0U);
            REQUIRE_TRUE(sz_fault_fail_after(0));
            REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), initial.data(), initial.size(), 0, initial.size()),
                       ZBUFF_buffer_ok);
            std::array<char, 32> growth{};
            REQUIRE_TRUE(sz_fault_fail_after(1));
            REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), growth.data(), growth.size(), 0, growth.size()),
                       ZBUFF_buffer_allocation_failure);
            std::array<char, 6> snapshot{};
            REQUIRE_EQ(ZBUFF_copyOwnedHistory(owner.get(), snapshot.data(), snapshot.size(), 0, snapshot.size(),
                                              snapshot.size()),
                       ZBUFF_buffer_ok);
            REQUIRE_TRUE(snapshot == initial);
            REQUIRE_TRUE(sz_fault_fail_after(0));
            REQUIRE_EQ(ZBUFF_cloneOwnedHistory(clone.get(), owner.get()), ZBUFF_buffer_ok);
            REQUIRE_EQ(ZBUFF_resetOwnedHistory(owner.get(), 64), ZBUFF_buffer_ok);
            REQUIRE_EQ(ZBUFF_copyOwnedHistory(clone.get(), snapshot.data(), snapshot.size(), 0, snapshot.size(),
                                              snapshot.size()),
                       ZBUFF_buffer_ok);
            REQUIRE_TRUE(snapshot == initial);
            REQUIRE_TRUE(sz_fault_fail_after(1));
            REQUIRE_EQ(ZBUFF_cloneOwnedHistory(owner.get(), clone.get()), ZBUFF_buffer_allocation_failure);
            REQUIRE_EQ(ZBUFF_ownedHistorySize(owner.get()), 0U);
            REQUIRE_EQ(ZBUFF_ownedHistorySize(clone.get()), initial.size());
            REQUIRE_TRUE(sz_fault_fail_after(0));
            REQUIRE_EQ(ZBUFF_cloneOwnedHistory(owner.get(), clone.get()), ZBUFF_buffer_ok);
            REQUIRE_EQ(ZBUFF_cloneOwnedHistory(owner.get(), owner.get()), ZBUFF_buffer_ok);
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        if (custom) {
            REQUIRE_EQ(state.acquisitions - state.releases, 4U);
        }
    }
}

// Purpose: Reject impossible history geometry before any output write, allocation or pointer formation.
// Inputs: Null owners, invalid callback pairs, extreme extents, offsets, distances and copy lengths.
// Outputs: Ordinary rejection with retained history, output canaries and allocation count.
TEST_CASE(zstd_legacy_owned_history_invalid_geometry) {
    using History = std::unique_ptr<ZBUFF_ownedHistory, decltype(&ZBUFF_releaseOwnedHistory)>;
    REQUIRE_TRUE(sz_fault_reset(0));
    REQUIRE_TRUE(ZBUFF_createOwnedHistory({allocate_custom, nullptr, nullptr}) == nullptr);
    REQUIRE_TRUE(ZBUFF_createOwnedHistory({nullptr, release_custom, nullptr}) == nullptr);
    History owner(ZBUFF_createOwnedHistory({}), ZBUFF_releaseOwnedHistory);
    REQUIRE_TRUE(owner != nullptr);
    const auto excessive = static_cast<std::size_t>(PTRDIFF_MAX) + 1;
    REQUIRE_EQ(ZBUFF_resetOwnedHistory(nullptr, 16), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_resetOwnedHistory(owner.get(), excessive), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_resetOwnedHistory(owner.get(), 16), ZBUFF_buffer_ok);
    const std::array<char, 3> input{'a', 'b', 'c'};
    REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), input.data(), input.size(), 0, input.size()), ZBUFF_buffer_ok);
    const auto live = sz_fault_live_allocations();
    REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), nullptr, 1, 0, 1), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), input.data(), input.size(), 4, 0), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), input.data(), input.size(), 2, 2), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_appendOwnedHistory(owner.get(), input.data(), excessive, 0, 0), ZBUFF_buffer_invalid);
    std::array<char, 5> output{'!', '!', '!', '!', '!'};
    const auto original = output;
    for (const auto request : {std::array<std::size_t, 3>{0, 4, 1}, {0, 2, 3}, {4, 2, 2}, {6, 0, 0}}) {
        REQUIRE_EQ(
            ZBUFF_copyOwnedHistory(owner.get(), output.data(), output.size(), request[0], request[1], request[2]),
            ZBUFF_buffer_invalid);
        REQUIRE_TRUE(output == original);
    }
    REQUIRE_EQ(ZBUFF_copyOwnedHistory(owner.get(), nullptr, 1, 0, 1, 1), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_copyOwnedHistory(nullptr, output.data(), output.size(), 0, 0, 0), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_limitOwnedHistory(owner.get(), excessive), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_ownedHistorySize(owner.get()), input.size());
    REQUIRE_EQ(sz_fault_live_allocations(), live);
    REQUIRE_EQ(ZBUFF_copyOwnedHistory(owner.get(), nullptr, 0, 0, 0, 0), ZBUFF_buffer_ok);
    REQUIRE_EQ(ZBUFF_cloneOwnedHistory(owner.get(), nullptr), ZBUFF_buffer_invalid);
    REQUIRE_EQ(ZBUFF_cloneOwnedHistory(nullptr, owner.get()), ZBUFF_buffer_invalid);
}

// Purpose: Exercise every decoder-tree acquisition failure under default and custom allocation contracts.
// Inputs: Three acquisition sites, bounded aligned storage and independent child history.
// Outputs: Requires complete construction, matching rollback, aligned storage and leak-free destruction.
TEST_CASE(zstd_legacy_decoder_owner_complete_construction) {
    using Decoder = std::unique_ptr<ZBUFF_decoderOwner, decltype(&ZBUFF_releaseDecoderOwner)>;
    for (const bool custom : {false, true}) {
        CallbackState state;
        const ZBUFF_bufferAllocator allocator =
            custom ? ZBUFF_bufferAllocator{allocate_custom, release_custom, &state} : ZBUFF_bufferAllocator{};
        for (const std::size_t failure : {1U, 2U, 3U}) {
            REQUIRE_TRUE(sz_fault_reset(failure));
            REQUIRE_TRUE(ZBUFF_createDecoderOwner(allocator, 512, alignof(std::max_align_t)) == nullptr);
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
            REQUIRE_TRUE(sz_fault_reset(0));
            {
                Decoder owner(ZBUFF_createDecoderOwner(allocator, 512, alignof(std::max_align_t)),
                              ZBUFF_releaseDecoderOwner);
                REQUIRE_TRUE(owner != nullptr);
                auto* storage = static_cast<char*>(ZBUFF_decoderStorage(owner.get()));
                REQUIRE_TRUE(storage != nullptr);
                REQUIRE_EQ(reinterpret_cast<std::uintptr_t>(storage) % alignof(std::max_align_t), 0U);
                std::fill_n(storage, 512, 'x');
                REQUIRE_EQ(storage[0], 'x');
                REQUIRE_EQ(storage[511], 'x');
                auto* history = ZBUFF_decoderHistory(owner.get());
                REQUIRE_TRUE(history != nullptr);
                const auto initial = ZBUFF_decoderBaseSize(512);
                REQUIRE_TRUE(initial > 512);
                REQUIRE_EQ(ZBUFF_decoderOwnedSize(owner.get()), initial);
                REQUIRE_EQ(ZBUFF_resetOwnedHistory(history, 32), ZBUFF_buffer_ok);
                REQUIRE_EQ(ZBUFF_appendOwnedHistory(history, storage, 512, 0, 16), ZBUFF_buffer_ok);
                REQUIRE_EQ(ZBUFF_decoderOwnedSize(owner.get()), initial + 16);
                std::fill_n(storage, 512, 'y');
                std::array<char, 16> retained{};
                REQUIRE_EQ(ZBUFF_copyOwnedHistory(history, retained.data(), retained.size(), 0, 16, 16),
                           ZBUFF_buffer_ok);
                // Purpose: Check retained byte identity. Inputs: One byte. Outputs: True for the captured value.
                REQUIRE_TRUE(std::all_of(retained.begin(), retained.end(), [](char byte) { return byte == 'x'; }));
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
        if (custom) {
            REQUIRE_EQ(state.acquisitions - state.releases, 3U);
        }
    }
    REQUIRE_TRUE(ZBUFF_createDecoderOwner({}, 0, 1) == nullptr);
    REQUIRE_TRUE(ZBUFF_createDecoderOwner({}, static_cast<std::size_t>(PTRDIFF_MAX) + 1, 1) == nullptr);
    REQUIRE_TRUE(ZBUFF_createDecoderOwner({}, 16, 0) == nullptr);
    REQUIRE_TRUE(ZBUFF_createDecoderOwner({}, 16, 3) == nullptr);
    REQUIRE_TRUE(ZBUFF_createDecoderOwner({}, 16, alignof(std::max_align_t) * 2) == nullptr);
    REQUIRE_TRUE(ZBUFF_decoderStorage(nullptr) == nullptr);
    REQUIRE_TRUE(ZBUFF_decoderHistory(nullptr) == nullptr);
    REQUIRE_EQ(ZBUFF_decoderOwnedSize(nullptr), 0U);
    REQUIRE_EQ(ZBUFF_decoderBaseSize(SIZE_MAX), SIZE_MAX);
    ZBUFF_releaseDecoderOwner(nullptr);
}

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
