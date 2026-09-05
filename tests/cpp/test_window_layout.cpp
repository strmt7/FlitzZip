#include "app/form_layout.hpp"
#include "app/window_layout.hpp"
#include "test_util.hpp"

#include <memory>

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
                const auto result = superzip::app::make_window_layout(work, proposed, frame, dpi);
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
                const auto compress = superzip::app::make_compress_layout(content, dpi);
                const auto settings = superzip::app::make_settings_layout(content, dpi);
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
    const auto normal = superzip::app::make_window_layout(large, position, frame, 144);
    REQUIRE_TRUE(normal && normal->minimum_fits);
    REQUIRE_EQ(normal->bounds.left, position.left);
    REQUIRE_EQ(normal->bounds.top, position.top);
    REQUIRE_EQ(normal->bounds.right - normal->bounds.left, 1800 + frame.cx);
    REQUIRE_EQ(normal->bounds.bottom - normal->bounds.top, 1140 + frame.cy);
    const RECT small{-1920, 0, 0, 1040};
    const auto compact = superzip::app::make_window_layout(small, normal->bounds, frame, 144);
    REQUIRE_TRUE(compact && compact->minimum_fits);
    REQUIRE_EQ(compact->bounds.bottom, small.bottom);
    REQUIRE_TRUE(compact->bounds.top >= small.top);
    const auto restored = superzip::app::make_window_layout(large, compact->bounds, frame_size(192), 192);
    REQUIRE_TRUE(restored && restored->minimum_fits);
    REQUIRE_EQ(restored->bounds.right - restored->bounds.left, 2400 + frame_size(192).cx);
    REQUIRE_EQ(restored->bounds.bottom - restored->bounds.top, 1520 + frame_size(192).cy);
}

TEST_CASE(window_layout_rejects_invalid_geometry_and_reports_insufficient_work_area) {
    const RECT work{0, 0, 1920, 1040};
    REQUIRE_TRUE(!superzip::app::make_window_layout(work, work, SIZE{}, 0));
    REQUIRE_TRUE(!superzip::app::make_window_layout(work, work, SIZE{}, 1000));
    REQUIRE_TRUE(!superzip::app::make_window_layout(RECT{}, work, SIZE{}, 96));
    REQUIRE_TRUE(!superzip::app::make_window_layout(work, work, SIZE{-1, 0}, 96));
    const auto tiny = superzip::app::make_window_layout(RECT{10, 20, 210, 220}, work, frame_size(144), 144);
    REQUIRE_TRUE(tiny && !tiny->minimum_fits);
    REQUIRE_EQ(tiny->bounds.left, 10);
    REQUIRE_EQ(tiny->bounds.top, 20);
    REQUIRE_EQ(tiny->bounds.right - tiny->bounds.left - frame_size(144).cx, 1440);
    REQUIRE_EQ(tiny->bounds.bottom - tiny->bounds.top - frame_size(144).cy, 900);
}

TEST_CASE(window_layout_real_hidden_window_has_the_requested_client_and_frame_dimensions) {
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> window(
        CreateWindowExW(0, L"STATIC", L"SuperZip layout test", kStyle, 0, 0, 1200, 760, nullptr, nullptr,
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
        const auto result = superzip::app::make_window_layout(work, work, frame, dpi);
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
