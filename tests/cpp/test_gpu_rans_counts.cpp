#include "test_util.hpp"

#include "gpu/hip_device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

#if FLITZZIP_ENABLE_HIP
#include "gpu/hip_kernel_api.hpp"
#include "gpu/hip_rans_statistics.hpp"
#include "rans_gpu_fixtures.hpp"

namespace {
using namespace flitzzip;
using namespace flitzzip::hip_detail;
using flitzzip_test::CountDeviceBuffer;

// Each source has new statistics owners, so neither source identities nor
// partial tables survive into another trial. The reservation outlives all buffers.
class CountFixture {
  public:
    explicit CountFixture(std::span<const std::byte> bytes)
        : source(bytes.begin(), bytes.end()),
          reservation(source.size() + 2U + 3U * (kRansCountWords + 2U) * sizeof(std::uint32_t) +
                          (kRansProjectionBatch + 2U) * sizeof(RansCountStatus),
                      "rANS count fixture"),
          input(static_cast<std::uint32_t>(source.size())), base(kRansCountWords), removed(kRansCountWords),
          output(kRansCountWords), statuses(kRansProjectionBatch) {
        input.reset(source);
    }
    std::uint32_t bytes() const {
        return static_cast<std::uint32_t>(source.size());
    }
    void prepare_status(std::uint32_t count) {
        statuses.reset(std::vector<RansCountStatus>(count, RansCountStatus::invalid_plan));
    }
    void require_status(std::uint32_t count, RansCountStatus expected = RansCountStatus::success) const {
        REQUIRE_EQ(statuses.read(count), std::vector<RansCountStatus>(count, expected));
    }
    void release_checked() {
        REQUIRE_EQ(input.read(bytes()), source);
        statuses.release_checked();
        output.release_checked();
        removed.release_checked();
        base.release_checked();
        input.release_checked();
        reservation.reduce_to(0U);
    }

    const std::vector<std::byte> source;
    HipDeviceMemoryReservation reservation;
    CountDeviceBuffer<std::byte> input;
    CountDeviceBuffer<std::uint32_t> base;
    CountDeviceBuffer<std::uint32_t> removed;
    CountDeviceBuffer<std::uint32_t> output;
    CountDeviceBuffer<RansCountStatus> statuses;
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
};

// Directly count actual source adjacency, independently of the device buckets.
std::vector<std::uint32_t> direct_transitions(std::span<const std::byte> source) {
    std::vector<std::uint32_t> counts(65536U);
    unsigned int previous = 0U;
    for (const auto byte : source) {
        const auto symbol = static_cast<unsigned int>(byte);
        ++counts[previous * 256U + symbol];
        previous = symbol;
    }
    return counts;
}

// Recount each complete segmented distribution from original bytes. This oracle
// never consumes or corrects the base table used by the production projection.
std::vector<std::uint32_t> direct_segment_counts(std::span<const std::byte> source, unsigned int bits,
                                                 unsigned int shift) {
    std::vector<std::uint32_t> counts(256U << bits);
    const auto segment = std::size_t{1U} << shift;
    for (std::size_t begin = 0U; begin < source.size(); begin += segment) {
        unsigned int previous = 0U;
        for (auto i = begin; i < std::min(source.size(), begin + segment); ++i) {
            const auto symbol = static_cast<unsigned int>(source[i]);
            const auto context = previous >> (8U - bits);
            ++counts[context * 256U + symbol];
            previous = symbol;
        }
    }
    return counts;
}

void collect_transitions(CountFixture& fixture) {
    fixture.base.fill(0U);
    const auto tiles = (fixture.bytes() + kRansCountTileBytes - 1U) / kRansCountTileBytes;
    for (std::uint32_t first = 0U; first < tiles;) {
        const auto count = std::min(kRansCountTilesPerDispatch, tiles - first);
        const auto statuses = count * kRansCountBucketCount;
        fixture.prepare_status(statuses);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().count_rans_transitions, statuses, 256U,
                              "rANS count source tiles", fixture.input.data(), fixture.bytes(), first, count,
                              fixture.base.data(), kRansCountWords, fixture.statuses.data(), statuses);
        fixture.require_status(statuses);
        first += count;
    }
    const auto actual = fixture.base.read(kRansCountWords);
    REQUIRE_EQ(actual, direct_transitions(fixture.source));
    REQUIRE_EQ(std::accumulate(actual.begin(), actual.end(), std::uint64_t{0}), fixture.source.size());
}

void collect_boundaries(CountFixture& fixture, std::uint32_t shift) {
    fixture.removed.fill(0U);
    const auto last = (fixture.bytes() - 1U) >> shift;
    for (std::uint32_t first = 1U; first <= last;) {
        const auto count = std::min(kRansCorrectionBatch, last - first + 1U);
        fixture.prepare_status(count);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().count_rans_boundaries, (count + 127U) / 128U, 128U,
                              "rANS count segment boundaries", fixture.input.data(), fixture.bytes(), shift, first,
                              count, fixture.removed.data(), kRansCountWords, fixture.statuses.data(), count);
        fixture.require_status(count);
        first += count;
    }
    // Observe raw removals as well: unconditional projection cancels them and
    // could otherwise conceal an incorrect correction producer.
    std::vector<std::uint32_t> expected(kRansCountWords);
    for (std::size_t offset = std::size_t{1U} << shift; offset < fixture.source.size();
         offset += std::size_t{1U} << shift) {
        ++expected[static_cast<unsigned int>(fixture.source[offset - 1U]) * 256U +
                   static_cast<unsigned int>(fixture.source[offset])];
    }
    REQUIRE_EQ(fixture.removed.read(kRansCountWords), expected);
}

std::vector<std::uint32_t> project_counts(CountFixture& fixture, std::uint32_t bits) {
    fixture.output.reset({});
    const auto entries = 256U << bits;
    for (std::uint32_t first = 0U; first < entries;) {
        const auto count = std::min(kRansProjectionBatch, entries - first);
        fixture.prepare_status(count);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().project_rans_counts, (count + 127U) / 128U, 128U,
                              "rANS project exact context counts", fixture.base.data(), kRansCountWords,
                              fixture.removed.data(), kRansCountWords, fixture.bytes(), bits, first, count,
                              fixture.output.data(), entries, fixture.statuses.data(), count);
        fixture.require_status(count);
        first += count;
    }
    return fixture.output.read(entries);
}

std::vector<std::byte> count_source_fixture(std::uint32_t size) {
    std::vector<std::byte> bytes(size);
    std::uint32_t state = 0x12345678U;
    for (auto& byte : bytes) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::byte>(state >> 24U);
    }
    return bytes;
}

std::size_t statistics_reserved_bytes() {
    int device = -1;
    REQUIRE_EQ(hipGetDevice(&device), hipSuccess);
    return hip_device_reservation_state().reserved_bytes(device);
}

// The oracle walks the original segmented bytes, never production base tables
// or correction arithmetic. Reading the returned resident view is test-only.
void require_statistics_projection(RansStatistics& statistics, const CountFixture& fixture, std::uint32_t bits,
                                   std::uint32_t shift) {
    const auto view = statistics.project(bits, shift);
    REQUIRE_EQ(view.size(), std::size_t{256U} << bits);
    std::vector<std::uint32_t> actual(view.size());
    REQUIRE_EQ(copy_on_codec_stream(actual.data(), view.data(), view.size_bytes(), hipMemcpyDeviceToHost), hipSuccess);
    REQUIRE_EQ(actual, direct_segment_counts(fixture.source, bits, shift));
    REQUIRE_EQ(std::accumulate(actual.begin(), actual.end(), std::uint64_t{0}), fixture.source.size());
}

struct StatisticsCancellation {
    unsigned int phase;
};

template <typename Error, typename Operation>
void require_statistics_error(Operation&& operation, std::string_view diagnostic) {
    bool rejected = false;
    try {
        operation();
    } catch (const Error& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
}  // namespace
#endif

TEST_CASE(gpu_rans_counts_match_raw_adjacency_and_every_segment_context) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::uint32_t size : {1U, 255U, 256U, 257U, 4095U, 4096U, 4097U, 65536U, 65537U}) {
        const auto source = count_source_fixture(size);
        CountFixture fixture(source);
        collect_transitions(fixture);
        for (std::uint32_t shift = 8U; shift <= 12U; ++shift) {
            collect_boundaries(fixture, shift);
            for (std::uint32_t bits = 0U; bits <= 8U; ++bits) {
                const auto actual = project_counts(fixture, bits);
                REQUIRE_EQ(actual, direct_segment_counts(source, bits, shift));
                REQUIRE_EQ(std::accumulate(actual.begin(), actual.end(), std::uint64_t{0}), source.size());
            }
        }
        REQUIRE_EQ(fixture.base.read(kRansCountWords), direct_transitions(source));
        REQUIRE_EQ(snapshot_gpu_telemetry(fixture.telemetry).timing_status, GpuTimingStatus::NotRequested);
        fixture.release_checked();
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_zero_predecessors_and_disappearing_contexts_reject_omitted_work) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    std::vector<std::byte> source(513U, std::byte{0});
    source[255] = std::byte{255};
    source[256] = std::byte{7};
    source[512] = std::byte{7};  // Boundary with a zero predecessor cancels exactly.
    CountFixture fixture(source);
    collect_transitions(fixture);
    fixture.removed.fill(0U);
    const auto without_correction = project_counts(fixture, 8U);
    const auto expected = direct_segment_counts(source, 8U, 8U);
    REQUIRE_TRUE(without_correction != expected);
    REQUIRE_EQ(without_correction[255U * 256U + 7U], 1U);
    collect_boundaries(fixture, 8U);
    const auto corrected = project_counts(fixture, 8U);
    REQUIRE_EQ(corrected, expected);
    REQUIRE_TRUE(
        std::ranges::all_of(std::span(corrected).subspan(255U * 256U, 256U), [](auto count) { return count == 0U; }));
    REQUIRE_EQ(project_counts(fixture, 0U), direct_segment_counts(source, 0U, 8U));
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_projection_rejects_underflow_and_mass_overflow_then_recovers) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto source = count_source_fixture(257U);
    CountFixture fixture(source);
    collect_transitions(fixture);
    collect_boundaries(fixture, 8U);
    const auto base = fixture.base.read(kRansCountWords);
    const auto removed = fixture.removed.read(kRansCountWords);
    for (const bool underflow : {true, false}) {
        auto corrupt_base = base;
        auto corrupt_removed = removed;
        if (underflow) {
            corrupt_removed[0] = corrupt_base[0] + 1U;
        } else {
            corrupt_base[0] = fixture.bytes();
            corrupt_base[256] = 1U;
        }
        fixture.base.reset(corrupt_base);
        fixture.removed.reset(corrupt_removed);
        fixture.output.reset({});
        fixture.prepare_status(1U);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().project_rans_counts, 1U, 128U,
                              "rANS reject corrupt exact counts", fixture.base.data(), kRansCountWords,
                              fixture.removed.data(), kRansCountWords, fixture.bytes(), 0U, 0U, 1U,
                              fixture.output.data(), 256U, fixture.statuses.data(), 1U);
        fixture.require_status(1U, RansCountStatus::invalid_counts);
        REQUIRE_TRUE(fixture.output.read(0U).empty());
    }
    fixture.base.reset(base);
    fixture.removed.reset(removed);
    REQUIRE_EQ(project_counts(fixture, 8U), direct_segment_counts(source, 8U, 8U));
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_maximum_source_stays_bounded_and_preserves_all_bytes) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto source = count_source_fixture(kRansCountMaxBytes);
    CountFixture fixture(source);
    collect_transitions(fixture);
    collect_boundaries(fixture, 12U);
    REQUIRE_EQ(project_counts(fixture, 8U), direct_segment_counts(source, 8U, 12U));
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_invalid_ranges_preserve_tables_and_inactive_guards) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    const auto source = count_source_fixture(513U);
    CountFixture fixture(source);
    fixture.base.fill(0U);
    struct LaunchShape {
        dim3 grid, block;
    };
    const std::array unsupported_shapes{LaunchShape{dim3(16U, 2U, 1U), dim3(256U)},
                                        LaunchShape{dim3(16U), dim3(128U, 2U, 1U)}};
    for (const auto& shape : unsupported_shapes) {
        fixture.prepare_status(16U);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().count_rans_transitions, shape.grid, shape.block,
                              "rANS reject unsupported launch shape", fixture.input.data(), fixture.bytes(), 0U, 1U,
                              fixture.base.data(), 65536U, fixture.statuses.data(), 16U);
        fixture.require_status(16U, RansCountStatus::invalid_plan);
        REQUIRE_EQ(fixture.base.read(65536U), std::vector<std::uint32_t>(65536U, 0U));
    }
    struct TileCase {
        std::uint32_t bytes, first, count, words;
    };
    const std::array tile_cases{TileCase{0U, 0U, 1U, 65536U},   TileCase{kRansCountMaxBytes + 1U, 0U, 1U, 65536U},
                                TileCase{513U, 1U, 1U, 65536U}, TileCase{513U, 0xFFFFFFFFU, 1U, 65536U},
                                TileCase{513U, 0U, 0U, 65536U}, TileCase{513U, 0U, 17U, 65536U},
                                TileCase{513U, 0U, 1U, 65535U}};
    for (const auto& value : tile_cases) {
        fixture.prepare_status(16U);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().count_rans_transitions, 18U, 256U,
                              "rANS reject unadmitted tile range", fixture.input.data(), value.bytes, value.first,
                              value.count, fixture.base.data(), value.words, fixture.statuses.data(), 16U);
        fixture.require_status(16U, RansCountStatus::invalid_plan);
        REQUIRE_EQ(fixture.base.read(65536U), std::vector<std::uint32_t>(65536U, 0U));
    }
    // Extra blocks beyond the exact live status range must not participate in
    // barriers, revisit source tiles or write any inactive status/table entry.
    fixture.prepare_status(16U);
    dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().count_rans_transitions, 18U, 256U,
                          "rANS inactive count blocks", fixture.input.data(), fixture.bytes(), 0U, 1U,
                          fixture.base.data(), 65536U, fixture.statuses.data(), 16U);
    fixture.require_status(16U);
    REQUIRE_EQ(fixture.base.read(65536U), direct_transitions(source));
    fixture.removed.fill(0U);
    struct BoundaryCase {
        std::uint32_t bytes, shift, first, count, words;
    };
    const std::array boundary_cases{
        BoundaryCase{513U, 7U, 1U, 1U, 65536U},          BoundaryCase{513U, 13U, 1U, 1U, 65536U},
        BoundaryCase{513U, 8U, 0U, 1U, 65536U},          BoundaryCase{513U, 8U, 3U, 1U, 65536U},
        BoundaryCase{513U, 8U, 0xFFFFFFFFU, 1U, 65536U}, BoundaryCase{513U, 8U, 1U, 257U, 65536U},
        BoundaryCase{513U, 8U, 1U, 0U, 65536U},          BoundaryCase{513U, 8U, 1U, 1U, 65535U}};
    for (const auto& value : boundary_cases) {
        fixture.prepare_status(1U);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().count_rans_boundaries, 1U, 128U,
                              "rANS reject unadmitted boundary range", fixture.input.data(), value.bytes, value.shift,
                              value.first, value.count, fixture.removed.data(), value.words, fixture.statuses.data(),
                              1U);
        fixture.require_status(1U, RansCountStatus::invalid_plan);
        REQUIRE_EQ(fixture.removed.read(65536U), std::vector<std::uint32_t>(65536U, 0U));
    }
    struct ProjectionCase {
        std::uint32_t bits, first, count, words;
    };
    const std::array projection_cases{ProjectionCase{9U, 0U, 1U, 256U},          ProjectionCase{0U, 256U, 1U, 256U},
                                      ProjectionCase{0U, 0xFFFFFFFFU, 1U, 256U}, ProjectionCase{0U, 0U, 0U, 256U},
                                      ProjectionCase{0U, 0U, 4097U, 256U},       ProjectionCase{0U, 0U, 1U, 255U}};
    for (const auto& value : projection_cases) {
        fixture.output.reset({});
        fixture.prepare_status(1U);
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().project_rans_counts, 1U, 128U,
                              "rANS reject unadmitted projection range", fixture.base.data(), 65536U,
                              fixture.removed.data(), 65536U, fixture.bytes(), value.bits, value.first, value.count,
                              fixture.output.data(), value.words, fixture.statuses.data(), 1U);
        fixture.require_status(1U, RansCountStatus::invalid_plan);
        REQUIRE_TRUE(fixture.output.read(0U).empty());
    }
    collect_boundaries(fixture, 8U);
    REQUIRE_EQ(project_counts(fixture, 8U), direct_segment_counts(source, 8U, 8U));
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_operation_reuses_complete_statistics_across_all_projections) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const std::uint32_t size : {1U, 257U, 65537U, 131073U}) {
        CountFixture fixture(count_source_fixture(size));
        const auto reserved = statistics_reserved_bytes();
        GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
        RansStatistics statistics({fixture.input.data(), size}, &telemetry);
        const auto required = rans_statistics_workspace_requirements();
        REQUIRE_EQ(required.host_bytes, sizeof(RansStatistics));
        REQUIRE_EQ(statistics_reserved_bytes(), reserved + required.device_bytes);
        std::uint64_t expected = (std::uint64_t{size} + 65535U) / 65536U;
        REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches, expected);
        for (std::uint32_t shift = 8U; shift <= 12U; ++shift) {
            expected += (((std::uint64_t{size} - 1U) >> shift) + 255U) / 256U;
            for (std::uint32_t bits = 0U; bits <= 8U; ++bits) {
                require_statistics_projection(statistics, fixture, bits, shift);
                expected += ((std::uint64_t{256U} << bits) + 4095U) / 4096U;
                const auto completed = snapshot_gpu_telemetry(telemetry);
                REQUIRE_EQ(completed.kernel_launches, expected);
                unsigned int checkpoint_calls = 0U;
                (void)statistics.project(bits, shift, [&] { ++checkpoint_calls; });
                const auto reused = snapshot_gpu_telemetry(telemetry);
                REQUIRE_EQ(checkpoint_calls, 1U);
                REQUIRE_EQ(reused.kernel_launches, completed.kernel_launches);
                REQUIRE_EQ(reused.h2d_bytes, completed.h2d_bytes);
                REQUIRE_EQ(reused.d2h_bytes, completed.d2h_bytes);
                REQUIRE_EQ(reused.device_allocation_bytes, required.device_bytes);
            }
        }
        // Returning to a prior shift must rebuild its correction, rather than
        // mistaking the most recent completed projection for this request.
        require_statistics_projection(statistics, fixture, 8U, 8U);
        statistics.close_checked();
        REQUIRE_EQ(statistics_reserved_bytes(), reserved);
        statistics.close_checked();
        require_statistics_error<GpuError>([&] { (void)statistics.project(0U, 8U); }, "closed");
        fixture.release_checked();
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_operation_cancellation_invalidates_partial_caches_and_recovers) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    CountFixture fixture(count_source_fixture(131073U));
    const auto reserved = statistics_reserved_bytes();
    for (const unsigned int phase : {1U, 2U, 3U, 4U}) {
        unsigned int calls = 0U;
        bool cancelled = false;
        try {
            RansStatistics statistics({fixture.input.data(), fixture.bytes()}, nullptr, [&] {
                if (++calls == phase) {
                    throw StatisticsCancellation{calls};
                }
            });
        } catch (const StatisticsCancellation& error) {
            REQUIRE_EQ(error.phase, phase);
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(calls, phase);
        REQUIRE_EQ(statistics_reserved_bytes(), reserved);
    }
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    RansStatistics statistics({fixture.input.data(), fixture.bytes()}, &telemetry);
    const auto live = statistics_reserved_bytes();
    for (const unsigned int phase : {1U, 2U, 3U, 4U, 19U}) {
        // Each trial starts with the other width cached. At shift 8 there are
        // two correction batches and sixteen projection batches after entry.
        require_statistics_projection(statistics, fixture, 0U, 12U);
        unsigned int calls = 0U;
        bool cancelled = false;
        try {
            (void)statistics.project(8U, 8U, [&] {
                if (++calls == phase) {
                    throw StatisticsCancellation{calls};
                }
            });
        } catch (const StatisticsCancellation& error) {
            REQUIRE_EQ(error.phase, phase);
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(calls, phase);
        REQUIRE_EQ(statistics_reserved_bytes(), live);
        const auto before = snapshot_gpu_telemetry(telemetry).kernel_launches;
        require_statistics_projection(statistics, fixture, 8U, 8U);
        REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches - before, 18U);
    }
    // A throwing checkpoint on a cache hit also invalidates its admission.
    bool cancelled_hit = false;
    try {
        (void)statistics.project(8U, 8U, [] { throw StatisticsCancellation{1U}; });
    } catch (const StatisticsCancellation& error) {
        REQUIRE_EQ(error.phase, 1U);
        cancelled_hit = true;
    }
    REQUIRE_TRUE(cancelled_hit);
    const auto before = snapshot_gpu_telemetry(telemetry).kernel_launches;
    require_statistics_projection(statistics, fixture, 8U, 8U);
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).kernel_launches - before, 18U);
    statistics.close_checked();
    REQUIRE_EQ(statistics_reserved_bytes(), reserved);
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_operation_rejects_options_without_poisoning_completed_cache) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    CountFixture fixture(count_source_fixture(513U));
    GpuTelemetry telemetry{GpuObservationMode::CountersOnly};
    const auto reserved = statistics_reserved_bytes();
    require_statistics_error<ArchiveError>([&] { RansStatistics statistics({}, &telemetry); }, "source extent");
    {
        const std::vector<std::byte> oversized(kRansCountMaxBytes + 1U);
        // Valid host storage is deliberately never passed to a HIP API:
        // extent rejection must precede capability queries and allocation.
        require_statistics_error<ArchiveError>([&] { RansStatistics statistics(oversized, &telemetry); },
                                               "source extent");
    }
    REQUIRE_EQ(statistics_reserved_bytes(), reserved);
    REQUIRE_EQ(snapshot_gpu_telemetry(telemetry).device_allocation_bytes, 0U);
    RansStatistics statistics({fixture.input.data(), fixture.bytes()}, &telemetry);
    require_statistics_projection(statistics, fixture, 8U, 8U);
    const auto before = snapshot_gpu_telemetry(telemetry);
    for (const auto options :
         {std::array{9U, 8U}, std::array{0U, 7U}, std::array{0U, 13U}, std::array{0xFFFFFFFFU, 0xFFFFFFFFU}}) {
        require_statistics_error<ArchiveError>([&] { (void)statistics.project(options[0], options[1]); },
                                               "projection options");
    }
    require_statistics_projection(statistics, fixture, 8U, 8U);
    const auto after = snapshot_gpu_telemetry(telemetry);
    REQUIRE_EQ(after.kernel_launches, before.kernel_launches);
    REQUIRE_EQ(after.device_allocation_bytes, before.device_allocation_bytes);
    REQUIRE_EQ(after.h2d_bytes, before.h2d_bytes);
    REQUIRE_EQ(after.d2h_bytes, before.d2h_bytes);
    statistics.close_checked();
    REQUIRE_EQ(statistics_reserved_bytes(), reserved);
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_counts_operation_maximum_source_and_independent_lifetimes) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    CountFixture first(count_source_fixture(kRansCountMaxBytes));
    CountFixture second(std::vector<std::byte>(513U, std::byte{0xFF}));
    const auto reserved = statistics_reserved_bytes();
    const auto scratch = rans_statistics_workspace_requirements().device_bytes;
    {
        RansStatistics full({first.input.data(), first.bytes()}, nullptr);
        {
            RansStatistics small({second.input.data(), second.bytes()}, nullptr);
            REQUIRE_EQ(statistics_reserved_bytes(), reserved + 2U * scratch);
            require_statistics_projection(full, first, 0U, 8U);
            require_statistics_projection(small, second, 8U, 8U);
            require_statistics_projection(full, first, 8U, 12U);
            require_statistics_projection(small, second, 0U, 12U);
            // Deliberately exercise normal RAII destruction of completed work.
        }
        REQUIRE_EQ(statistics_reserved_bytes(), reserved + scratch);
        require_statistics_projection(full, first, 8U, 12U);
        full.close_checked();
    }
    REQUIRE_EQ(statistics_reserved_bytes(), reserved);
    second.release_checked();
    first.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
