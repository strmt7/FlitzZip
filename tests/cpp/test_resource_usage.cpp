#include "core/resource_usage.hpp"

#include "test_util.hpp"

#include <cstdint>

using superzip::reconcile_vram_usage;

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

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

}  // namespace
