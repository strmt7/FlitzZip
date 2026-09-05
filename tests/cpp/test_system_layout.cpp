#include "app/system_layout.hpp"
#include "test_util.hpp"

#include <memory>

namespace {
// Purpose: Assert usable contained geometry; inputs: child/parent rectangles; outputs: fails invalid dimensions.
void require_contained(const RECT& child, const RECT& parent) {
    REQUIRE_TRUE(child.right > child.left);
    REQUIRE_TRUE(child.bottom > child.top);
    REQUIRE_TRUE(child.left >= parent.left && child.right <= parent.right);
    REQUIRE_TRUE(child.top >= parent.top && child.bottom <= parent.bottom);
}
}  // namespace

TEST_CASE(system_layout_keeps_runtime_controls_and_monitor_inside_tested_viewports) {
    for (const UINT dpi : {96U, 120U, 144U, 192U, 240U}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        for (const int width : {960, 1200, 1920}) {
            for (const int height : {600, 760, 1080}) {
                const RECT content{scale(86), scale(52), scale(width), scale(height - 34)};
                const auto layout = superzip::app::make_system_layout(content, dpi);
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
    const auto layout = superzip::app::make_system_layout(RECT{86, 52, 1200, 726}, 96);
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
                const auto body = superzip::app::make_performance_card_body(dc.get(), card, dpi, text);
                require_contained(body.plot, card);
                require_contained(body.detail, card);
                REQUIRE_TRUE(body.plot.bottom + scale(8) <= body.detail.top);
                REQUIRE_TRUE(body.plot.bottom - body.plot.top >= scale(48));
                RECT measured{body.detail.left, 0, body.detail.right, 0};
                DrawTextW(dc.get(), text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                REQUIRE_TRUE(measured.bottom <= body.detail.bottom - body.detail.top);
            }
        }
        const auto unchanged = superzip::app::make_performance_card_body(dc.get(), RECT{0, 0, scale(242), scale(408)},
                                                                         dpi, L"Read: 0 B/s\nWrite: 0 B/s");
        REQUIRE_EQ(unchanged.detail.top, scale(408) - scale(8) - scale(42));
        SelectObject(dc.get(), original);
    }
}
