#include "test_util.hpp"
#include "fault_allocator.h"

#include <array>

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

}  // namespace

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
