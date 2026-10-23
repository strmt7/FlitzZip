#include "app/form_layout.hpp"
#include "app/window_layout.hpp"
#include "app/table_columns.hpp"
#include "test_util.hpp"

#include <memory>
#include <numeric>
#include <limits>

namespace {
constexpr DWORD kStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

// Purpose: Query real frame metrics; inputs: positive DPI; outputs: nonclient width/height or a failed test.
SIZE frame_size(UINT dpi) {
    RECT frame{};
    REQUIRE_TRUE(AdjustWindowRectExForDpi(&frame, kStyle, FALSE, 0, dpi));
    return {frame.right - frame.left, frame.bottom - frame.top};
}
}  // namespace

TEST_CASE(window_layout_fits_work_areas_without_crossing_native_control_size_limits) {
    for (const UINT dpi : {96U, 110U, 120U, 144U, 168U, 192U, 240U, 288U}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        const auto frame = frame_size(dpi);
        for (const SIZE size : {SIZE{1280, 680}, SIZE{1366, 728}, SIZE{1920, 1040}, SIZE{2560, 1400}, SIZE{3840, 2100},
                                SIZE{7680, 4260}}) {
            for (const POINT origin : {POINT{0, 0}, POINT{-2560, -1440}, POINT{3840, 100}}) {
                const RECT work{origin.x, origin.y, origin.x + size.cx, origin.y + size.cy};
                const RECT proposed{work.right - 10, work.bottom - 10, work.right + 1200, work.bottom + 760};
                const auto result = flitzzip::app::make_window_layout(work, proposed, frame, dpi);
                REQUIRE_TRUE(result.has_value());
                const auto& bounds = result->bounds;
                const LONG width = bounds.right - bounds.left - frame.cx;
                const LONG height = bounds.bottom - bounds.top - frame.cy;
                REQUIRE_TRUE(width >= scale(960) && width <= scale(1200));
                REQUIRE_TRUE(height >= scale(600) && height <= scale(760));
                REQUIRE_EQ(result->minimum_fits, size.cx >= scale(960) + frame.cx && size.cy >= scale(600) + frame.cy);
                REQUIRE_TRUE(bounds.left >= work.left && bounds.top >= work.top);
                if (result->minimum_fits) {
                    REQUIRE_TRUE(bounds.right <= work.right && bounds.bottom <= work.bottom);
                }
                const RECT content{scale(86), scale(52), width, height - scale(34)};
                const auto compress = flitzzip::app::make_compress_layout(content, dpi);
                const auto settings = flitzzip::app::make_settings_layout(content, dpi);
                REQUIRE_TRUE(compress.security.bottom < compress.start.top);
                REQUIRE_TRUE(settings.logging.bottom < settings.apply.top);
                REQUIRE_TRUE(settings.log_level.right < settings.open_log_file.left);
            }
        }
    }
}

TEST_CASE(window_layout_preserves_normal_size_and_restores_it_after_monitor_changes) {
    const RECT large{0, 0, 3840, 2100};
    const RECT position{120, 100, 500, 500};
    const auto frame = frame_size(144);
    const auto normal = flitzzip::app::make_window_layout(large, position, frame, 144);
    REQUIRE_TRUE(normal && normal->minimum_fits);
    REQUIRE_EQ(normal->bounds.left, position.left);
    REQUIRE_EQ(normal->bounds.top, position.top);
    REQUIRE_EQ(normal->bounds.right - normal->bounds.left, 1800 + frame.cx);
    REQUIRE_EQ(normal->bounds.bottom - normal->bounds.top, 1140 + frame.cy);
    const RECT small{-1920, 0, 0, 1040};
    const auto compact = flitzzip::app::make_window_layout(small, normal->bounds, frame, 144);
    REQUIRE_TRUE(compact && compact->minimum_fits);
    REQUIRE_EQ(compact->bounds.bottom, small.bottom);
    REQUIRE_TRUE(compact->bounds.top >= small.top);
    const auto restored = flitzzip::app::make_window_layout(large, compact->bounds, frame_size(192), 192);
    REQUIRE_TRUE(restored && restored->minimum_fits);
    REQUIRE_EQ(restored->bounds.right - restored->bounds.left, 2400 + frame_size(192).cx);
    REQUIRE_EQ(restored->bounds.bottom - restored->bounds.top, 1520 + frame_size(192).cy);
}

TEST_CASE(window_layout_rejects_invalid_geometry_and_reports_insufficient_work_area) {
    const RECT work{0, 0, 1920, 1040};
    REQUIRE_TRUE(!flitzzip::app::make_window_layout(work, work, SIZE{}, 0));
    REQUIRE_TRUE(!flitzzip::app::make_window_layout(work, work, SIZE{}, 1000));
    REQUIRE_TRUE(!flitzzip::app::make_window_layout(RECT{}, work, SIZE{}, 96));
    REQUIRE_TRUE(!flitzzip::app::make_window_layout(work, work, SIZE{-1, 0}, 96));
    const auto tiny = flitzzip::app::make_window_layout(RECT{10, 20, 210, 220}, work, frame_size(144), 144);
    REQUIRE_TRUE(tiny && !tiny->minimum_fits);
    REQUIRE_EQ(tiny->bounds.left, 10);
    REQUIRE_EQ(tiny->bounds.top, 20);
    REQUIRE_EQ(tiny->bounds.right - tiny->bounds.left - frame_size(144).cx, 1440);
    REQUIRE_EQ(tiny->bounds.bottom - tiny->bounds.top - frame_size(144).cy, 900);
}

TEST_CASE(window_layout_real_hidden_window_has_the_requested_client_and_frame_dimensions) {
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window(
        CreateWindowExW(0, L"STATIC", L"FlitzZip layout test", kStyle, 0, 0, 1200, 760, nullptr, nullptr,
                        GetModuleHandleW(nullptr), nullptr),
        DestroyWindow);
    REQUIRE_TRUE(window != nullptr);
    const UINT dpi = GetDpiForWindow(window.get());
    const auto frame = frame_size(dpi);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    REQUIRE_TRUE(GetMonitorInfoW(MonitorFromWindow(window.get(), MONITOR_DEFAULTTONEAREST), &monitor));
    for (const SIZE size : {SIZE{1100, 700}, SIZE{1200, 760}}) {
        const LONG width = MulDiv(size.cx, static_cast<int>(dpi), 96) + frame.cx;
        const LONG height = MulDiv(size.cy, static_cast<int>(dpi), 96) + frame.cy;
        // Real windows obey the host's tracking limits; synthetic work areas belong to the pure tests above.
        const RECT work{monitor.rcWork.left, monitor.rcWork.top,
                        std::min(monitor.rcWork.right, monitor.rcWork.left + width),
                        std::min(monitor.rcWork.bottom, monitor.rcWork.top + height)};
        const auto result = flitzzip::app::make_window_layout(work, work, frame, dpi);
        REQUIRE_TRUE(result && result->minimum_fits);
        const auto& bounds = result->bounds;
        REQUIRE_TRUE(SetWindowPos(window.get(), nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE));
        RECT actual{};
        REQUIRE_TRUE(GetWindowRect(window.get(), &actual));
        REQUIRE_EQ(actual.left, bounds.left);
        REQUIRE_EQ(actual.top, bounds.top);
        REQUIRE_EQ(actual.right, bounds.right);
        REQUIRE_EQ(actual.bottom, bounds.bottom);
        REQUIRE_TRUE(GetClientRect(window.get(), &actual));
        REQUIRE_EQ(actual.right, bounds.right - bounds.left - frame.cx);
        REQUIRE_EQ(actual.bottom, bounds.bottom - bounds.top - frame.cy);
        REQUIRE_TRUE(!IsWindowVisible(window.get()));
    }
}

namespace {
// Purpose: Check pixel tracking independently of stored design weights across both table consumers.
// Inputs: Initial requested widths and minimums; all generated viewports fit the documented readable columns.
// Outputs: Requires exact pointer displacement up to a minimum, constant pair/total sizes and unchanged neighbors.
template <std::size_t N>
void require_table_resize_geometry(const std::array<double, N>& design, const std::array<int, N>& minimums) {
    for (const int dpi : {96, 110, 120, 144, 168, 192, 240, 288, 480}) {
        for (const int capacity : {620, 700, 940, 982, 1048, 1600}) {
            const int available = MulDiv(capacity, dpi, 96);
            const auto baseline = flitzzip::app::fit_table_columns(design, minimums, available, dpi);
            REQUIRE_EQ(std::accumulate(baseline.begin(), baseline.end(), 0), available);
            for (std::size_t boundary = 0; boundary + 1 < N; ++boundary) {
                for (const std::int64_t delta :
                     {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1000000}, std::int64_t{-60},
                      std::int64_t{-40}, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, std::int64_t{40},
                      std::int64_t{60}, std::int64_t{1000000}, std::numeric_limits<std::int64_t>::max()}) {
                    const auto changed = flitzzip::app::resize_table_columns(baseline, minimums, boundary, delta, dpi);
                    const auto actual = flitzzip::app::fit_table_columns(changed, minimums, available, dpi);
                    const auto left_minimum = MulDiv(minimums[boundary], dpi, 96);
                    const auto right_minimum = MulDiv(minimums[boundary + 1], dpi, 96);
                    const auto movement = std::clamp<std::int64_t>(delta, left_minimum - baseline[boundary],
                                                                   baseline[boundary + 1] - right_minimum);
                    REQUIRE_EQ(actual[boundary], baseline[boundary] + movement);
                    REQUIRE_EQ(actual[boundary + 1], baseline[boundary + 1] - movement);
                    REQUIRE_EQ(std::accumulate(actual.begin(), actual.end(), 0), available);
                    for (std::size_t i = 0; i < N; ++i) {
                        REQUIRE_TRUE(actual[i] >= MulDiv(minimums[i], dpi, 96));
                        if (i != boundary && i != boundary + 1) {
                            REQUIRE_EQ(actual[i], baseline[i]);
                        }
                    }
                    const auto returned = flitzzip::app::resize_table_columns(baseline, minimums, boundary, 0, dpi);
                    REQUIRE_TRUE(flitzzip::app::fit_table_columns(returned, minimums, available, dpi) == baseline);
                }
            }
        }
    }
}

// Purpose: Distinguish numeric/geometry admission failures from unrelated exceptions.
// Inputs: A synchronous table-model operation with independently invalid arguments.
// Outputs: Requires invalid_argument rather than accepting overflow, unrelated exceptions or silent success.
template <class Action> void require_invalid_table_argument(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}
}  // namespace

// Purpose: Regress proportional re-scaling of pointer movement in normal and compact Queue viewports.
// Inputs: Default column weights and extreme valid resized weights through the production fitting/drag model.
// Outputs: Requires all boundary movements, readable minimums, stable neighbors and exact reverse positioning.
TEST_CASE(queue_columns_track_physical_pointer_across_viewports_and_dpi) {
    require_table_resize_geometry(std::array<double, 4>{276, 100, 96, 468}, flitzzip::app::kQueueColumnMinimums);
    require_table_resize_geometry(std::array<double, 4>{90, 70, 660, 120}, flitzzip::app::kQueueColumnMinimums);
}

// Purpose: Prevent the sibling History table from retaining the same design-versus-pixel drag defect.
// Inputs: Default and minimum-constrained History shapes at independent viewport/DPI/displacement combinations.
// Outputs: Requires exact physical geometry and no changes outside the dragged adjacent pair.
TEST_CASE(history_columns_track_physical_pointer_across_viewports_and_dpi) {
    require_table_resize_geometry(std::array<double, 5>{112, 100, 300, 410, 102},
                                  flitzzip::app::kHistoryColumnMinimums);
    require_table_resize_geometry(std::array<double, 5>{82, 78, 140, 650, 74}, flitzzip::app::kHistoryColumnMinimums);
}

// Purpose: Constrain proportional fitting without borrowing space from already minimal columns.
// Inputs: Independently specified small four-column examples, including a viewport below the readable minimum sum.
// Outputs: Requires exact balanced proportions, saturation to all minimums and valid aggregate bounds.
TEST_CASE(table_columns_preserve_minimums_and_complete_available_extent) {
    constexpr std::array<double, 4> proportions{100, 200, 300, 400};
    constexpr std::array<int, 4> minimums{10, 10, 10, 10};
    REQUIRE_TRUE(
        (flitzzip::app::fit_table_columns(proportions, minimums, 500, 96) == std::array<int, 4>{50, 100, 150, 200}));
    REQUIRE_TRUE((flitzzip::app::fit_table_columns(proportions, minimums, 40, 96) == minimums));
    REQUIRE_TRUE((flitzzip::app::fit_table_columns(proportions, minimums, 1, 96) == minimums));
    REQUIRE_TRUE((flitzzip::app::fit_table_columns(std::array<double, 4>{1, 1, 1, 1000}, minimums, 41, 96) ==
                  std::array<int, 4>{10, 10, 10, 11}));
}

// Purpose: Reject invalid numeric extents and drag geometry before undefined floating or integer conversions.
// Inputs: Nonfinite/negative widths, invalid DPI/capacity/minimums, missing pair and undersized physical baseline.
// Outputs: Requires the precise argument-error class; displacement overflow is exercised by the valid geometry tests.
TEST_CASE(table_columns_reject_invalid_numeric_and_resize_geometry) {
    for (const double width : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::max()}) {
        require_invalid_table_argument([&] { (void)flitzzip::app::table_column_pixels(width, 96); });
    }
    for (const int dpi : {0, -1}) {
        require_invalid_table_argument([&] { (void)flitzzip::app::table_column_pixels(10, dpi); });
    }
    constexpr std::array<double, 2> widths{100, 100};
    constexpr std::array<int, 2> minimums{10, 10};
    require_invalid_table_argument([&] { (void)flitzzip::app::fit_table_columns(widths, minimums, -1, 96); });
    require_invalid_table_argument(
        [&] { (void)flitzzip::app::fit_table_columns(widths, std::array<int, 2>{0, 10}, 200, 96); });
    require_invalid_table_argument([&] {
        (void)flitzzip::app::fit_table_columns(
            widths, std::array<int, 2>{std::numeric_limits<int>::max(), std::numeric_limits<int>::max()}, 200, 96);
    });
    require_invalid_table_argument(
        [&] { (void)flitzzip::app::resize_table_columns(std::array<int, 2>{100, 100}, minimums, 1, 0, 96); });
    require_invalid_table_argument(
        [&] { (void)flitzzip::app::resize_table_columns(std::array<int, 2>{1, 100}, minimums, 0, 0, 96); });
}
