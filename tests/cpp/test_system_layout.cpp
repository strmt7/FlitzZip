#include "app/system_layout.hpp"
#include "app/graph_series.hpp"
#include "app/performance_options.hpp"
#include "app/performance_counters.hpp"
#include "test_util.hpp"

#include <memory>
#include <limits>

namespace {
// Purpose: Check numeric results with a relative rounding bound while preserving exact observed zero.
// Inputs: Finite actual and independently calculated expected values.
// Outputs: Fails for nonfinite values, nonzero idle readings or an error beyond eight rounding units.
void require_numeric_sample(double actual, double expected) {
    REQUIRE_TRUE(std::isfinite(actual) && std::isfinite(expected));
    if (std::fpclassify(expected) == FP_ZERO) {
        REQUIRE_EQ(std::fpclassify(actual), FP_ZERO);
    } else {
        const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() * std::abs(expected);
        REQUIRE_TRUE(std::abs(actual - expected) <= tolerance);
    }
}

// Purpose: Assert usable contained geometry; inputs: child/parent rectangles; outputs: fails invalid dimensions.
void require_contained(const RECT& child, const RECT& parent) {
    REQUIRE_TRUE(child.right > child.left);
    REQUIRE_TRUE(child.bottom > child.top);
    REQUIRE_TRUE(child.left >= parent.left && child.right <= parent.right);
    REQUIRE_TRUE(child.top >= parent.top && child.bottom <= parent.bottom);
}
}  // namespace

// Purpose: Distinguish an observed idle GPU from missing or invalid counters before graph rendering.
// Inputs: Valid endpoints, ordinary utilization, unavailable values and nonfinite/out-of-range counters.
// Outputs: Keeps actual zero at zero and represents every unavailable or invalid value as a gap.
TEST_CASE(system_graph_percentages_preserve_missing_measurements) {
    using flitzzip::app::graph_percent_sample;
    require_numeric_sample(graph_percent_sample(0.0, true), 0.0);
    require_numeric_sample(graph_percent_sample(50.0, true), 0.5);
    require_numeric_sample(graph_percent_sample(100.0, true), 1.0);
    require_numeric_sample(graph_percent_sample(std::optional<double>{0.0}), 0.0);
    require_numeric_sample(graph_percent_sample(std::optional<double>{50.0}), 0.5);
    for (int tenth_percent = 0; tenth_percent <= 1000; ++tenth_percent) {
        const double percent = static_cast<double>(tenth_percent) / 10.0;
        const double expected = static_cast<double>(tenth_percent) / 1000.0;
        require_numeric_sample(graph_percent_sample(percent, true), expected);
        require_numeric_sample(graph_percent_sample(std::optional<double>{percent}), expected);
    }
    REQUIRE_TRUE(std::isnan(graph_percent_sample(std::nullopt)));
    for (const double value : {0.0, 50.0, 100.0}) {
        REQUIRE_TRUE(std::isnan(graph_percent_sample(value, false)));
    }
    for (const double invalid : {-0.001, 100.001, std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        REQUIRE_TRUE(std::isnan(graph_percent_sample(invalid, true)));
        REQUIRE_TRUE(std::isnan(graph_percent_sample(invalid, false)));
    }
}

// Purpose: Preserve newly observed and genuinely idle counters while rejecting unavailable or malformed samples.
// Inputs: Independent call/sample errors, valid/new statuses, zero, ordinary rates and invalid numeric values.
// Outputs: Only documented successful nonnegative finite samples are admitted; independent failures stay absent.
TEST_CASE(system_performance_counters_preserve_independent_availability) {
    using flitzzip::app::admitted_pdh_counter;
    for (const DWORD status : {PDH_CSTATUS_VALID_DATA, PDH_CSTATUS_NEW_DATA}) {
        for (const double value : {0.0, 42.0, 1024.0}) {
            const auto sample = admitted_pdh_counter(ERROR_SUCCESS, status, value);
            REQUIRE_TRUE(sample.has_value());
            require_numeric_sample(*sample, value);
        }
        for (const double invalid :
             {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            REQUIRE_TRUE(!admitted_pdh_counter(ERROR_SUCCESS, status, invalid));
        }
        REQUIRE_TRUE(!admitted_pdh_counter(PDH_INVALID_HANDLE, status, 42.0));
    }
    REQUIRE_TRUE(!admitted_pdh_counter(ERROR_SUCCESS, PDH_CSTATUS_NO_INSTANCE, 0.0));
    REQUIRE_TRUE(!admitted_pdh_counter(ERROR_SUCCESS, PDH_CSTATUS_INVALID_DATA, 42.0));
    const std::array independent{admitted_pdh_counter(ERROR_SUCCESS, PDH_CSTATUS_NO_INSTANCE, 0.0),
                                 admitted_pdh_counter(ERROR_SUCCESS, PDH_CSTATUS_NEW_DATA, 0.0),
                                 admitted_pdh_counter(ERROR_SUCCESS, PDH_CSTATUS_VALID_DATA, 2048.0)};
    REQUIRE_TRUE(!independent[0]);
    REQUIRE_EQ(independent[1], std::optional<double>{0.0});
    REQUIRE_EQ(independent[2], std::optional<double>{2048.0});
}

// Purpose: Preserve drive attribution across selection changes and inventory refreshes without losing observed zero.
// Inputs: Present/absent measurements associated with two independent logical-drive identities and empty selections.
// Outputs: Only exact selected-drive measurements survive; another drive's history never becomes current data.
TEST_CASE(system_performance_counters_bind_values_to_drive_identity) {
    using flitzzip::app::matching_drive_counter;
    REQUIRE_EQ(matching_drive_counter(0.0, L"C:", L"C:"), std::optional<double>{0.0});
    REQUIRE_EQ(matching_drive_counter(2048.0, L"D:", L"D:"), std::optional<double>{2048.0});
    REQUIRE_TRUE(!matching_drive_counter(2048.0, L"C:", L"D:"));
    REQUIRE_TRUE(!matching_drive_counter(0.0, L"D:", L"C:"));
    REQUIRE_TRUE(!matching_drive_counter(2048.0, L"C:", L""));
    REQUIRE_TRUE(!matching_drive_counter(2048.0, L"", L""));
    REQUIRE_TRUE(!matching_drive_counter(std::nullopt, L"C:", L"C:"));
}

// Purpose: Prevent graph lines and filled areas from crossing missing intervals or shifting later observations.
// Inputs: Empty, entirely missing, finite and mixed histories; missing samples occur at both ends and internally.
// Outputs: Requires exact contiguous original-index runs, including isolated observed zeroes.
TEST_CASE(system_graph_segments_preserve_gaps_and_original_indices) {
    const double missing = std::numeric_limits<double>::quiet_NaN();
    const double infinite = std::numeric_limits<double>::infinity();
    using Runs = std::vector<std::pair<std::size_t, std::size_t>>;
    // Purpose: Collect production segment selection. Inputs: A history. Outputs: Its original first/count runs.
    const auto segments = [](std::span<const double> values) {
        Runs runs;
        // Purpose: Retain selected geometry. Inputs: Original first/count. Outputs: Appends a test observation.
        flitzzip::app::for_each_graph_segment(
            values, [&](std::size_t first, std::size_t count) { runs.emplace_back(first, count); });
        return runs;
    };
    REQUIRE_TRUE(segments({}).empty());
    REQUIRE_TRUE(segments(std::array{missing, infinite, -infinite}).empty());
    REQUIRE_EQ(segments(std::array{0.0, 0.5, 1.0}), (Runs{{0U, 3U}}));
    REQUIRE_EQ(segments(std::array{missing, 0.0, missing, 0.5, 1.0, infinite, 0.0, missing}),
               (Runs{{1U, 1U}, {3U, 2U}, {6U, 1U}}));
}

// Purpose: Preserve graph history capacity, right alignment and sample spacing when missing samples split a line.
// Inputs: A 96-pixel plot, short/full/long histories and invalid indices or collapsed geometry.
// Outputs: Requires independently calculated integer pixel coordinates and explicit rejection of invalid calls.
TEST_CASE(system_graph_sample_positions_preserve_the_existing_timeline) {
    using flitzzip::app::graph_sample_x;
    const RECT plot{100, 0, 196, 101};
    REQUIRE_EQ(graph_sample_x(plot, 1U, 0U), 195);
    for (std::size_t index = 0U; index < 3U; ++index) {
        REQUIRE_EQ(graph_sample_x(plot, 3U, index), 193 + static_cast<int>(index));
    }
    for (std::size_t index = 0U; index < 96U; ++index) {
        REQUIRE_EQ(graph_sample_x(plot, 96U, index), 100 + static_cast<int>(index));
    }
    for (std::size_t index = 0U; index < 128U; ++index) {
        const auto expected = 100U + (190U * index + 127U) / 254U;
        REQUIRE_EQ(graph_sample_x(plot, 128U, index), static_cast<int>(expected));
    }
    for (const auto [count, index] :
         std::array<std::pair<std::size_t, std::size_t>, 3>{{{0U, 0U}, {1U, 1U}, {3U, 4U}}}) {
        bool rejected = false;
        try {
            (void)graph_sample_x(plot, count, index);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    bool collapsed_rejected = false;
    try {
        (void)graph_sample_x(RECT{100, 0, 100, 101}, 1U, 0U);
    } catch (const std::invalid_argument&) {
        collapsed_rejected = true;
    }
    REQUIRE_TRUE(collapsed_rejected);
}

// Purpose: Prevent a refreshed drive count from indexing an earlier inventory after a host drive change.
// Inputs: Independently captured empty, one-drive and two-drive inventories, with forward/backward selections.
// Outputs: Each selection remains in its own snapshot and empty inventories produce no index.
TEST_CASE(system_drive_selection_binds_indices_to_the_captured_inventory) {
    const std::vector<std::wstring> absent;
    const std::vector<std::wstring> before{L"fixture-A"};
    const std::vector<std::wstring> after{L"fixture-A", L"fixture-B"};
    for (const int index : {-1, 1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        REQUIRE_TRUE(!flitzzip::app::fixed_drive_index(absent, index));
        const auto earlier = flitzzip::app::fixed_drive_index(before, index);
        const auto later = flitzzip::app::fixed_drive_index(after, index);
        REQUIRE_TRUE(earlier && later);
        REQUIRE_EQ(before.at(*earlier), L"fixture-A");
        REQUIRE_EQ(after.at(*later), index == std::numeric_limits<int>::min() ? L"fixture-A" : L"fixture-B");
        REQUIRE_EQ(flitzzip::app::fixed_drive_index(before, index), earlier);
    }
}

// Purpose: Exercise circular drive selection at every possible Windows drive count and signed boundaries.
// Inputs: Inventories of 1-26 drive labels, small signed indices and both signed-int extremes.
// Outputs: Matches independently evaluated signed modular arithmetic and never indexes outside the snapshot.
TEST_CASE(system_drive_selection_handles_all_drive_counts_and_signed_extremes) {
    for (std::size_t count = 1; count <= 26; ++count) {
        const std::vector<std::wstring> drives(count, L"fixture");
        std::vector<int> indices{std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 999};
        for (int index = -52; index <= 52; ++index) {
            indices.push_back(index);
        }
        for (const int index : indices) {
            const auto width = static_cast<std::int64_t>(count);
            const auto expected = (static_cast<std::int64_t>(index) % width + width) % width;
            const auto actual = flitzzip::app::fixed_drive_index(drives, index);
            REQUIRE_TRUE(actual && *actual < count);
            REQUIRE_EQ(*actual, static_cast<std::size_t>(expected));
            REQUIRE_EQ(flitzzip::app::fixed_drive_index(drives, static_cast<int>(*actual)), actual);
        }
    }
}

// Purpose: Verify refresh-interval choices against independently specified transition thresholds.
// Inputs: Every integer around the supported options, including both exact midpoint ties.
// Outputs: Requires stable nearest values, valid indices, and idempotent normalization.
TEST_CASE(system_performance_options_choose_nearest_interval_and_shorter_ties) {
    for (int seconds = -32; seconds <= 32; ++seconds) {
        const int expected_index = seconds <= 2 ? 0 : (seconds <= 4 ? 1 : (seconds <= 7 ? 2 : 3));
        constexpr std::array<int, 4> expected_seconds{1, 3, 5, 10};
        const int actual = flitzzip::app::normalize_performance_update_seconds(seconds);
        REQUIRE_EQ(actual, expected_seconds[static_cast<std::size_t>(expected_index)]);
        REQUIRE_EQ(flitzzip::app::performance_update_index_for_seconds(seconds), expected_index);
        REQUIRE_EQ(flitzzip::app::normalize_performance_update_seconds(actual), actual);
    }
}

// Purpose: Prevent subtraction and absolute-value overflow in interval normalization.
// Inputs: Adjacent values at both signed-int limits. Outputs: Always selects the nearest endpoint and its index.
TEST_CASE(system_performance_options_handle_full_signed_integer_range) {
    for (int offset = 0; offset < 32; ++offset) {
        const int low = std::numeric_limits<int>::min() + offset;
        const int high = std::numeric_limits<int>::max() - offset;
        REQUIRE_EQ(flitzzip::app::normalize_performance_update_seconds(low), 1);
        REQUIRE_EQ(flitzzip::app::performance_update_index_for_seconds(low), 0);
        REQUIRE_EQ(flitzzip::app::normalize_performance_update_seconds(high), 10);
        REQUIRE_EQ(flitzzip::app::performance_update_index_for_seconds(high), 3);
    }
}

TEST_CASE(system_layout_keeps_runtime_controls_and_monitor_inside_tested_viewports) {
    for (const UINT dpi : {96U, 120U, 144U, 192U, 240U}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        for (const int width : {960, 1200, 1920}) {
            for (const int height : {600, 760, 1080}) {
                const RECT content{scale(86), scale(52), scale(width), scale(height - 34)};
                const auto layout = flitzzip::app::make_system_layout(content, dpi);
                require_contained(layout.area, content);
                for (const auto& rect : {layout.runtime, layout.policy, layout.architecture, layout.monitor}) {
                    require_contained(rect, layout.area);
                }
                REQUIRE_TRUE(layout.runtime.bottom < layout.policy.top);
                REQUIRE_TRUE(layout.policy.right < layout.architecture.left);
                REQUIRE_TRUE(layout.policy.bottom < layout.monitor.top);
                // The graph's header, labels, details, and positive plot need at least 220 DIPs.
                REQUIRE_TRUE(layout.monitor.bottom - layout.monitor.top >= scale(220));
            }
        }
    }
}

TEST_CASE(system_layout_default_geometry_matches_mouse_smoke_coordinates) {
    const auto layout = flitzzip::app::make_system_layout(RECT{86, 52, 1200, 726}, 96);
    REQUIRE_EQ(layout.monitor.left, 116);
    REQUIRE_EQ(layout.monitor.top, 220);
    REQUIRE_EQ(layout.monitor.right, 1170);
    REQUIRE_EQ(layout.monitor.bottom, 704);
}

TEST_CASE(system_metric_details_fit_native_font_at_compact_and_default_dpi) {
    const std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)> dc(CreateCompatibleDC(nullptr), DeleteDC);
    REQUIRE_TRUE(dc != nullptr);
    for (const UINT dpi : {96U, 110U, 120U, 144U, 168U, 192U, 240U, 288U}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        const std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)> font(
            CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                        L"Segoe UI"),
            DeleteObject);
        REQUIRE_TRUE(font != nullptr);
        const auto original = SelectObject(dc.get(), font.get());
        for (const int width : {182, 242, 400}) {
            const RECT card{0, 0, scale(width), scale(248)};
            for (const auto text :
                 {L"CPU used (total): 100.0%\nCPU used (dedicated): 100.0%",
                  L"VRAM used (total): 999.9 GiB / 1023.9 GiB\nVRAM used (dedicated): 999.9 GiB",
                  L"RAM used (total): 999.9 TiB / 1023.9 TiB\nRAM used (dedicated): 999.9 GiB",
                  L"HIP VRAM unavailable\nGPU memory counter unavailable", L"Read: 999.9 GiB/s\nWrite: 999.9 GiB/s"}) {
                const auto body = flitzzip::app::make_performance_card_body(dc.get(), card, dpi, text);
                require_contained(body.plot, card);
                require_contained(body.detail, card);
                REQUIRE_TRUE(body.plot.bottom + scale(8) <= body.detail.top);
                REQUIRE_TRUE(body.plot.bottom - body.plot.top >= scale(48));
                RECT measured{body.detail.left, 0, body.detail.right, 0};
                DrawTextW(dc.get(), text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                REQUIRE_TRUE(measured.bottom <= body.detail.bottom - body.detail.top);
            }
        }
        const auto unchanged = flitzzip::app::make_performance_card_body(dc.get(), RECT{0, 0, scale(242), scale(408)},
                                                                         dpi, L"Read: 0 B/s\nWrite: 0 B/s");
        REQUIRE_EQ(unchanged.detail.top, scale(408) - scale(8) - scale(42));
        SelectObject(dc.get(), original);
    }
}
