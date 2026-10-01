#include "test_util.hpp"

#include "gpu/gpu_codec.hpp"
#include "gpu/hip_device.hpp"
#include "gpu/pinned_host_budget.hpp"
#include "core/checksum.hpp"
#include "core/decoded_chunk.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "core/worker_budget.hpp"

#include <array>
#include <barrier>
#include <cmath>
#include <future>
#include <limits>
#include <thread>
#include <utility>

// Purpose: Keep production and RAM validation on one overflow-safe per-window worker allocation policy.
// Inputs: Empty work, ordinary CPU/GPU depths, short entries, and extreme unsigned counts.
// Outputs: Requires exact shared allocation and overflow-safe ceiling division.
TEST_CASE(shared_codec_worker_budget_boundaries) {
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 32U, 80U), 1U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 4U, 80U), 8U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 4U, 1U), 32U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(32U, 0U, 0U), 32U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(1U, 64U, 80U), 1U);
    REQUIRE_EQ(superzip::resolve_codec_worker_count(std::numeric_limits<std::uint32_t>::max(), 1U, 1U),
               std::numeric_limits<std::uint32_t>::max());
    REQUIRE_EQ(superzip::resolve_codec_worker_count(std::numeric_limits<std::uint32_t>::max(),
                                                    std::numeric_limits<std::uint32_t>::max(),
                                                    std::numeric_limits<std::uint64_t>::max()),
               1U);
}

// Purpose: Verify actual copied host output is checksummed with the admitted parallel CPU budget after HIP decode.
// Inputs: Required-HIP raw blocks with an irregular tail and a multi-task checksum extent.
// Outputs: Requires full byte equality and serial CRC equality; no device-produced CRC substitutes for host validation.
TEST_CASE(owned_decoded_chunk_parallel_crc_validates_host_output) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    constexpr std::uint32_t block_size = 16U * 1024U * 1024U;
    constexpr std::uint32_t tail = 37U;
    std::vector<std::byte> payload(block_size + tail);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::byte>((index * 31U + index / 997U) & 255U);
    }
    const std::array<superzip::BlockDescriptor, 2> blocks{{
        {.kind = superzip::BlockKind::Raw, .uncompressed_len = block_size, .encoded_len = block_size},
        {.kind = superzip::BlockKind::Raw, .uncompressed_len = tail, .encoded_offset = block_size, .encoded_len = tail},
    }};
    superzip::GpuCodecOptions options;
    options.block_size = block_size;
    options.worker_count = 8U;
    options.host_output_pool = superzip::make_owned_decode_pool(options);
    const auto decoded = superzip::decode_owned_chunk(payload, blocks, payload.size(), options);
    REQUIRE_TRUE(decoded.gpu_used);
    REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), payload));
    REQUIRE_EQ(decoded.crc32, superzip::crc32(payload));
}

// Purpose: Enforce host-relative and absolute pin limits without overflow or dropped concurrent reservations.
// Inputs: Independent budgets, shrinking RAM snapshots, oversized requests, and eight simultaneous contenders.
// Outputs: Requires exact bounded admission, balanced release, and reuse without a HIP device dependency.
TEST_CASE(pinned_host_budget_admission_and_concurrency) {
    superzip::PinnedHostBudget budget;
    constexpr std::uint64_t mib = 1024U * 1024U;
    REQUIRE_TRUE(!budget.try_reserve(0, 8192U * mib));
    REQUIRE_TRUE(!budget.try_reserve(1, 15));
    REQUIRE_EQ(superzip::PinnedHostBudget::allowance(1024U * mib), 64U * mib);
    REQUIRE_EQ(superzip::PinnedHostBudget::allowance(std::numeric_limits<std::uint64_t>::max()),
               superzip::PinnedHostBudget::maximum_bytes);
    REQUIRE_TRUE(
        !budget.try_reserve(std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(budget.try_reserve(32U * mib, 1024U * mib));
    REQUIRE_TRUE(!budget.try_reserve(1, 512U * mib));
    REQUIRE_TRUE(budget.try_reserve(32U * mib, 1024U * mib));
    REQUIRE_TRUE(!budget.try_reserve(1, 1024U * mib));
    budget.release(64U * mib);
    std::barrier start(8);
    std::barrier reserved(8);
    std::atomic<unsigned> admitted{0};
    std::array<std::future<void>, 8> workers;
    for (auto& worker : workers) {
        worker = std::async(std::launch::async, [&] {
            start.arrive_and_wait();
            const auto owned = budget.try_reserve(128U * mib, std::numeric_limits<std::uint64_t>::max());
            if (owned) {
                admitted.fetch_add(1U, std::memory_order_relaxed);
            }
            reserved.arrive_and_wait();
            if (owned) {
                budget.release(128U * mib);
            }
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    REQUIRE_EQ(admitted.load(), 4U);
    REQUIRE_EQ(budget.reserved_bytes(), 0U);
    REQUIRE_TRUE(budget.try_reserve(superzip::PinnedHostBudget::maximum_bytes, 8192U * mib));
    budget.release(superzip::PinnedHostBudget::maximum_bytes);
}

// Purpose: Preserve CPU concurrency while required-HIP owned output respects live aggregate pin capacity.
// Inputs: CPU/automatic/required-HIP options, zero/large windows, and requested depths above and below capacity.
// Outputs: Requires bounded admission and consistent capacity-derived slots without requiring a GPU device.
TEST_CASE(owned_decoded_chunk_concurrency_admission_policy) {
    superzip::GpuCodecOptions options;
    REQUIRE_TRUE(superzip::hip_host_output_capacity_bytes() <= superzip::PinnedHostBudget::maximum_bytes);
    const auto admitted = superzip::resolve_owned_decode_inflight(32U, 128U * 1024U * 1024U, options);
    REQUIRE_TRUE(admitted >= 1U && admitted <= 4U);
    REQUIRE_EQ(superzip::resolve_owned_decode_inflight(32U, superzip::PinnedHostBudget::maximum_bytes + 1U, options),
               1U);
    REQUIRE_EQ(superzip::resolve_owned_decode_inflight(1U, 128U * 1024U * 1024U, options), 1U);
    REQUIRE_EQ(superzip::resolve_owned_decode_inflight(32U, 0U, options), 32U);
    options.force_cpu = true;
    REQUIRE_EQ(superzip::resolve_owned_decode_inflight(32U, 128U * 1024U * 1024U, options), 32U);
    options.force_cpu = false;
    options.require_gpu = false;
    REQUIRE_EQ(superzip::resolve_owned_decode_inflight(32U, 128U * 1024U * 1024U, options), 32U);
}

// Purpose: Prove real pinned decode ownership survives concurrent workers and destruction on another thread.
// Inputs: Four distinct 4 MiB required-HIP raw chunks, independent telemetry, and the normal host pin admission.
// Outputs: Requires exact restored bytes/CRC, pin telemetry matching admitted owners, and unchanged release accounting.
TEST_CASE(owned_decoded_chunk_pinned_concurrent_cross_thread_release) {
    const auto gpu = superzip::query_gpu_info();
    if (!gpu.available) {
        return;
    }
    const auto before = superzip::snapshot_hip_pinned_host_stats();
    std::array<std::future<superzip::DecodedChunk>, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::async(std::launch::async, [index] {
            constexpr std::size_t size = 4U * 1024U * 1024U;
            std::vector<std::byte> payload(size, static_cast<std::byte>(index + 1U));
            const superzip::BlockDescriptor block{
                .kind = superzip::BlockKind::Raw,
                .uncompressed_len = size,
                .encoded_len = size,
            };
            superzip::GpuCodecOptions options;
            options.block_size = size;
            options.telemetry = std::make_shared<superzip::GpuTelemetry>();
            auto decoded = superzip::decode_owned_chunk(payload, std::span(&block, 1U), size, options);
            REQUIRE_TRUE(decoded.gpu_used);
            REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), payload));
            REQUIRE_EQ(decoded.crc32, superzip::crc32(payload));
            const auto pinned = decoded.storage.get_deleter().release == superzip::release_hip_host_output;
            REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*options.telemetry).host_pinned_allocation_bytes,
                       pinned ? size : 0U);
            return decoded;
        });
    }
    for (auto& worker : workers) {
        auto owned = worker.get();
        REQUIRE_EQ(owned.bytes().size(), 4U * 1024U * 1024U);
    }
    const auto after = superzip::snapshot_hip_pinned_host_stats();
    REQUIRE_EQ(after.reserved_bytes, before.reserved_bytes);
    REQUIRE_EQ(after.release_failures, before.release_failures);
    REQUIRE_EQ(superzip::query_gpu_info().runtime_version, gpu.runtime_version);
}

// Purpose: Preserve CPU-only decode and release pinned storage when admitted output fails validation.
// Inputs: A CPU Zstandard block in automatic mode and a malformed HIP raw payload with bounded output.
// Outputs: Requires no pinning for CPU decode, ArchiveError for corruption, and unchanged aggregate pin accounting.
TEST_CASE(owned_decoded_chunk_pinned_failure_and_cpu_only_policy) {
    constexpr std::size_t size = 1024U * 1024U;
    std::vector<std::byte> input(size);
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<std::byte>(index & 15U);
    }
    superzip::GpuCodecOptions cpu;
    cpu.force_cpu = true;
    cpu.require_gpu = false;
    cpu.block_size = size;
    const auto encoded = superzip::encode_chunk(input, cpu);
    REQUIRE_EQ(encoded.blocks.front().kind, superzip::BlockKind::CpuZstd);
    superzip::GpuCodecOptions automatic = cpu;
    automatic.force_cpu = false;
    automatic.telemetry = std::make_shared<superzip::GpuTelemetry>();
    const auto before = superzip::snapshot_hip_pinned_host_stats();
    const auto decoded = superzip::decode_owned_chunk(encoded.payload, encoded.blocks, size, automatic);
    REQUIRE_TRUE(!decoded.gpu_used);
    REQUIRE_TRUE(decoded.storage.get_deleter().release == nullptr);
    REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), input));
    REQUIRE_EQ(superzip::snapshot_gpu_telemetry(*automatic.telemetry).host_pinned_allocation_bytes, 0U);
    if (superzip::query_gpu_info().available) {
        for (const bool pooled : {false, true}) {
            superzip::GpuCodecOptions gpu;
            if (pooled) {
                gpu.host_output_pool = superzip::make_owned_decode_pool(gpu);
            }
            const superzip::BlockDescriptor invalid{
                .kind = superzip::BlockKind::Raw, .uncompressed_len = size, .encoded_len = size};
            bool rejected = false;
            try {
                (void)superzip::decode_owned_chunk({}, std::span(&invalid, 1U), size, gpu);
            } catch (const superzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
    }
    const auto after = superzip::snapshot_hip_pinned_host_stats();
    REQUIRE_EQ(after.reserved_bytes, before.reserved_bytes);
    REQUIRE_EQ(after.release_failures, before.release_failures);
    REQUIRE_TRUE(superzip::try_allocate_hip_host_output(superzip::kMaxArchiveChunkBytes + 1U) == nullptr);
}

// Purpose: Prove cache reuse, mixed-size admission, cross-thread ownership, and cleanup after the operation drops its
// pool. Inputs: Bounded required-HIP raw output, a smaller reused extent, and final owners retained beyond operation
// scope. Outputs: Requires exact bytes/CRC, truthful fresh/reused telemetry, and reservation restoration after the last
// owner.
TEST_CASE(owned_decoded_chunk_pinned_pool_reuse_and_lifetime) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    const auto before = superzip::snapshot_hip_pinned_host_stats();
    superzip::GpuCodecOptions options;
    options.telemetry = std::make_shared<superzip::GpuTelemetry>();
    options.host_output_pool = superzip::make_owned_decode_pool(options);
    constexpr std::size_t large_size = 4U * 1024U * 1024U;
    const std::vector<std::byte> payload(large_size, std::byte{0x19});
    const superzip::BlockDescriptor large{
        .kind = superzip::BlockKind::Raw, .uncompressed_len = large_size, .encoded_len = large_size};
    auto first = superzip::decode_owned_chunk(payload, std::span(&large, 1U), large_size, options);
    const auto pinned = first.storage.get_deleter().pool != nullptr;
    auto* first_pointer = first.storage.get();
    first = {};
    constexpr std::size_t small_size = 1024U * 1024U;
    const superzip::BlockDescriptor small{
        .kind = superzip::BlockKind::Raw, .uncompressed_len = small_size, .encoded_len = small_size};
    auto second = std::async(std::launch::async, [&] {
                      return superzip::decode_owned_chunk(std::span(payload).first(small_size), std::span(&small, 1U),
                                                          small_size, options);
                  }).get();
    REQUIRE_TRUE(std::ranges::equal(second.bytes(), std::span(payload).first(small_size)));
    REQUIRE_EQ(second.crc32, superzip::crc32(std::span(payload).first(small_size)));
    if (pinned) {
        REQUIRE_TRUE(second.storage.get() == first_pointer);
        REQUIRE_TRUE(!second.storage.get_deleter().new_pinned_allocation);
        const auto stats = superzip::snapshot_gpu_telemetry(*options.telemetry);
        REQUIRE_EQ(stats.host_pinned_allocation_bytes, large_size);
        REQUIRE_EQ(stats.host_pinned_output_bytes, large_size + small_size);
    }
    options.host_output_pool.reset();
    second = {};
    const auto after = superzip::snapshot_hip_pinned_host_stats();
    REQUIRE_EQ(after.reserved_bytes, before.reserved_bytes);
    REQUIRE_EQ(after.release_failures, before.release_failures);
    options.force_cpu = true;
    REQUIRE_TRUE(superzip::make_owned_decode_pool(options) == nullptr);
    options.force_cpu = false;
    options.require_gpu = false;
    REQUIRE_TRUE(superzip::make_owned_decode_pool(options) == nullptr);
}

// Purpose: Prove that owned decoding fills every byte and remains safe when ownership moves.
// Inputs: Fill, raw, and irregular-tail blocks through CPU and, when present, required HIP.
// Outputs: Requires exact bytes, CRC, backend identity, and an empty moved-from view.
TEST_CASE(owned_decoded_chunk_roundtrip_and_move) {
    constexpr std::size_t block_size = 4096U;
    std::vector<std::byte> input(block_size * 2U + 17U, std::byte{0x7b});
    for (std::size_t i = block_size; i < input.size(); ++i) {
        input[i] = static_cast<std::byte>((i * 31U + i / 97U) & 255U);
    }
    const auto gpu_available = superzip::query_gpu_info().available;
    for (const bool use_gpu : {false, true}) {
        if (use_gpu && !gpu_available) {
            continue;
        }
        superzip::GpuCodecOptions options;
        options.force_cpu = !use_gpu;
        options.require_gpu = use_gpu;
        options.block_size = block_size;
        options.compression_level = 1;
        const auto encoded = superzip::encode_chunk(input, options);
        auto decoded = superzip::decode_owned_chunk(encoded.payload, encoded.blocks, input.size(), options);
        REQUIRE_TRUE(std::ranges::equal(decoded.bytes(), input));
        REQUIRE_EQ(decoded.crc32, superzip::crc32(input));
        REQUIRE_EQ(decoded.gpu_used, use_gpu);
        auto moved = std::move(decoded);
        REQUIRE_TRUE(decoded.bytes().empty());
        REQUIRE_TRUE(std::ranges::equal(moved.bytes(), input));
        superzip::DecodedChunk assigned;
        assigned = std::move(moved);
        REQUIRE_TRUE(moved.bytes().empty());
        REQUIRE_TRUE(std::ranges::equal(assigned.bytes(), input));
    }
}

// Purpose: Reject malformed output extents before returning any owned decoded bytes.
// Inputs: Zero/oversized blocks, missing/excess output, invalid encoding kinds, and truncated raw payload.
// Outputs: Requires ArchiveError and a well-defined empty owner for an empty chunk.
TEST_CASE(owned_decoded_chunk_rejects_invalid_layouts) {
    superzip::GpuCodecOptions options;
    options.require_gpu = false;
    options.force_cpu = true;
    const std::array payload{std::byte{0x42}};
    const std::array<std::pair<superzip::BlockDescriptor, std::size_t>, 8> invalid = {{
        {{.kind = superzip::BlockKind::Fill, .uncompressed_len = 0}, 1},
        {{.kind = superzip::BlockKind::Fill, .uncompressed_len = superzip::kMaxArchiveBlockBytes + 1U}, 1},
        {{.kind = superzip::BlockKind::Fill, .uncompressed_len = 1}, 0},
        {{.kind = superzip::BlockKind::Fill, .uncompressed_len = 1}, 2},
        {{.kind = superzip::BlockKind::Fill, .uncompressed_len = 1}, superzip::kMaxArchiveChunkBytes + 1U},
        {{.kind = superzip::BlockKind::Raw, .uncompressed_len = 2, .encoded_len = 2}, 2},
        {{.kind = static_cast<superzip::BlockKind>(255), .uncompressed_len = 1}, 1},
        {{.kind = superzip::BlockKind::Fill, .uncompressed_len = 1, .encoded_len = 1}, 1},
    }};
    for (const auto& [block, size] : invalid) {
        bool rejected = false;
        try {
            (void)superzip::decode_owned_chunk(payload, std::span(&block, 1U), size, options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    const auto empty = superzip::decode_owned_chunk({}, {}, 0U, options);
    REQUIRE_TRUE(empty.bytes().empty());
    REQUIRE_EQ(empty.crc32, 0U);
    REQUIRE_TRUE(!empty.gpu_used);
}

// Purpose: Preserve required-GPU refusal rather than decoding a CPU-only block into the new owner.
// Inputs: CPU-generated Deflate and Zstandard blocks with required HIP and no fallback permission.
// Outputs: Requires GpuError on both HIP-enabled and CPU-only builds.
TEST_CASE(owned_decoded_chunk_required_gpu_rejects_cpu_blocks) {
    for (const auto size : {1024U, 4096U}) {
        std::vector<std::byte> input(size);
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::byte>(i & 15U);
        }
        superzip::GpuCodecOptions cpu;
        cpu.require_gpu = false;
        cpu.force_cpu = true;
        cpu.block_size = 4096U;
        const auto encoded = superzip::encode_chunk(input, cpu);
        REQUIRE_EQ(encoded.blocks.front().kind,
                   size == 1024U ? superzip::BlockKind::Deflate : superzip::BlockKind::CpuZstd);
        superzip::GpuCodecOptions gpu;
        gpu.require_gpu = true;
        gpu.block_size = 4096U;
        bool rejected = false;
        try {
            (void)superzip::decode_owned_chunk(encoded.payload, encoded.blocks, input.size(), gpu);
        } catch (const superzip::GpuError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Keep allocator compatibility decisions tied to the loaded numeric DLL version rather than SDK labels.
// Inputs: Missing metadata, the reproducing older runtime, minimum-version boundaries, and newer numeric versions.
// Outputs: Requires conservative legacy eligibility and exact path-free dotted diagnostic formatting.
TEST_CASE(gpu_stream_allocator_runtime_version_admission) {
    using superzip::HipRuntimeVersion;
    REQUIRE_TRUE(!superzip::hip_runtime_allows_stream_allocations(std::nullopt));
    for (const auto version : {HipRuntimeVersion{10, 0, 3665, 0}, HipRuntimeVersion{10, 0, 3678, 65535},
                               HipRuntimeVersion{9, 65535, 65535, 65535}}) {
        REQUIRE_TRUE(!superzip::hip_runtime_allows_stream_allocations(version));
    }
    for (const auto version : {HipRuntimeVersion{10, 0, 3679, 0}, HipRuntimeVersion{10, 0, 3679, 1},
                               HipRuntimeVersion{10, 1, 0, 0}, HipRuntimeVersion{11, 0, 0, 0}}) {
        REQUIRE_TRUE(superzip::hip_runtime_allows_stream_allocations(version));
    }
    REQUIRE_EQ(superzip::hip_runtime_version_text(std::nullopt), std::string("unavailable"));
    REQUIRE_EQ(superzip::hip_runtime_version_text(HipRuntimeVersion{10, 0, 3679, 0}), std::string("10.0.3679.0"));
}

// Purpose: Detect allocation cross-talk and incomplete frees on joined production codec streams.
// Inputs: Four 128 MiB distinct fill streams using required HIP, plus the exact loaded runtime's capability state.
// Outputs: Requires byte-exact independent roundtrips and zero live pool bytes after all operation owners finish.
TEST_CASE(gpu_concurrent_large_allocations_preserve_data_and_release_pool) {
    const auto before = superzip::query_gpu_info();
    if (!before.available) {
        return;
    }
    std::array<std::future<void>, 4> workers;
    for (std::size_t worker = 0; worker < workers.size(); ++worker) {
        workers[worker] = std::async(std::launch::async, [worker] {
            const std::vector<std::byte> input(128U * 1024U * 1024U, static_cast<std::byte>(worker + 1U));
            superzip::GpuCodecOptions options;
            options.require_gpu = true;
            options.compression_level = 1;
            const auto encoded = superzip::encode_chunk(input, options);
            REQUIRE_TRUE(encoded.gpu_used);
            REQUIRE_TRUE(encoded.source_crc32_available);
            REQUIRE_EQ(encoded.source_crc32, superzip::crc32(input));
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    const auto after = superzip::query_gpu_info();
    REQUIRE_TRUE(after.available);
    REQUIRE_TRUE(after.runtime_version == before.runtime_version);
    if (after.stream_ordered_allocator_supported) {
        REQUIRE_TRUE(after.pool_used_bytes.has_value());
        REQUIRE_EQ(after.pool_used_bytes.value(), 0U);
    }
}

// Purpose: Reject non-finite or out-of-range diagnostic durations before any HIP work.
// Inputs: NaN, infinities, invalid endpoints, and invalid device-work admission values.
// Outputs: Requires ArchiveError independently of compiled backend or installed GPU hardware.
TEST_CASE(gpu_diagnostic_invalid_options_fail_before_dispatch) {
    const std::array durations = {std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  0.0,
                                  31.0,
                                  std::nextafter(1.0, 0.0),
                                  std::nextafter(30.0, 31.0)};
    for (const auto seconds : durations) {
        superzip::GpuDiagnosticOptions options;
        options.seconds = seconds;
        bool rejected = false;
        try {
            (void)superzip::run_gpu_diagnostic(options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    for (const auto buffer : {0U, 15U, 513U}) {
        superzip::GpuDiagnosticOptions options;
        options.buffer_mib = buffer;
        bool rejected = false;
        try {
            (void)superzip::run_gpu_diagnostic(options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    for (const auto iterations : {0U, 4097U}) {
        superzip::GpuDiagnosticOptions options;
        options.inner_iterations = iterations;
        bool rejected = false;
        try {
            (void)superzip::run_gpu_diagnostic(options);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Preserve thread-local device selection and explicit missing-backend errors in readiness admission.
// Inputs: The current compiled backend; available HIP is checked concurrently without changing device selection.
// Outputs: Requires stable diagnostics on HIP, or the precise CPU-only GpuError on hosted validation builds.
TEST_CASE(gpu_device_readiness_contract) {
#if SUPERZIP_ENABLE_HIP
    const auto before = superzip::query_gpu_info();
    if (!before.available) {
        return;
    }
    std::array<std::future<void>, 4> workers;
    for (auto& worker : workers) {
        worker = std::async(std::launch::async, [] {
            const auto first = superzip::query_gpu_info();
            REQUIRE_TRUE(first.available);
            superzip::require_hip_device_ready();
            superzip::require_hip_device_ready();
            const auto second = superzip::query_gpu_info();
            REQUIRE_TRUE(second.available);
            REQUIRE_EQ(first.selected_device, second.selected_device);
            REQUIRE_EQ(first.device_count, second.device_count);
            REQUIRE_EQ(first.gcn_arch, second.gcn_arch);
        });
    }
    for (auto& worker : workers) {
        worker.get();
    }
    superzip::require_hip_device_ready();
    REQUIRE_EQ(before.selected_device, superzip::query_gpu_info().selected_device);
#else
    bool rejected = false;
    try {
        superzip::require_hip_device_ready();
    } catch (const superzip::GpuError& error) {
        rejected = true;
        REQUIRE_EQ(std::string(error.what()), "Built without HIP acceleration");
    }
    REQUIRE_TRUE(rejected);
#endif
}

// Purpose: Preserve finite timing precision and distinguish real zero duration from unavailable data.
// Inputs: Synthetic event durations; no HIP device or workload is required.
// Outputs: Requires nearest-microsecond rounding, correct launch counts, and a finite empty snapshot.
TEST_CASE(gpu_telemetry_finite_event_rounding) {
    superzip::GpuTelemetry telemetry;
    REQUIRE_TRUE(std::abs(superzip::snapshot_gpu_telemetry(telemetry).kernel_ms) < 1e-12);
    superzip::record_gpu_dictionary_blocks(&telemetry, 2U);
    superzip::record_gpu_sparse_pattern_blocks(&telemetry, 3U);
    for (const auto value : {0.0, -0.0, 0.00049, 0.00050, 1.234}) {
        superzip::record_gpu_kernel_launch(&telemetry, value);
    }
    const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(stats.kernel_launches, 5U);
    REQUIRE_EQ(stats.dictionary_blocks, 2U);
    REQUIRE_EQ(stats.sparse_pattern_blocks, 3U);
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), 1235U);
    REQUIRE_TRUE(std::abs(stats.kernel_ms - 1.235) < 1e-12);
    superzip::record_gpu_kernel_launch(nullptr, std::numeric_limits<double>::quiet_NaN());
}

// Purpose: Preserve independently named HIP encode worker-stage totals under concurrent accumulation.
// Inputs: Four joined writers record synthetic durations; null, zero, negative, and sentinel stages are ignored.
// Outputs: Requires exact summed seconds for populated stages and zero for untouched stages.
TEST_CASE(gpu_encode_worker_stage_accumulation) {
    superzip::GpuTelemetry telemetry;
    {
        std::vector<std::jthread> workers;
        for (int worker = 0; worker < 4; ++worker) {
            workers.emplace_back([&telemetry] {
                for (int event = 0; event < 1000; ++event) {
                    superzip::record_gpu_encode_stage_time(&telemetry, superzip::GpuEncodeStage::Readiness,
                                                           std::chrono::milliseconds(1));
                    superzip::record_gpu_encode_stage_time(&telemetry, superzip::GpuEncodeStage::Dictionary,
                                                           std::chrono::milliseconds(2));
                }
            });
        }
    }
    superzip::record_gpu_encode_stage_time(nullptr, superzip::GpuEncodeStage::Prefix, std::chrono::milliseconds(1));
    superzip::record_gpu_encode_stage_time(&telemetry, superzip::GpuEncodeStage::Prefix, std::chrono::milliseconds(-1));
    superzip::record_gpu_encode_stage_time(&telemetry, superzip::GpuEncodeStage::Count, std::chrono::milliseconds(1));
    const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_TRUE(
        std::abs(stats.encode_stage_worker_seconds[static_cast<std::size_t>(superzip::GpuEncodeStage::Readiness)] -
                 4.0) < 1e-12);
    REQUIRE_TRUE(
        std::abs(stats.encode_stage_worker_seconds[static_cast<std::size_t>(superzip::GpuEncodeStage::Dictionary)] -
                 8.0) < 1e-12);
    REQUIRE_TRUE(
        std::abs(stats.encode_stage_worker_seconds[static_cast<std::size_t>(superzip::GpuEncodeStage::Prefix)]) <
        1e-12);
}

// Purpose: Keep invalid HIP event values out of integer conversion and preserve unavailable timing thereafter.
// Inputs: Negative, non-finite, and out-of-range synthetic durations followed by a valid duration.
// Outputs: Requires NaN timing, intact execution counters, and a sticky unavailable marker without throwing.
TEST_CASE(gpu_telemetry_invalid_events_remain_unavailable) {
    for (const auto value :
         {-0.000001, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), std::ldexp(1.0, 64) / 1000.0}) {
        superzip::GpuTelemetry telemetry;
        superzip::record_gpu_encode_chunk(&telemetry);
        superzip::record_gpu_kernel_launch(&telemetry, 1.0);
        superzip::record_gpu_kernel_launch(&telemetry, value);
        superzip::record_gpu_kernel_launch(&telemetry, 2.0);
        const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(stats.encode_chunks, 1U);
        REQUIRE_EQ(stats.kernel_launches, 3U);
        REQUIRE_TRUE(std::isnan(stats.kernel_ms));
        REQUIRE_EQ(telemetry.kernel_microseconds.load(), std::numeric_limits<std::uint64_t>::max());
    }
}

// Purpose: Protect totals when individually valid times exceed the accumulator's representable range.
// Inputs: A synthetic near-limit counter and durations crossing its reserved unavailable value.
// Outputs: Requires no wraparound and permanent unavailable timing after overflow.
TEST_CASE(gpu_telemetry_total_overflow_is_unavailable) {
    superzip::GpuTelemetry telemetry;
    telemetry.kernel_microseconds.store(std::numeric_limits<std::uint64_t>::max() - 10U);
    superzip::record_gpu_kernel_launch(&telemetry, 0.009);
    REQUIRE_TRUE(std::isfinite(superzip::snapshot_gpu_telemetry(telemetry).kernel_ms));
    superzip::record_gpu_kernel_launch(&telemetry, 0.001);
    REQUIRE_TRUE(std::isnan(superzip::snapshot_gpu_telemetry(telemetry).kernel_ms));
    superzip::record_gpu_kernel_launch(&telemetry, 1.0);
    REQUIRE_EQ(telemetry.kernel_microseconds.load(), std::numeric_limits<std::uint64_t>::max());
}

// Purpose: Exercise atomic accumulation and invalidation with bounded concurrent writers.
// Inputs: Four joined CPU threads, each recording 4096 synthetic one-microsecond events.
// Outputs: Requires exact valid sums and no lost unavailable state when another writer invalidates the total.
TEST_CASE(gpu_telemetry_concurrent_accumulation) {
    for (const bool invalidate : {false, true}) {
        superzip::GpuTelemetry telemetry;
        {
            std::vector<std::jthread> workers;
            for (int worker = 0; worker < 4; ++worker) {
                workers.emplace_back([&telemetry, worker, invalidate] {
                    for (int event = 0; event < 4096; ++event) {
                        superzip::record_gpu_kernel_launch(&telemetry, 0.001);
                    }
                    if (invalidate && worker == 0) {
                        superzip::record_gpu_kernel_launch(&telemetry, -0.01);
                    }
                });
            }
        }
        const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
        REQUIRE_EQ(stats.kernel_launches, 16384U + (invalidate ? 1U : 0U));
        if (invalidate) {
            REQUIRE_TRUE(std::isnan(stats.kernel_ms));
        } else {
            REQUIRE_TRUE(std::abs(stats.kernel_ms - 16.384) < 1e-12);
        }
    }
}

namespace {

// Purpose: Require usable dispatch timing independently of byte correctness.
// Inputs: telemetry contains one completed production GPU operation.
// Outputs: Requires real launches, finite positive device time, and actual device transfers.
void require_device_timing(const superzip::GpuTelemetry& telemetry) {
    const auto stats = superzip::snapshot_gpu_telemetry(telemetry);
    REQUIRE_TRUE(stats.kernel_launches > 0U);
    REQUIRE_TRUE(std::isfinite(stats.kernel_ms));
    REQUIRE_TRUE(stats.kernel_ms > 0.0);
    REQUIRE_TRUE(stats.h2d_bytes > 0U);
    REQUIRE_TRUE(stats.device_allocation_bytes > 0U);
}

// Purpose: Exercise dispatch timestamps across all production encoding families and decoded CRC paths.
// Inputs: seed selects deterministic RAM bytes; uses small repeated operations on the calling thread's HIP stream.
// Outputs: Requires independent CPU CRC/output agreement and positive timing for every GPU operation.
void check_production_dispatch_timing(std::uint32_t seed) {
    constexpr std::uint32_t block_size = superzip::kMinArchiveBlockBytes;
    for (const int compression_level : {5, 9}) {
        for (const unsigned int family : {0U, 1U, 2U, 3U, 4U, 5U, 0U, 1U, 2U, 3U, 4U, 5U}) {
            std::vector<std::byte> input(block_size + 17U);
            for (std::size_t i = 0; i < input.size(); ++i) {
                seed ^= seed << 13U;
                seed ^= seed >> 17U;
                seed ^= seed << 5U;
                const auto value = family == 0   ? 41U
                                   : family == 1 ? i % 7U
                                   : family == 2 ? seed % 5U
                                   : family == 3 ? 193U + seed % 5U
                                   : family == 4 ? seed % 256U
                                                 : (i < 8192U ? i % 3U : seed % 256U);
                input[i] = static_cast<std::byte>(value);
            }
            superzip::GpuCodecOptions options;
            options.block_size = block_size;
            options.compression_level = compression_level;
            options.telemetry = std::make_shared<superzip::GpuTelemetry>();
            const auto encoded = superzip::encode_owned_chunk(input, options);
            REQUIRE_TRUE(encoded.gpu_used);
            REQUIRE_TRUE(encoded.source_crc32_available);
            REQUIRE_EQ(encoded.source_crc32, superzip::crc32(input));
            require_device_timing(*options.telemetry);

            options.telemetry = std::make_shared<superzip::GpuTelemetry>();
            const std::array<std::uint32_t, 2> lengths{block_size, 17U};
            const auto batch = superzip::encode_owned_block_batch(input, lengths, options);
            REQUIRE_TRUE(batch.encoded.gpu_used);
            REQUIRE_TRUE(batch.encoded.payload == encoded.payload);
            REQUIRE_EQ(batch.block_crc32.size(), lengths.size());
            REQUIRE_EQ(batch.block_crc32[0], superzip::crc32(std::span(input).first(block_size)));
            REQUIRE_EQ(batch.block_crc32[1], superzip::crc32(std::span(input).last(17U)));
            require_device_timing(*options.telemetry);

            options.telemetry = std::make_shared<superzip::GpuTelemetry>();
            const auto crc = superzip::crc_decoded_chunk(encoded.payload, encoded.blocks, input.size(), options);
            REQUIRE_TRUE(crc.gpu_used);
            REQUIRE_EQ(crc.crc32, superzip::crc32(input));
            require_device_timing(*options.telemetry);

            options.telemetry = std::make_shared<superzip::GpuTelemetry>();
            std::vector<std::byte> decoded(input.size());
            REQUIRE_TRUE(superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options));
            REQUIRE_TRUE(decoded == input);
            require_device_timing(*options.telemetry);
            options.require_gpu = false;
            options.force_cpu = true;
            std::fill(decoded.begin(), decoded.end(), std::byte{0});
            (void)superzip::decode_chunk(encoded.payload, encoded.blocks, decoded, options);
            REQUIRE_TRUE(decoded == input);
        }
    }
}

}  // namespace

// Purpose: Detect invalid event measurements on independent production HIP worker streams.
// Inputs: Four bounded concurrent callers with distinct fixtures; unavailable HIP skips hardware assertions.
// Outputs: Requires valid per-operation device times and byte-exact CPU/HIP decoding; worker failures propagate.
TEST_CASE(gpu_telemetry_production_dispatch_timing) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::future<void>> workers;
    for (std::uint32_t worker = 0; worker < 4U; ++worker) {
        workers.push_back(std::async(std::launch::async, check_production_dispatch_timing, 0x76543210U + worker));
    }
    for (auto& worker : workers) {
        worker.get();
    }
}

// Purpose: Cover dispatch timing on the diagnostic's compute and shared-memory checksum kernels.
// Inputs: A bounded short RAM-only diagnostic; unavailable HIP skips hardware assertions.
// Outputs: Requires positive finite device time and nonzero checksum/transfer/launch evidence.
TEST_CASE(gpu_telemetry_diagnostic_dispatch_timing) {
    if (!superzip::query_gpu_info().available) {
        return;
    }
    const auto result = superzip::run_gpu_diagnostic({.seconds = 1.0, .buffer_mib = 16, .inner_iterations = 1});
    REQUIRE_TRUE(result.kernel_launches >= 2U);
    REQUIRE_TRUE(std::isfinite(result.kernel_ms));
    REQUIRE_TRUE(result.kernel_ms > 0.0);
    REQUIRE_TRUE(result.h2d_bytes > 0U);
    REQUIRE_TRUE(result.d2h_bytes > 0U);
    REQUIRE_TRUE(result.checksum > 0U);
}
