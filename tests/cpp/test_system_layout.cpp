#include "app/system_layout.hpp"
#include "app/performance_options.hpp"
#include "test_util.hpp"

#include <memory>
#include <limits>

namespace {
// Purpose: Assert usable contained geometry; inputs: child/parent rectangles; outputs: fails invalid dimensions.
void require_contained(const RECT& child, const RECT& parent) {
    REQUIRE_TRUE(child.right > child.left);
    REQUIRE_TRUE(child.bottom > child.top);
    REQUIRE_TRUE(child.left >= parent.left && child.right <= parent.right);
    REQUIRE_TRUE(child.top >= parent.top && child.bottom <= parent.bottom);
}
}  // namespace

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
