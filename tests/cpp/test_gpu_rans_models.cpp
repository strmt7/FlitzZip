#include "test_util.hpp"
#include "gpu/hip_device.hpp"
#include "gpu/rans_model_plan.hpp"

#include <array>
#include <numeric>
#include <vector>

#if FLITZZIP_ENABLE_HIP
#include "rans_gpu_fixtures.hpp"
#include "rans_model_fixtures.hpp"

namespace {
using namespace flitzzip;
using namespace flitzzip::hip_detail;
using namespace flitzzip_test;

void require_model_cdfs(RansModelFixture& fixture, std::span<const std::uint32_t> expected, std::uint32_t precision) {
    const auto frequencies = fixture.frequencies.read(fixture.words);
    REQUIRE_EQ(frequencies, std::vector<std::uint32_t>(expected.begin(), expected.end()));
    const auto models = fixture.models.read(fixture.contexts * 258U);
    const auto reciprocal = fixture.reciprocals.read(fixture.words);
    for (std::uint32_t row = 0U; row < fixture.contexts; ++row) {
        const auto distribution = expected.subspan(row * 256U, 256U);
        const auto total = std::accumulate(distribution.begin(), distribution.end(), std::uint32_t{0});
        REQUIRE_TRUE(total == 0U || total == (1U << precision));
        const auto offset = fixture.contexts + row * 257U;
        REQUIRE_EQ(models[row], total == 0U ? 0U : offset);
        REQUIRE_EQ(models[offset], 0U);
        std::uint32_t cumulative = 0U;
        for (std::uint32_t symbol = 0U; symbol < 256U; ++symbol) {
            const auto frequency = distribution[symbol];
            cumulative += frequency;
            REQUIRE_EQ(models[offset + symbol + 1U], cumulative);
            REQUIRE_EQ(reciprocal[row * 256U + symbol],
                       frequency > 1U ? static_cast<std::uint32_t>((std::uint64_t{1U} << 32U) / frequency) : 0U);
        }
    }
}
}  // namespace
#endif

TEST_CASE(gpu_rans_models_row_mass_admission_rejects_partial_and_forged_metadata) {
    using namespace flitzzip::hip_detail;
    constexpr auto good = RansModelStatus::success;
    const std::array<RansModelRow, 4> valid{{{512U, 2U, good}, {512U, 1U, good}, {0U, 0U, good}, {0U, 0U, good}}};
    REQUIRE_TRUE(rans_model_rows_admitted(valid, 1024U, 2U));
    REQUIRE_TRUE(!rans_model_rows_admitted({}, 1024U, 2U));
    REQUIRE_TRUE(!rans_model_rows_admitted(valid, 0U, 2U));
    REQUIRE_TRUE(!rans_model_rows_admitted(valid, kRansCountMaxBytes + 1U, 2U));
    REQUIRE_TRUE(!rans_model_rows_admitted(valid, 1024U, 9U));
    REQUIRE_TRUE(!rans_model_rows_admitted(valid, 1024U, 1U));
    for (const RansModelRow replacement :
         {RansModelRow{511U, 2U, good}, RansModelRow{513U, 2U, good}, RansModelRow{512U, 0U, good},
          RansModelRow{512U, 257U, good}, RansModelRow{1U, 2U, good}, RansModelRow{1025U, 2U, good},
          RansModelRow{512U, 2U, RansModelStatus::invalid_counts}}) {
        auto rows = valid;
        rows[0] = replacement;
        REQUIRE_TRUE(!rans_model_rows_admitted(rows, 1024U, 2U));
    }
    auto no_start = valid;
    no_start[0] = {0U, 0U, good};
    no_start[1] = {1024U, 1U, good};
    REQUIRE_TRUE(!rans_model_rows_admitted(no_start, 1024U, 2U));
    std::array<RansModelRow, 256> full{};
    full.fill({kRansCountMaxBytes, 1U, good});
    REQUIRE_TRUE(!rans_model_rows_admitted(full, kRansCountMaxBytes, 8U));
    full.fill({1U, 1U, good});
    REQUIRE_TRUE(rans_model_rows_admitted(full, 256U, 8U));
    REQUIRE_TRUE(rans_model_rows_admitted(valid, 1024U, 2U));
}

TEST_CASE(gpu_rans_models_integer_quotas_match_independent_golden_vectors) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    for (const auto& golden : kRansGoldenModels) {
        std::vector<std::uint32_t> counts(256U);
        for (const auto item : golden.counts) {
            counts[item.symbol] = item.count;
        }
        const auto mass = std::accumulate(counts.begin(), counts.end(), std::uint32_t{0});
        RansModelFixture fixture(1U, mass);
        fixture.counts.reset(counts);
        fixture.prepare();
        const auto rows = fixture.read_rows();
        REQUIRE_TRUE(rans_model_rows_admitted(rows, mass, 0U));
        auto sorted = std::vector<RansGoldenCount>(golden.counts.begin(), golden.counts.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) {
            return a.count < b.count || (a.count == b.count && a.symbol < b.symbol);
        });
        const auto order = fixture.order.read(static_cast<std::uint32_t>(sorted.size()));
        for (std::size_t i = 0U; i < sorted.size(); ++i) {
            REQUIRE_EQ(order[i], sorted[i].symbol);
        }
        for (std::uint32_t precision = 8U; precision <= 16U; ++precision) {
            fixture.normalize(precision);
            fixture.require_status(RansModelStatus::success);
            std::vector<std::uint32_t> expected(256U);
            for (std::size_t i = 0U; i < golden.counts.size(); ++i) {
                expected[golden.counts[i].symbol] = golden.frequencies[(precision - 8U) * golden.counts.size() + i];
            }
            fixture.build(precision);
            fixture.require_status(RansModelStatus::success);
            require_model_cdfs(fixture, expected, precision);
        }
        REQUIRE_EQ(fixture.counts.read(256U), counts);
        fixture.release_checked();
    }
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_models_missing_contexts_and_full_width_singletons_are_explicit) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    RansModelFixture fixture(256U, 513U);
    std::vector<std::uint32_t> counts(65536U);
    counts[17U] = 1U;
    counts[65535U] = 512U;
    fixture.counts.reset(counts);
    fixture.prepare();
    REQUIRE_TRUE(rans_model_rows_admitted(fixture.read_rows(), 513U, 8U));
    for (const std::uint32_t precision : {8U, 16U}) {
        fixture.normalize(precision);
        fixture.require_status(RansModelStatus::success);
        fixture.build(precision);
        fixture.require_status(RansModelStatus::success);
        auto expected = counts;
        expected[17U] = 1U << precision;
        expected[65535U] = 1U << precision;
        require_model_cdfs(fixture, expected, precision);
    }
    REQUIRE_EQ(fixture.counts.read(65536U), counts);
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_models_corrupted_order_mass_and_frequencies_reject_before_publication) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    RansModelFixture fixture(1U, 7U);
    std::vector<std::uint32_t> counts(256U);
    counts[1U] = 1U;
    counts[17U] = 2U;
    counts[255U] = 4U;
    fixture.counts.reset(counts);
    fixture.prepare();
    const auto correct_rows = fixture.row_words.read(3U);
    const auto correct_order = fixture.order.read(3U);
    for (const auto order : {std::array<std::uint8_t, 3>{1U, 1U, 255U}, std::array<std::uint8_t, 3>{17U, 1U, 255U},
                             std::array<std::uint8_t, 3>{1U, 17U, 42U}}) {
        fixture.order.reset(order);
        fixture.normalize(16U);
        fixture.require_status(RansModelStatus::invalid_model);
        REQUIRE_TRUE(fixture.frequencies.read(0U).empty());
    }
    fixture.order.reset(correct_order);
    for (const auto rows : {std::array<std::uint32_t, 3>{6U, 3U, 1U}, std::array<std::uint32_t, 3>{7U, 4U, 1U},
                            std::array<std::uint32_t, 3>{7U, 3U, 0U}}) {
        fixture.row_words.reset(rows);
        fixture.normalize(16U);
        fixture.require_status(RansModelStatus::invalid_counts);
        REQUIRE_TRUE(fixture.frequencies.read(0U).empty());
    }
    fixture.row_words.reset(correct_rows);
    fixture.normalize(16U);
    fixture.require_status(RansModelStatus::success);
    const auto good_frequencies = fixture.frequencies.read(256U);
    for (const std::uint32_t value : {0U, 0xFFFFFFFFU, good_frequencies[1U] + 1U}) {
        auto bad = good_frequencies;
        bad[1U] = value;
        fixture.frequencies.reset(bad);
        fixture.build(16U);
        fixture.require_status(RansModelStatus::invalid_model);
        REQUIRE_TRUE(fixture.models.read(0U).empty());
        REQUIRE_TRUE(fixture.reciprocals.read(0U).empty());
    }
    auto overflow = counts;
    overflow[1U] = 0xFFFFFFFFU;
    overflow[17U] = 2U;
    overflow[255U] = 0U;
    fixture.counts.reset(overflow);
    fixture.prepare();
    REQUIRE_EQ(fixture.read_rows()[0].status, RansModelStatus::invalid_counts);
    fixture.counts.reset(counts);
    fixture.prepare();
    REQUIRE_TRUE(rans_model_rows_admitted(fixture.read_rows(), 7U, 0U));
    fixture.normalize(16U);
    fixture.require_status(RansModelStatus::success);
    fixture.build(16U);
    fixture.require_status(RansModelStatus::success);
    require_model_cdfs(fixture, good_frequencies, 16U);
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}

TEST_CASE(gpu_rans_models_invalid_launch_and_precision_leave_guarded_storage_unchanged) {
#if FLITZZIP_ENABLE_HIP
    if (!flitzzip::query_gpu_info().available) {
        return;
    }
    RansModelFixture fixture(1U, 1U);
    std::vector<std::uint32_t> counts(256U);
    counts[255U] = 1U;
    fixture.counts.reset(counts);
    for (const auto shape : {std::array{dim3(1U), dim3(128U, 2U)}, std::array{dim3(1U, 2U), dim3(256U)}}) {
        fixture.row_words.reset({});
        dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().prepare_rans_models, shape[0], shape[1],
                              "reject rANS model launch shape", fixture.counts.data(), 256U, 1U, fixture.order.data(),
                              256U, fixture.row_data(), 1U);
        REQUIRE_TRUE(fixture.row_words.read(0U).empty());
        REQUIRE_TRUE(fixture.order.read(0U).empty());
    }
    // Excess inactive blocks must not touch the outer status or data guards.
    dispatch_codec_kernel(&fixture.telemetry, hip_kernel_api().prepare_rans_models, 2U, 256U,
                          "prepare rANS model with inactive block", fixture.counts.data(), 256U, 1U,
                          fixture.order.data(), 256U, fixture.row_data(), 1U);
    REQUIRE_TRUE(rans_model_rows_admitted(fixture.read_rows(), 1U, 0U));
    REQUIRE_EQ(fixture.order.read(1U), std::vector<std::uint8_t>{255U});
    for (const std::uint32_t precision : {0U, 7U, 17U, 0xFFFFFFFFU}) {
        fixture.normalize(precision);
        fixture.require_status(RansModelStatus::invalid_plan);
        REQUIRE_TRUE(fixture.frequencies.read(0U).empty());
        fixture.build(precision);
        fixture.require_status(RansModelStatus::invalid_plan);
        REQUIRE_TRUE(fixture.models.read(0U).empty());
        REQUIRE_TRUE(fixture.reciprocals.read(0U).empty());
    }
    fixture.normalize(16U);
    fixture.require_status(RansModelStatus::success);
    fixture.build(16U);
    fixture.require_status(RansModelStatus::success);
    std::vector<std::uint32_t> expected(256U);
    expected[255U] = 65536U;
    require_model_cdfs(fixture, expected, 16U);
    fixture.release_checked();
#else
    REQUIRE_TRUE(!flitzzip::query_gpu_info().available);
#endif
}
