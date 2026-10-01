#include "core/resource_limit_checks.hpp"
#include "core/resource_usage.hpp"

#include "test_util.hpp"

#include <cstdint>
#include <array>
#include <limits>

using superzip::reconcile_vram_usage;

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

// Purpose: Prevent multi-adapter and similarly prefixed process counters from contaminating selected-GPU VRAM.
// Inputs: Adapter-wide and process counters for two LUIDs, two physical nodes, and PIDs 42/420.
// Outputs: Requires exact LUID/PID totals and keeps unavailable data distinct from valid zero usage.
TEST_CASE(selected_gpu_memory_matches_adapter_and_process_identity) {
    using superzip::GpuMemorySample;
    constexpr std::uint64_t selected = 0x12345678000146daULL;
    const std::array samples{
        GpuMemorySample{L"luid_0x12345678_0x000146da_phys_0", 100},
        GpuMemorySample{L"luid_0x12345678_0x000146da_phys_1", 200},
        GpuMemorySample{L"luid_0x12345678_0x000177de_phys_0", 900},
        GpuMemorySample{L"pid_42_luid_0x12345678_0x000146da_phys_0", 20},
        GpuMemorySample{L"pid_42_luid_0x12345678_0x000146da_phys_1", 30},
        GpuMemorySample{L"pid_420_luid_0x12345678_0x000146da_phys_0", 80},
        GpuMemorySample{L"pid_42_luid_0x12345678_0x000177de_phys_0", 70},
    };
    REQUIRE_EQ(superzip::selected_gpu_memory_usage(samples, selected).value(), 300U);
    REQUIRE_EQ(superzip::selected_gpu_memory_usage(samples, selected, 42U).value(), 50U);
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage(samples, 0U).has_value());
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage(samples, selected, 4U).has_value());
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage({}, selected).has_value());
    const std::array idle{GpuMemorySample{L"luid_0x12345678_0x000146da_phys_0", 0}};
    REQUIRE_EQ(superzip::selected_gpu_memory_usage(idle, selected).value(), 0U);
}

// Purpose: Reject malformed identities and invalid byte totals rather than clamping cross-adapter data.
// Inputs: Invalid numeric fields, negative matching counters, and three counters whose sum overflows.
// Outputs: Requires malformed/unrelated data to be excluded and invalid matching totals to stay unavailable.
TEST_CASE(selected_gpu_memory_rejects_malformed_negative_and_overflowing_data) {
    using superzip::GpuMemorySample;
    constexpr auto valid = L"luid_0x00000000_0x000146da_phys_0";
    const std::array invalid{
        GpuMemorySample{L"luid_0x100000000_0x000146da_phys_0", 900},
        GpuMemorySample{L"luid_0x0_0x100000000_phys_0", 900},
        GpuMemorySample{L"luid_0x0_0x000146dg_phys_0", 900},
        GpuMemorySample{L"luid_0x0_0x000146da_phys_", 900},
        GpuMemorySample{L"luid_0x0_0x000146da_phys_0_extra", 900},
        GpuMemorySample{L"pid_4294967296_luid_0x0_0x000146da_phys_0", 900},
        GpuMemorySample{L"pid_x_luid_0x0_0x000146da_phys_0", 900},
        GpuMemorySample{L"luid_0x0_0x000177de_phys_0", -1},
    };
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage(invalid, 0x146daU).has_value());
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage(invalid, 0x146daU, 42U).has_value());
    const std::array negative{GpuMemorySample{valid, -1}};
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage(negative, 0x146daU).has_value());
    const std::array overflowing{
        GpuMemorySample{valid, std::numeric_limits<std::int64_t>::max()},
        GpuMemorySample{L"luid_0x0_0x000146da_phys_1", std::numeric_limits<std::int64_t>::max()},
        GpuMemorySample{L"luid_0x0_0x000146da_phys_2", 2},
    };
    REQUIRE_TRUE(!superzip::selected_gpu_memory_usage(overflowing, 0x146daU).has_value());
}

// Purpose: Detect incorrect summation across independent GPU engines while including every process.
// Inputs: Two processes sharing one engine, another engine, and a second physical GPU.
// Outputs: Requires the maximum engine total, not the largest process or sum of all engine totals.
TEST_CASE(system_gpu_utilization_groups_processes_before_selecting_busiest_engine) {
    const std::array samples{
        superzip::GpuEngineSample{L"pid_42_luid_0x00000000_0x000146da_phys_0_eng_0_engtype_3D", 40.0},
        superzip::GpuEngineSample{L"pid_420_luid_0x00000000_0x000146da_phys_0_eng_0_engtype_3D", 30.0},
        superzip::GpuEngineSample{L"pid_42_luid_0x00000000_0x000146da_phys_0_eng_1_engtype_Copy", 60.0},
        superzip::GpuEngineSample{L"pid_42_luid_0x00000000_0x000156da_phys_0_eng_0_engtype_3D", 55.0},
        superzip::GpuEngineSample{L"pid_42_luid_0x00000000_0x000146da_phys_1_eng_0_engtype_3D", 65.0},
    };
    REQUIRE_EQ(superzip::system_gpu_utilization(samples).value(), 70.0);
}

// Purpose: Keep absent or invalid performance counters distinct from a genuinely idle GPU.
// Inputs: Invalid instance names, nonfinite/out-of-range counters, an idle sample, and an oversubscribed engine.
// Outputs: Requires unavailable data to stay absent and a valid idle engine to report zero.
TEST_CASE(system_gpu_utilization_rejects_invalid_and_inconsistent_counters) {
    using superzip::GpuEngineSample;
    constexpr auto valid_name = L"pid_42_luid_0x00000000_0x000146da_phys_0_eng_0_engtype_3D";
    const std::array invalid{
        GpuEngineSample{L"not-an-engine", 90.0},
        GpuEngineSample{L"pid_x_luid_0x00000000_0x000146da_phys_0_eng_0_engtype_3D", 90.0},
        GpuEngineSample{L"pid_42_luid_0x00000000_0x000146da_eng_0_engtype_3D", 90.0},
        GpuEngineSample{L"pid_42_luid_0x00000000_0x000146da_phys_0_eng__engtype_3D", 90.0},
        GpuEngineSample{valid_name, std::numeric_limits<double>::quiet_NaN()},
        GpuEngineSample{valid_name, std::numeric_limits<double>::infinity()},
        GpuEngineSample{valid_name, -1.0},
        GpuEngineSample{valid_name, 101.0},
    };
    REQUIRE_TRUE(!superzip::system_gpu_utilization(invalid).has_value());
    REQUIRE_TRUE(!superzip::system_gpu_utilization({}).has_value());
    const std::array idle{GpuEngineSample{valid_name, 0.0}};
    REQUIRE_EQ(superzip::system_gpu_utilization(idle).value(), 0.0);
    const std::array saturated{GpuEngineSample{valid_name, 100.0}};
    REQUIRE_EQ(superzip::system_gpu_utilization(saturated).value(), 100.0);
    const std::array inconsistent{
        GpuEngineSample{valid_name, 80.0},
        GpuEngineSample{L"pid_420_luid_0x00000000_0x000146da_phys_0_eng_0_engtype_3D", 40.0},
    };
    REQUIRE_TRUE(!superzip::system_gpu_utilization(inconsistent).has_value());
}

TEST_CASE(vram_reconciliation_keeps_process_usage_under_total_usage) {
    const auto usage = reconcile_vram_usage(16ULL * 1024ULL * kMiB, (16ULL * 1024ULL * kMiB) - (151ULL * kMiB),
                                            151ULL * kMiB, 183ULL * kMiB);
    REQUIRE_EQ(usage.total_used_bytes, 183ULL * kMiB);
    REQUIRE_EQ(usage.process_dedicated_bytes, 183ULL * kMiB);
}

TEST_CASE(vram_reconciliation_clamps_untrusted_counters_to_capacity) {
    const auto usage = reconcile_vram_usage(256ULL * kMiB, 128ULL * kMiB, 1024ULL * kMiB, 512ULL * kMiB);
    REQUIRE_EQ(usage.total_used_bytes, 256ULL * kMiB);
    REQUIRE_EQ(usage.process_dedicated_bytes, 256ULL * kMiB);
}

TEST_CASE(vram_reconciliation_handles_unknown_capacity) {
    const auto usage = reconcile_vram_usage(0U, 0U, 64ULL * kMiB, 96ULL * kMiB);
    REQUIRE_EQ(usage.total_used_bytes, 96ULL * kMiB);
    REQUIRE_EQ(usage.process_dedicated_bytes, 96ULL * kMiB);
}

// Purpose: Verify decoded-output accounting rejects totals above SuperZip's extraction policy limit.
// Inputs: A byte count at the policy limit plus one additional decoded byte.
// Outputs: Throws if the extracted-output cap is not enforced.
TEST_CASE(extracted_output_accounting_rejects_policy_limit_excess) {
    bool rejected = false;
    try {
        (void)superzip::checked_add_extracted_output_bytes(superzip::kMaxExtractedOutputBytes, 1U, "test output");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace
