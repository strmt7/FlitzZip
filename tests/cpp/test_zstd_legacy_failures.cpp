#include "test_util.hpp"
#include "fault_allocator.h"
#include "zstd_legacy_fixture.hpp"

#include <array>
#include <span>

namespace {

constexpr std::array<unsigned, 3> kLegacyVersions{5, 6, 7};

// Purpose: Free a test-owned buffered decoder on every assertion path.
// Inputs: A context from the intercepted production source and its exact legacy version.
// Outputs: Releases it before subsequent tests; never transfers ownership implicitly.
struct LegacyOwner {
    void* context;
    unsigned version;

    // Purpose: Release this exclusively owned context; no inputs and no thrown exceptions.
    ~LegacyOwner() {
        sz_legacy_free(context, version);
    }
};

struct CustomAllocationOwner {
    void* address;
    int custom;

    // Purpose: Release helper output even when a test assertion throws.
    // Inputs: Retained allocation and allocator selection.
    // Outputs: Frees the allocation exactly once without changing fault state.
    ~CustomAllocationOwner() {
        sz_custom_free(address, custom);
    }
};

}  // namespace

// Purpose: Return custom allocation failure without a null write or disturbing an existing owner.
// Inputs: Injected failures for zero and positive extents, with a live preceding allocation and a successful retry.
// Outputs: Requires NULL, retained ownership, zero-initialized retry and leak-free destruction.
TEST_CASE(zstd_custom_calloc_failure_preserves_owners) {
    for (const auto bytes : {0U, 1U, 64U, 4096U, 1048576U}) {
        REQUIRE_TRUE(sz_fault_reset(0));
        {
            CustomAllocationOwner retained{sz_custom_calloc(16U, 1), 1};
            REQUIRE_TRUE(retained.address != nullptr);
            REQUIRE_TRUE(sz_fault_fail_after(1U));
            REQUIRE_TRUE(sz_custom_calloc(bytes, 1) == nullptr);
            REQUIRE_EQ(sz_fault_live_allocations(), 1U);
            REQUIRE_TRUE(sz_fault_fail_after(0U));
            if (bytes != 0U) {
                CustomAllocationOwner retry{sz_custom_calloc(bytes, 1), 1};
                REQUIRE_TRUE(retry.address != nullptr);
                const auto* data = static_cast<const unsigned char*>(retry.address);
                for (unsigned index = 0; index < bytes; ++index) {
                    REQUIRE_EQ(data[index], 0U);
                }
            }
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}

// Purpose: Preserve successful zero initialization and release for custom and standard allocators.
// Inputs: Four bounded positive allocation extents; custom callback fills storage with nonzero bytes before return.
// Outputs: Requires every requested byte to be zero and no leaked or invalid custom releases.
TEST_CASE(zstd_custom_calloc_success_zeroes_requested_extent) {
    for (const auto custom : {0, 1}) {
        for (const auto bytes : {1U, 64U, 4096U, 1048576U}) {
            REQUIRE_TRUE(sz_fault_reset(0));
            {
                CustomAllocationOwner owner{sz_custom_calloc(bytes, custom), custom};
                REQUIRE_TRUE(owner.address != nullptr);
                const auto* data = static_cast<const unsigned char*>(owner.address);
                for (unsigned index = 0; index < bytes; ++index) {
                    REQUIRE_EQ(data[index], 0U);
                }
                REQUIRE_EQ(sz_fault_live_allocations(), custom == 0 ? 0U : 1U);
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
    }
}

// Purpose: Check both constructor failures and a successful owner for one shipped legacy decoder.
// Inputs: version is 5, 6 or 7; each case injects a specific allocation failure without memory pressure.
// Outputs: Requires NULL and no leaks on failure, two owned allocations on success, and no invalid releases.
void check_constructor_failures(unsigned version) {
    for (const auto fail_on : {1U, 2U, 3U}) {
        REQUIRE_TRUE(sz_fault_reset(fail_on));
        bool expected_result;
        std::size_t live;
        {
            LegacyOwner owner{sz_legacy_create(version), version};
            expected_result = (owner.context == nullptr) == (fail_on <= 2U);
            live = sz_fault_live_allocations();
        }
        REQUIRE_TRUE(expected_result);
        REQUIRE_EQ(live, fail_on <= 2U ? 0U : 2U);
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}

// Purpose: Cover v0.5 constructor failures; no inputs, requires clean allocation ownership.
TEST_CASE(zstd_legacy_v05_constructor_allocation_failures) {
    check_constructor_failures(5);
}

// Purpose: Cover v0.6 constructor failures; no inputs, requires clean allocation ownership.
TEST_CASE(zstd_legacy_v06_constructor_allocation_failures) {
    check_constructor_failures(6);
}

// Purpose: Cover v0.7 constructor failures; no inputs, requires clean allocation ownership.
TEST_CASE(zstd_legacy_v07_constructor_allocation_failures) {
    check_constructor_failures(7);
}

// Purpose: Reject either allocation failure during first-time legacy initialization without a partial owner.
// Inputs: All shipped versions and their two constructor allocation sites.
// Outputs: Requires an unchanged null owner on error and leak-free success/destruction.
TEST_CASE(zstd_legacy_first_initialization_allocation_failures) {
    for (const auto version : kLegacyVersions) {
        for (const auto fail_on : {1U, 2U, 3U}) {
            REQUIRE_TRUE(sz_fault_reset(fail_on));
            {
                LegacyOwner owner{nullptr, version};
                const auto failed = sz_legacy_initialize(&owner.context, 0, version, nullptr, 0);
                REQUIRE_EQ(failed, fail_on <= 2U ? 1 : 0);
                REQUIRE_EQ(owner.context == nullptr, fail_on <= 2U);
                REQUIRE_EQ(sz_fault_live_allocations(), fail_on <= 2U ? 0U : 2U);
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
    }
}

// Purpose: Preserve the old owner across either allocation failure in every cross-version transition.
// Inputs: All six shipped legacy-version transitions, each followed by a successful retry.
// Outputs: Requires unchanged ownership on error, publication only on success, and no leaks or invalid frees.
TEST_CASE(zstd_legacy_version_transition_allocation_failures) {
    for (const auto previous : kLegacyVersions) {
        for (const auto version : kLegacyVersions) {
            if (version == previous) {
                continue;
            }
            for (const auto fail_after : {1U, 2U}) {
                REQUIRE_TRUE(sz_fault_reset(0));
                {
                    LegacyOwner owner{sz_legacy_create(previous), previous};
                    REQUIRE_TRUE(owner.context != nullptr);
                    const auto original = owner.context;
                    REQUIRE_TRUE(sz_fault_fail_after(fail_after));
                    REQUIRE_EQ(sz_legacy_initialize(&owner.context, previous, version, nullptr, 0), 1);
                    REQUIRE_EQ(owner.context, original);
                    REQUIRE_EQ(sz_fault_live_allocations(), 2U);
                    REQUIRE_TRUE(sz_fault_fail_after(0));
                    REQUIRE_EQ(sz_legacy_initialize(&owner.context, previous, version, nullptr, 0), 0);
                    owner.version = version;
                    REQUIRE_TRUE(owner.context != original);
                    REQUIRE_EQ(sz_fault_live_allocations(), 2U);
                }
                REQUIRE_EQ(sz_fault_live_allocations(), 0U);
                REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
            }
        }
    }
}

// Purpose: Reinitialize a same-version owner without allocation or ownership churn.
// Inputs: Every shipped legacy version with the next allocation forced to fail.
// Outputs: Requires success, the same pointer, and leak-free destruction.
TEST_CASE(zstd_legacy_same_version_reuses_owner) {
    for (const auto version : kLegacyVersions) {
        REQUIRE_TRUE(sz_fault_reset(0));
        {
            LegacyOwner owner{sz_legacy_create(version), version};
            REQUIRE_TRUE(owner.context != nullptr);
            const auto original = owner.context;
            REQUIRE_TRUE(sz_fault_fail_after(1));
            REQUIRE_EQ(sz_legacy_initialize(&owner.context, version, version, nullptr, 0), 0);
            REQUIRE_EQ(owner.context, original);
            REQUIRE_EQ(sz_fault_live_allocations(), 2U);
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}

// Purpose: Preserve valid owned buffer state after one streaming allocation fails, then reinitialize safely.
// Inputs: version is 5-7, fail_after selects the input or output allocation, and pinned bytes provide the oracle.
// Outputs: Requires valid capacity/storage, exact retried output and complete cleanup without an unsafe baseline retry.
void check_stream_allocation_failure(unsigned version, std::size_t fail_after) {
    const auto extent = superzip_test::kLegacyFrames[version - 4U];
    const auto frame = std::span(superzip_test::kLegacyCompressed).subspan(extent.offset, extent.size);
    REQUIRE_TRUE(sz_fault_reset(0U));
    {
        LegacyOwner owner{nullptr, version};
        REQUIRE_EQ(sz_legacy_initialize(&owner.context, 0U, version, nullptr, 0U), 0);
        REQUIRE_TRUE(owner.context != nullptr);
        REQUIRE_TRUE(sz_fault_fail_after(fail_after));
        std::array<unsigned char, 512> output{};
        auto input_bytes = frame.size();
        auto output_bytes = output.size();
        std::size_t hint = 0;
        REQUIRE_EQ(
            sz_legacy_decode(owner.context, version, output.data(), &output_bytes, frame.data(), &input_bytes, &hint),
            1);
        REQUIRE_TRUE(sz_legacy_buffers_consistent(owner.context, version));
        REQUIRE_TRUE(sz_fault_fail_after(0U));
        REQUIRE_EQ(sz_legacy_initialize(&owner.context, version, version, nullptr, 0U), 0);
        input_bytes = frame.size();
        output_bytes = output.size();
        REQUIRE_EQ(
            sz_legacy_decode(owner.context, version, output.data(), &output_bytes, frame.data(), &input_bytes, &hint),
            0);
        REQUIRE_EQ(hint, 0U);
        REQUIRE_EQ(input_bytes, frame.size());
        REQUIRE_EQ(output_bytes, superzip_test::kLegacyExpected.size());
        REQUIRE_TRUE(std::ranges::equal(std::span(output).first(output_bytes), superzip_test::kLegacyExpected));
        REQUIRE_TRUE(sz_legacy_buffers_consistent(owner.context, version));
    }
    REQUIRE_EQ(sz_fault_live_allocations(), 0U);
    REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
}

// Purpose: Verify v0.5 input-buffer allocation failure and retry; no inputs, requires valid ownership.
TEST_CASE(zstd_legacy_v05_stream_input_allocation_failure) {
    check_stream_allocation_failure(5U, 1U);
}

// Purpose: Verify v0.5 output-buffer allocation failure and retry; no inputs, requires valid ownership.
TEST_CASE(zstd_legacy_v05_stream_output_allocation_failure) {
    check_stream_allocation_failure(5U, 2U);
}

// Purpose: Verify v0.6 input-buffer allocation failure and retry; no inputs, requires valid ownership.
TEST_CASE(zstd_legacy_v06_stream_input_allocation_failure) {
    check_stream_allocation_failure(6U, 1U);
}

// Purpose: Verify v0.6 output-buffer allocation failure and retry; no inputs, requires valid ownership.
TEST_CASE(zstd_legacy_v06_stream_output_allocation_failure) {
    check_stream_allocation_failure(6U, 2U);
}

// Purpose: Verify v0.7 input-buffer allocation failure and retry; no inputs, requires valid ownership.
TEST_CASE(zstd_legacy_v07_stream_input_allocation_failure) {
    check_stream_allocation_failure(7U, 1U);
}

// Purpose: Verify v0.7 output-buffer allocation failure and retry; no inputs, requires valid ownership.
TEST_CASE(zstd_legacy_v07_stream_output_allocation_failure) {
    check_stream_allocation_failure(7U, 2U);
}

// Purpose: Decode a complete pinned frame through the exact interposed stream and check its independent oracle.
// Inputs: owner is initialized and frame encodes the pinned legacy plaintext with a permitted window variation.
// Outputs: Requires full consumption, end-of-frame, exact bytes and valid buffer ownership.
void require_legacy_stream_readback(const LegacyOwner& owner, std::span<const unsigned char> frame) {
    std::array<unsigned char, 512> output{};
    auto input_bytes = frame.size();
    auto output_bytes = output.size();
    std::size_t hint = 0;
    REQUIRE_EQ(
        sz_legacy_decode(owner.context, owner.version, output.data(), &output_bytes, frame.data(), &input_bytes, &hint),
        0);
    REQUIRE_EQ(hint, 0U);
    REQUIRE_EQ(input_bytes, frame.size());
    REQUIRE_EQ(output_bytes, superzip_test::kLegacyExpected.size());
    REQUIRE_TRUE(std::ranges::equal(std::span(output).first(output_bytes), superzip_test::kLegacyExpected));
    REQUIRE_TRUE(sz_legacy_buffers_consistent(owner.context, owner.version));
}

// Purpose: Retain both previously initialized buffer owners when a larger frame needs storage and acquisition fails.
// Inputs: version is 5-7; pinned frame bytes retain their payload while the format's window declaration grows.
// Outputs: Requires exact owner/capacity preservation, successful reinitialization and retry, and complete cleanup.
void check_stream_buffer_growth_failure(unsigned version) {
    const auto extent = superzip_test::kLegacyFrames[version - 4U];
    const auto frame = std::span(superzip_test::kLegacyCompressed).subspan(extent.offset, extent.size);
    std::vector<unsigned char> larger(frame.begin(), frame.end());
    if (version == 7U) {
        REQUIRE_EQ(larger[4], 0x20U);
        larger[4] = 0x00U;  // Replace direct content-size mode with an explicit window and unknown content size.
        larger[5] = 0x48U;  // 512 KiB, leaving the original block payload and extent unchanged.
    } else {
        larger[4] = static_cast<unsigned char>((larger[4] & 0xF0U) | 7U);
    }
    for (const auto fail_after : {1U, 2U}) {
        if (version == 5U && fail_after == 2U) {
            continue;  // Version five always owns a full-size input buffer, so only its output buffer grows.
        }
        REQUIRE_TRUE(sz_fault_reset(0U));
        {
            LegacyOwner owner{nullptr, version};
            REQUIRE_EQ(sz_legacy_initialize(&owner.context, 0U, version, nullptr, 0U), 0);
            require_legacy_stream_readback(owner, frame);
            const auto original = sz_legacy_get_buffer_state(owner.context, version);
            const auto live = sz_fault_live_allocations();
            REQUIRE_EQ(sz_legacy_initialize(&owner.context, version, version, nullptr, 0U), 0);
            REQUIRE_TRUE(sz_fault_fail_after(fail_after));
            std::array<unsigned char, 512> output{};
            auto input_bytes = larger.size();
            auto output_bytes = output.size();
            std::size_t hint = 0;
            REQUIRE_EQ(sz_legacy_decode(owner.context, version, output.data(), &output_bytes, larger.data(),
                                        &input_bytes, &hint),
                       1);
            const auto retained = sz_legacy_get_buffer_state(owner.context, version);
            REQUIRE_EQ(retained.input, original.input);
            REQUIRE_EQ(retained.input_capacity, original.input_capacity);
            REQUIRE_EQ(retained.output, original.output);
            REQUIRE_EQ(retained.output_capacity, original.output_capacity);
            REQUIRE_EQ(sz_fault_live_allocations(), live);
            REQUIRE_TRUE(sz_fault_fail_after(0U));
            REQUIRE_EQ(sz_legacy_initialize(&owner.context, version, version, nullptr, 0U), 0);
            require_legacy_stream_readback(owner, larger);
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}

// Purpose: Verify v0.5 output-buffer growth failure and retry; no inputs, requires retained owners and exact output.
TEST_CASE(zstd_legacy_v05_stream_growth_preserves_buffers) {
    check_stream_buffer_growth_failure(5U);
}

// Purpose: Verify v0.6 input/output-buffer growth failures and retry; no inputs, requires retained owners and output.
TEST_CASE(zstd_legacy_v06_stream_growth_preserves_buffers) {
    check_stream_buffer_growth_failure(6U);
}

// Purpose: Verify v0.7 input/output-buffer growth failures and retry; no inputs, requires retained owners and output.
TEST_CASE(zstd_legacy_v07_stream_growth_preserves_buffers) {
    check_stream_buffer_growth_failure(7U);
}

// Purpose: Exercise empty legacy public buffers through header accumulation and output backpressure.
// Inputs: All shipped buffered decoder versions and independent upstream golden frames.
// Outputs: Requires zero consumption on empty input, bounded progress and exact read-back after output resumes.
TEST_CASE(zstd_legacy_stream_empty_buffers_preserve_progress) {
    for (const auto version : kLegacyVersions) {
        REQUIRE_TRUE(sz_fault_reset(0U));
        {
            LegacyOwner owner{nullptr, version};
            REQUIRE_EQ(sz_legacy_initialize(&owner.context, 0U, version, nullptr, 0U), 0);
            std::size_t input_bytes = 0;
            std::size_t output_bytes = 0;
            std::size_t hint = 0;
            REQUIRE_EQ(sz_legacy_decode(owner.context, version, nullptr, &output_bytes, nullptr, &input_bytes, &hint),
                       0);
            REQUIRE_EQ(input_bytes, 0U);
            REQUIRE_EQ(output_bytes, 0U);
            REQUIRE_TRUE(hint > 0U);

            const auto extent = superzip_test::kLegacyFrames[version - 4U];
            const auto frame = std::span(superzip_test::kLegacyCompressed).subspan(extent.offset, extent.size);
            input_bytes = 1U;
            REQUIRE_EQ(
                sz_legacy_decode(owner.context, version, nullptr, &output_bytes, frame.data(), &input_bytes, &hint), 0);
            REQUIRE_EQ(input_bytes, 1U);
            input_bytes = 0;
            REQUIRE_EQ(sz_legacy_decode(owner.context, version, nullptr, &output_bytes, nullptr, &input_bytes, &hint),
                       0);
            REQUIRE_EQ(input_bytes, 0U);
            REQUIRE_EQ(output_bytes, 0U);

            input_bytes = frame.size() - 1U;
            REQUIRE_EQ(sz_legacy_decode(owner.context, version, nullptr, &output_bytes, frame.data() + 1U, &input_bytes,
                                        &hint),
                       0);
            REQUIRE_TRUE(input_bytes <= frame.size() - 1U);
            REQUIRE_EQ(output_bytes, 0U);
            auto position = 1U + input_bytes;
            std::array<unsigned char, 512> output{};
            std::size_t produced = 0;
            for (unsigned attempt = 0; hint != 0U && attempt < 16U; ++attempt) {
                input_bytes = frame.size() - position;
                output_bytes = output.size() - produced;
                const auto* input = input_bytes != 0U ? frame.data() + position : nullptr;
                REQUIRE_EQ(sz_legacy_decode(owner.context, version, output.data() + produced, &output_bytes, input,
                                            &input_bytes, &hint),
                           0);
                REQUIRE_TRUE(input_bytes <= frame.size() - position);
                REQUIRE_TRUE(output_bytes <= output.size() - produced);
                position += input_bytes;
                produced += output_bytes;
            }
            REQUIRE_EQ(hint, 0U);
            REQUIRE_EQ(position, frame.size());
            REQUIRE_EQ(produced, superzip_test::kLegacyExpected.size());
            REQUIRE_TRUE(std::ranges::equal(std::span(output).first(produced), superzip_test::kLegacyExpected));
            REQUIRE_TRUE(sz_legacy_buffers_consistent(owner.context, version));
        }
        REQUIRE_EQ(sz_fault_live_allocations(), 0U);
        REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
    }
}

// Purpose: Propagate malformed-dictionary initialization errors without publishing a wrong-version owner.
// Inputs: A truncated dictionary with the destination version's actual magic, for all nine version pairs.
// Outputs: Requires an error, retained previous ownership, successful no-dictionary retry and complete cleanup.
TEST_CASE(zstd_legacy_dictionary_failure_preserves_owner) {
    for (const auto previous : kLegacyVersions) {
        for (const auto version : kLegacyVersions) {
            const std::array<unsigned char, 8> dictionary{
                static_cast<unsigned char>(0x30U + version), 0xA4, 0x30, 0xEC, 0, 0, 0, 0};
            REQUIRE_TRUE(sz_fault_reset(0));
            {
                LegacyOwner owner{sz_legacy_create(previous), previous};
                REQUIRE_TRUE(owner.context != nullptr);
                const auto original = owner.context;
                REQUIRE_EQ(
                    sz_legacy_initialize(&owner.context, previous, version, dictionary.data(), dictionary.size()), 1);
                REQUIRE_EQ(owner.context, original);
                REQUIRE_EQ(sz_fault_live_allocations(), 2U);
                REQUIRE_EQ(sz_legacy_initialize(&owner.context, previous, version, nullptr, 0), 0);
                owner.version = version;
            }
            REQUIRE_EQ(sz_fault_live_allocations(), 0U);
            REQUIRE_EQ(sz_fault_invalid_frees(), 0U);
        }
    }
}
