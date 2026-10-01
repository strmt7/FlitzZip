#include "test_util.hpp"

#include "core/archive.hpp"
#include "core/archive_blocks.hpp"
#include "core/parallel_ranges.hpp"
#include "core/result.hpp"
#include "core/worker_budget.hpp"

#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>

// Purpose: Verify balanced scheduling without allocating storage proportional to the logical item count.
// Inputs: count is a logical extent through SIZE_MAX; workers is a bounded requested task count.
// Outputs: Requires complete, disjoint, nonempty ranges with sizes differing by at most one.
void require_balanced_ranges(std::size_t count, std::uint32_t workers) {
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    std::mutex mutex;
    superzip::run_parallel_ranges(count, workers, [&](std::size_t begin, std::size_t end) {
        std::lock_guard lock(mutex);
        ranges.emplace_back(begin, end);
    });
    std::sort(ranges.begin(), ranges.end());
    const auto expected = std::min<std::size_t>(count, std::max(1U, workers));
    REQUIRE_EQ(ranges.size(), expected);
    std::size_t covered = 0;
    for (const auto& [begin, end] : ranges) {
        REQUIRE_EQ(begin, covered);
        REQUIRE_TRUE(end > begin && end <= count);
        REQUIRE_TRUE(end - begin >= count / expected);
        REQUIRE_TRUE(end - begin - count / expected <= 1U);
        covered = end;
    }
    REQUIRE_EQ(covered, count);
}

// Purpose: Keep production and RAM validation on one overflow-safe aggregate worker allocation policy.
// Inputs: Empty work, uneven queue depths, short entries, and extreme unsigned counts.
// Outputs: Requires exact floor allocation and the same worker-capped queue limit.
TEST_CASE(shared_codec_worker_budget_boundaries) {
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 32U, 80U), 1U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 4U, 80U), 8U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 4U, 1U), 32U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 0U, 0U), 32U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(64U, 17U, 80U), 3U);
    REQUIRE_EQ(superzip::resolve_worker_inflight_limit(32U, 64U, 80U), 32U);
    REQUIRE_EQ(superzip::resolve_worker_inflight_limit(0U, 0U, 0U), 1U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(std::numeric_limits<std::uint32_t>::max(), 1U, 1U),
               std::numeric_limits<std::uint32_t>::max());
    REQUIRE_EQ(superzip::resolve_codec_worker_count(std::numeric_limits<std::uint32_t>::max(),
                                                    std::numeric_limits<std::uint32_t>::max(),
                                                    std::numeric_limits<std::uint64_t>::max()),
               1U);
}

// Purpose: Prevent aggregate oversubscription independently of hardware, memory pressure, or scheduler timing.
// Inputs: All workers/inflight in [0,64] and available window counts in [0,80].
// Outputs: Requires positive shares, bounded queue admission, and aggregate workers within the normalized limit.
TEST_CASE(shared_codec_worker_budget_aggregate_matrix) {
    for (std::uint32_t workers = 0; workers <= 64U; ++workers) {
        for (std::uint32_t inflight = 0; inflight <= 64U; ++inflight) {
            for (std::uint64_t windows = 0; windows <= 80U; ++windows) {
                const auto active = superzip::resolve_worker_inflight_limit(workers, inflight, windows);
                const auto share = superzip::resolve_codec_worker_count(workers, inflight, windows);
                REQUIRE_TRUE(active >= 1U && share >= 1U);
                REQUIRE_TRUE(active <= std::max(1U, workers));
                REQUIRE_TRUE(active <= std::max(1U, inflight));
                REQUIRE_TRUE(active <= std::max<std::uint64_t>(1U, windows));
                REQUIRE_TRUE(static_cast<std::uint64_t>(active) * share <= std::max(1U, workers));
            }
        }
    }
}

// Purpose: Cover uneven, empty, short, and maximum-size logical range partitions using the production dispatcher.
// Inputs: Counts [0,17], workers [0,9], and SIZE_MAX without iterating or allocating that many items.
// Outputs: Requires one visit per item by partition geometry and no empty worker calls or unsigned overflow.
TEST_CASE(parallel_ranges_balanced_extents) {
    for (std::size_t count = 0; count <= 17U; ++count) {
        for (std::uint32_t workers = 0; workers <= 9U; ++workers) {
            require_balanced_ranges(count, workers);
        }
    }
    require_balanced_ranges(std::numeric_limits<std::size_t>::max(), 3U);
    require_balanced_ranges(std::numeric_limits<std::size_t>::max(), 4U);
}

// Purpose: Preserve borrowed callable/storage lifetime when either an async range or the caller range throws.
// Inputs: Four independent ranges; either the first asynchronous range or the calling-thread range fails.
// Outputs: Requires the original exception and completion of every other range before unwinding reaches the caller.
TEST_CASE(parallel_ranges_join_before_exception_escape) {
    for (const auto failing_begin : {0U, 6U}) {
        std::atomic<std::uint32_t> completed{0};
        bool threw = false;
        try {
            superzip::run_parallel_ranges(8U, 4U, [&](std::size_t begin, std::size_t) {
                if (begin == failing_begin) {
                    throw std::runtime_error("range failure");
                }
                ++completed;
            });
        } catch (const std::runtime_error& error) {
            threw = std::string(error.what()) == "range failure";
        }
        REQUIRE_TRUE(threw);
        REQUIRE_EQ(completed.load(), 3U);
    }
}

// Purpose: Keep complete CPU block representations identical when balanced range geometry changes context reuse.
// Inputs: Ten mixed blocks plus a short tail; serial encoding and worker counts 2,3,4,9 at effort five.
// Outputs: Requires identical payload/descriptors and byte-exact independent CPU materialization for every geometry.
TEST_CASE(cpu_codec_worker_geometry_preserves_encoded_bytes) {
    constexpr auto block_size = superzip::kMinArchiveBlockBytes;
    std::vector<std::byte> input(10U * block_size + 127U);
    std::uint32_t state = 0x92F71461U;
    for (std::size_t i = 0; i < input.size(); ++i) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        const auto family = (i / block_size) % 3U;
        input[i] = family == 0U ? std::byte{'F'} : static_cast<std::byte>(family == 1U ? i % 7U : state & 255U);
    }
    superzip::ArchiveCodecOptions options{.block_size = block_size, .worker_count = 1U, .compression_level = 5};
    const auto baseline = superzip::encode_chunk_cpu(input, options);
    for (const auto workers : {2U, 3U, 4U, 9U}) {
        options.worker_count = workers;
        const auto encoded = superzip::encode_chunk_cpu(input, options);
        REQUIRE_EQ(encoded.payload, baseline.payload);
        REQUIRE_EQ(encoded.blocks.size(), baseline.blocks.size());
        for (std::size_t i = 0; i < encoded.blocks.size(); ++i) {
            const auto& actual = encoded.blocks[i];
            const auto& expected = baseline.blocks[i];
            REQUIRE_EQ(actual.kind, expected.kind);
            REQUIRE_EQ(actual.fill_value, expected.fill_value);
            REQUIRE_EQ(actual.uncompressed_len, expected.uncompressed_len);
            REQUIRE_EQ(actual.encoded_offset, expected.encoded_offset);
            REQUIRE_EQ(actual.encoded_len, expected.encoded_len);
        }
        std::vector<std::byte> output(input.size());
        superzip::decode_chunk_cpu(encoded.payload, encoded.blocks, output, options);
        REQUIRE_EQ(output, input);
    }
}

// Purpose: Keep concurrency admission identical to real complete-block grouping, including uneven decoded lengths.
// Inputs: Seven six-byte blocks under a ten-byte window, exact fits, empty blocks, and extreme byte bounds.
// Outputs: Requires exact count/extent and rejects zero or oversized lengths without unsigned wraparound.
TEST_CASE(decode_block_window_exact_admission) {
    std::array<superzip::BlockDescriptor, 7> blocks{};
    for (auto& block : blocks) {
        block.uncompressed_len = 6U;
    }
    REQUIRE_EQ(superzip::count_decode_block_windows(blocks, 10U), 7U);
    REQUIRE_EQ(superzip::count_decode_block_windows(blocks, 12U), 4U);
    REQUIRE_EQ(superzip::count_decode_block_windows(blocks, 42U), 1U);
    const auto first = superzip::resolve_decode_block_window(blocks, 0, 12U);
    REQUIRE_EQ(first.end, 2U);
    REQUIRE_EQ(first.uncompressed_size, 12U);
    REQUIRE_EQ(superzip::count_decode_block_windows({}, 1U), 0U);
    bool invalid_index = false;
    try {
        (void)superzip::resolve_decode_block_window(blocks, blocks.size() + 1U, 12U);
    } catch (const superzip::ArchiveError&) {
        invalid_index = true;
    }
    REQUIRE_TRUE(invalid_index);
    for (auto& block : blocks) {
        block.uncompressed_len = std::numeric_limits<std::uint32_t>::max();
    }
    REQUIRE_EQ(superzip::count_decode_block_windows(blocks, std::numeric_limits<std::uint64_t>::max()), 1U);
    for (const auto bad_limit : {0ULL, 1ULL}) {
        bool threw = false;
        try {
            (void)superzip::count_decode_block_windows(blocks, bad_limit);
        } catch (const superzip::ArchiveError&) {
            threw = true;
        }
        REQUIRE_TRUE(threw);
    }
    blocks.front().uncompressed_len = 0U;
    bool threw = false;
    try {
        (void)superzip::count_decode_block_windows(blocks, std::numeric_limits<std::uint64_t>::max());
    } catch (const superzip::ArchiveError&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

// Purpose: Exercise one production backend when requested queue depth exceeds its aggregate workers.
// Inputs: use_gpu selects required HIP or CPU; multi-chunk input is read back through a smaller decode window.
// Outputs: Requires depth two and exact bytes in all operations, with HIP telemetry when requested; cleans closed
// files.
void require_pipeline_worker_cap_roundtrip(bool use_gpu) {
    const auto root = test_temp_dir(use_gpu ? "gpu-worker-cap-roundtrip" : "cpu-worker-cap-roundtrip");
    const auto source = root / "input.bin";
    const auto archive = root / "input.suzip";
    std::vector<std::byte> input(3U * 1024U * 1024U + 17U);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>(i % 251U);
    }
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size()));
        REQUIRE_TRUE(file.good());
    }
    superzip::CompressOptions compress;
    compress.force_cpu = !use_gpu;
    compress.gpu_required = use_gpu;
    compress.worker_count = 2U;
    compress.max_inflight_chunks = 8U;
    compress.chunk_size = 1024U * 1024U;
    compress.block_size = superzip::kMinArchiveBlockBytes;
    compress.verify_after_write = true;
    const auto created = superzip::compress_suzip({source}, archive, compress);
    REQUIRE_EQ(created.inflight_chunks, 2U);
    REQUIRE_EQ(created.gpu_used, use_gpu);
    if (use_gpu) {
        REQUIRE_TRUE(created.gpu_runtime.kernel_launches > 0U);
    }
    superzip::ExtractOptions extract;
    extract.force_cpu = !use_gpu;
    extract.gpu_required = use_gpu;
    extract.worker_count = 2U;
    extract.max_inflight_chunks = 8U;
    extract.chunk_size = superzip::kMinArchiveBlockBytes;
    extract.block_size = superzip::kMinArchiveBlockBytes;
    REQUIRE_EQ(superzip::verify_suzip(archive, extract).inflight_chunks, 2U);
    REQUIRE_EQ(superzip::extract_suzip(archive, root / "decoded", extract).inflight_chunks, 2U);
    {
        std::ifstream decoded(root / "decoded" / "input.bin", std::ios::binary);
        std::vector<std::byte> output(input.size());
        decoded.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        REQUIRE_EQ(decoded.gcount(), static_cast<std::streamsize>(input.size()));
        REQUIRE_EQ(output, input);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify worker-capped production queues in CPU-only CI and on available HIP development systems.
// Inputs: Both backend policies; required HIP runs only when the backend and device are available.
// Outputs: Requires byte-exact creation, verification, and extraction without a CPU fallback in the GPU lane.
TEST_CASE(suzip_pipeline_worker_cap_roundtrip) {
    require_pipeline_worker_cap_roundtrip(false);
    if (superzip::query_gpu_info().available) {
        require_pipeline_worker_cap_roundtrip(true);
    }
}
