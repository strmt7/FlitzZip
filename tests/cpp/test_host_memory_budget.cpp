#include "test_util.hpp"

#include "core/host_memory_budget.hpp"
#include "core/archive_blocks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "zstd/zstd_runtime.hpp"

#include <array>
#include <limits>
#include <memory>

namespace {

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

// Purpose: Construct an exact target-growth fixture without changing the real host's memory usage.
// Inputs: growth is no greater than the fixture's 80 GiB usage target.
// Outputs: Returns a valid 100 GiB physical-memory snapshot with exactly growth bytes below the target.
superzip::HostMemorySnapshot memory_with_growth(std::uint64_t growth) {
    REQUIRE_TRUE(growth <= 80U * kGiB);
    return {.total_bytes = 100U * kGiB, .available_bytes = 20U * kGiB + growth};
}

// Purpose: Exercise admission failures without permitting unrelated exception classes to pass.
// Inputs: memory, chunk_size, and reserve are synthetic admission arguments.
// Outputs: Returns true only when the shared policy throws ArchiveError.
bool admission_rejected(superzip::HostMemorySnapshot memory, std::uint64_t chunk_size, std::uint64_t reserve = 0,
                        superzip::HostPipelineWorkspace workspace = {}) {
    try {
        (void)superzip::resolve_host_pipeline_inflight_limit(memory, chunk_size, reserve, workspace);
    } catch (const superzip::ArchiveError&) {
        return true;
    }
    return false;
}

}  // namespace

// Purpose: Validate exact target boundaries, odd totals, invalid counters, and unsigned overflow limits.
// Inputs: Synthetic physical-memory snapshots, independent of this workstation's RAM size.
// Outputs: Requires exact growth and rejection of unavailable or contradictory memory counters.
TEST_CASE(host_memory_growth_boundaries) {
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({1000U, 1000U}), 800U);
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({1000U, 201U}), 1U);
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({1000U, 200U}), 0U);
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({1000U, 199U}), 0U);
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({103U, 103U}), 82U);
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({1U, 1U}), 0U);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_EQ(superzip::safe_host_memory_growth_bytes({maximum, maximum}), 14757395258967641292ULL);
    REQUIRE_TRUE(admission_rejected({0U, 0U}, 4096U));
    REQUIRE_TRUE(admission_rejected({1000U, 1001U}, 4096U));
}

// Purpose: Prevent forced minimum-depth admission below the three-buffer working-set estimate.
// Inputs: All representative native chunk sizes and growth just below, at, and above each window boundary.
// Outputs: Requires refusal below one window and exact capped integer depths above it, with no host allocations.
TEST_CASE(host_memory_pipeline_window_boundaries) {
    constexpr std::array<std::uint64_t, 7> chunks{
        4096U,
        64U * 1024U,
        256U * 1024U,
        8U * 1024U * 1024U,
        16U * 1024U * 1024U,
        64U * 1024U * 1024U,
        superzip::kMaxArchiveChunkBytes,
    };
    for (const auto chunk : chunks) {
        const auto window = 3U * chunk;
        REQUIRE_TRUE(admission_rejected(memory_with_growth(0U), chunk));
        REQUIRE_TRUE(admission_rejected(memory_with_growth(window - 1U), chunk));
        REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(window), chunk), 1U);
        REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(2U * window - 1U), chunk), 1U);
        REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(2U * window), chunk), 2U);
        REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(65U * window), chunk), 64U);
    }
}

// Purpose: Keep RAM benchmark reserve additive rather than discounting a native processing buffer.
// Inputs: The default native chunk and exact reserve/window boundaries, including an oversized reserve.
// Outputs: Requires the same window estimate as production, with refusal when only two buffers fit.
TEST_CASE(host_memory_pipeline_reserve_parity) {
    constexpr auto chunk = superzip::kMaxArchiveChunkBytes;
    constexpr auto reserve = kGiB;
    REQUIRE_TRUE(admission_rejected(memory_with_growth(reserve), chunk, reserve));
    REQUIRE_TRUE(admission_rejected(memory_with_growth(reserve + 2U * chunk), chunk, reserve));
    REQUIRE_TRUE(admission_rejected(memory_with_growth(reserve + 3U * chunk - 1U), chunk, reserve));
    REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(reserve + 3U * chunk), chunk, reserve),
               1U);
    const auto memory = memory_with_growth(reserve + 6U * chunk);
    REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory, chunk, reserve), 2U);
    REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(6U * chunk), chunk), 2U);
    REQUIRE_TRUE(admission_rejected(memory, chunk, std::numeric_limits<std::uint64_t>::max()));
}

// Purpose: Verify the buffer admission inequality across small and large host configurations.
// Inputs: Eight host sizes, varying physical usage, all representative chunk sizes, and optional reserves.
// Outputs: Every admitted depth must fit the independent growth bound and never exceed the public queue ceiling.
TEST_CASE(host_memory_pipeline_configuration_matrix) {
    constexpr std::array<std::uint64_t, 8> totals{2U, 4U, 8U, 16U, 32U, 64U, 128U, 1024U};
    constexpr std::array<std::uint64_t, 5> chunks{4096U, 256U * 1024U, 8U * 1024U * 1024U, 16U * 1024U * 1024U,
                                                  superzip::kMaxArchiveChunkBytes};
    constexpr std::array<std::uint64_t, 2> reserves{0U, kGiB};
    for (const auto gib : totals) {
        const auto total = gib * kGiB;
        for (std::uint64_t used_percent = 0; used_percent <= 100U; ++used_percent) {
            const auto used = (total / 100U) * used_percent;
            const superzip::HostMemorySnapshot memory{total, total - used};
            // Independent reference: these fixture totals permit multiplication before division.
            const auto target = (total * 4U) / 5U;
            const auto growth = used >= target ? 0U : target - used;
            for (const auto chunk : chunks) {
                for (const auto reserve : reserves) {
                    if (growth < reserve || growth - reserve < 3U * chunk) {
                        REQUIRE_TRUE(admission_rejected(memory, chunk, reserve));
                        continue;
                    }
                    const auto depth = superzip::resolve_host_pipeline_inflight_limit(memory, chunk, reserve);
                    REQUIRE_TRUE(depth >= 1U && depth <= superzip::kMaxInflightArchiveChunks);
                    REQUIRE_TRUE(reserve + depth * 3U * chunk <= growth);
                    const auto expected =
                        std::min<std::uint64_t>((growth - reserve) / (3U * chunk), superzip::kMaxInflightArchiveChunks);
                    REQUIRE_EQ(depth, expected);
                }
            }
        }
    }
}

// Purpose: Reject invalid windows and preserve full-width depth arithmetic before narrowing.
// Inputs: Zero/oversized windows and a maximum unsigned physical-memory snapshot.
// Outputs: Requires ArchiveError for invalid windows and capped depth for huge valid counters.
TEST_CASE(host_memory_pipeline_invalid_and_extreme_windows) {
    const auto memory = memory_with_growth(10U * kGiB);
    REQUIRE_TRUE(admission_rejected(memory, 0U));
    REQUIRE_TRUE(admission_rejected(memory, superzip::kMaxArchiveChunkBytes + 1U));
    REQUIRE_TRUE(admission_rejected(memory, std::numeric_limits<std::uint64_t>::max()));
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit({maximum, maximum}, 1U),
               superzip::kMaxInflightArchiveChunks);
}

// Purpose: Check the actual Windows counter path without assuming an idle host or minimum free RAM.
// Inputs: One current physical-memory snapshot from the supported production platform.
// Outputs: Requires a valid snapshot; does not require that a processing window is currently admissible.
TEST_CASE(host_memory_snapshot_windows_query) {
    const auto memory = superzip::query_host_memory_snapshot();
    REQUIRE_TRUE(memory.total_bytes > 0U);
    REQUIRE_TRUE(memory.available_bytes <= memory.total_bytes);
    REQUIRE_TRUE(superzip::safe_host_memory_growth_bytes(memory) <= memory.available_bytes);
}

// Purpose: Require metadata and aggregate codec contexts at exact admission boundaries.
// Inputs: Synthetic per-worker/window costs, including saturation at eight aggregate workers.
// Outputs: Requires rejection one byte below each boundary and the greatest fitting queue depth.
TEST_CASE(host_memory_pipeline_workspace_boundaries) {
    constexpr std::uint64_t chunk = 4096U;
    constexpr std::uint64_t reserve = 123U;
    const superzip::HostPipelineWorkspace workspace{77U, 1024U, 8U, 4U};
    for (std::uint32_t depth = 1; depth <= 64U; ++depth) {
        const auto contexts = std::min<std::uint64_t>(8U, depth * 4U);
        const auto exact = reserve + depth * (3U * chunk + 77U) + contexts * 1024U;
        REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(exact), chunk, reserve, workspace),
                   depth);
        if (depth == 1U) {
            REQUIRE_TRUE(admission_rejected(memory_with_growth(exact - 1U), chunk, reserve, workspace));
        } else {
            REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(exact - 1U), chunk, reserve,
                                                                      workspace),
                       depth - 1U);
        }
    }
    REQUIRE_EQ(superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(3U * chunk), chunk), 1U);
    REQUIRE_TRUE(admission_rejected(memory_with_growth(3U * chunk), chunk, 0U, workspace));
}

// Purpose: Bound aggregate workspace over every worker/queue geometry without allocating codec memory.
// Inputs: Worker ceilings 1..64, per-window fan-out 1..4, and exact growth for depths 1..64.
// Outputs: Requires maximal admitted depth and an independent total-storage inequality in every geometry.
TEST_CASE(host_memory_pipeline_workspace_geometry) {
    constexpr std::uint64_t chunk = 256U * 1024U;
    for (std::uint32_t workers = 1; workers <= 64U; ++workers) {
        for (std::uint32_t fanout = 1; fanout <= 4U; ++fanout) {
            const superzip::HostPipelineWorkspace workspace{4096U, 13U * 1024U * 1024U, workers, fanout};
            for (std::uint32_t depth = 1; depth <= 64U; ++depth) {
                const auto contexts = std::min<std::uint64_t>(workers, static_cast<std::uint64_t>(depth) * fanout);
                const auto growth = depth * (3U * chunk + 4096U) + contexts * workspace.per_worker_bytes;
                const auto admitted =
                    superzip::resolve_host_pipeline_inflight_limit(memory_with_growth(growth), chunk, 0U, workspace);
                REQUIRE_EQ(admitted, depth);
                REQUIRE_TRUE(admitted * (3U * chunk + workspace.per_window_bytes) +
                                 std::min<std::uint64_t>(workers, static_cast<std::uint64_t>(admitted) * fanout) *
                                     workspace.per_worker_bytes <=
                             growth);
            }
        }
    }
}

// Purpose: Reject malformed workspace geometry and full-width allocation arithmetic before overflow.
// Inputs: Contradictory/oversized ceilings and maximum unsigned workspace sizes.
// Outputs: Requires ArchiveError rather than a wrapped or forced nonzero queue depth.
TEST_CASE(host_memory_pipeline_workspace_invalid) {
    const auto memory = memory_with_growth(10U * kGiB);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto workspace : std::array<superzip::HostPipelineWorkspace, 6>{{{0U, 1U, 0U, 0U},
                                                                                {0U, 0U, 1U, 0U},
                                                                                {0U, 0U, 0U, 1U},
                                                                                {0U, 1U, 65U, 4U},
                                                                                {0U, 1U, 64U, 65U},
                                                                                {maximum, 0U, 0U, 0U}}}) {
        REQUIRE_TRUE(admission_rejected(memory, 4096U, 0U, workspace));
    }
    REQUIRE_TRUE(admission_rejected(memory, 4096U, 0U, {0U, maximum, 64U, 4U}));
}

// Purpose: Check production CPU estimates against real retained contexts across nested effort changes.
// Inputs: Non-fill 4 KiB, 256 KiB, and 1 MiB blocks through all nine native effort levels.
// Outputs: Requires the pinned context bound to cover actual runtime storage and exact decoded bytes.
TEST_CASE(host_memory_cpu_workspace_runtime_bound) {
    const auto& runtime = superzip::zstd_runtime();
    const auto release = [&runtime](superzip::ZstdCompressionContext* context) {
        runtime.free_compression_context(context);
    };
    for (const std::uint32_t bytes : {4096U, 256U * 1024U, 1024U * 1024U}) {
        std::unique_ptr<superzip::ZstdCompressionContext, decltype(release)> context(
            runtime.create_compression_context(), release);
        REQUIRE_TRUE(context != nullptr);
        std::vector<std::byte> source(bytes);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<std::byte>((index * 73U + index / 257U) & 255U);
        }
        std::vector<std::byte> encoded(runtime.block_compress_bound(bytes));
        std::vector<std::byte> decoded(bytes);
        for (int level = 1; level <= 9; ++level) {
            const auto written = runtime.compress_block_with_context(context.get(), encoded.data(), encoded.size(),
                                                                     source.data(), source.size(), level);
            REQUIRE_TRUE(!runtime.is_error(written));
            REQUIRE_TRUE(written <= encoded.size());
            const auto bound = runtime.estimate_block_workspace_bytes(level);
            REQUIRE_TRUE(!runtime.is_error(bound));
            REQUIRE_TRUE(runtime.compression_workspace_bytes(context.get()) <= bound);
            const auto restored = runtime.decompress_block(decoded.data(), decoded.size(), encoded.data(), written);
            REQUIRE_EQ(restored, bytes);
            REQUIRE_TRUE(decoded == source);
            const auto workspace =
                superzip::cpu_encode_workspace_estimate(bytes, {.block_size = bytes, .compression_level = level}, 64U);
            REQUIRE_TRUE(workspace.per_worker_bytes > bound);
            REQUIRE_TRUE(workspace.per_window_bytes >= 2U * sizeof(superzip::BlockDescriptor));
            REQUIRE_EQ(workspace.workers_per_window, 1U);
        }
    }
}

// Purpose: Account for tiny Deflate-only windows and bounded maximum-size block metadata.
// Inputs: Tiny windows, a maximum native window, and invalid estimator resource options.
// Outputs: Requires zero codec overhead only below the framing threshold and bounded per-window fan-out.
TEST_CASE(host_memory_cpu_workspace_small_and_invalid_windows) {
    const auto tiny = superzip::cpu_encode_workspace_estimate(8U, {}, 64U);
    REQUIRE_EQ(tiny.per_worker_bytes, 0U);
    REQUIRE_TRUE(tiny.per_window_bytes > 0U);
    const auto short_block = superzip::cpu_encode_workspace_estimate(9U, {}, 64U);
    REQUIRE_TRUE(short_block.per_worker_bytes > 0U);
    const auto full = superzip::cpu_encode_workspace_estimate(superzip::kMaxArchiveChunkBytes,
                                                              {.block_size = superzip::kMinArchiveBlockBytes}, 64U);
    REQUIRE_EQ(full.workers_per_window, superzip::kMaxCpuEncodeWorkersPerWindow);
    REQUIRE_TRUE(full.per_window_bytes >= 32768U * 2U * sizeof(superzip::BlockDescriptor));
    for (const auto chunk : {0ULL, superzip::kMaxArchiveChunkBytes + 1U}) {
        bool rejected = false;
        try {
            (void)superzip::cpu_encode_workspace_estimate(chunk, {}, 64U);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}
