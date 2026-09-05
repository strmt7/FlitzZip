#include "app/system_layout.hpp"
#include "test_util.hpp"

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
