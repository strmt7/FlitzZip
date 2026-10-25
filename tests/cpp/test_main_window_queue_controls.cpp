#include "native_window_visual_fixture.hpp"

namespace {

using namespace flitzzip_test::native_window;

// Purpose: Exercise native Queue controls with small distinct files and an independently specified row model.
// Inputs: Isolated hidden session. Outputs: Public table geometry and literal filename/checkbox observations.
class QueueSession final {
  public:
    VisualSession ui;
    RECT table{};
    std::vector<std::filesystem::path> inputs;

    // Purpose: Locate the documented fixed-header table. Inputs: Current DPI. Outputs: Table body and header bounds.
    QueueSession() {
        const auto content = native_content_rect(ui.window);
        table = RECT{content.left + ui.px(30), content.top + ui.px(22 + 54), content.right - ui.px(30),
                     content.bottom - ui.px(22)};
    }

    // Purpose: Count complete visible rows independently from actual client geometry.
    // Inputs: Physical table bounds. Outputs: Number of full 34-DIP rows below the 36-DIP header.
    int visible() const {
        return (table.bottom - table.top - ui.px(36)) / ui.px(34);
    }

    // Purpose: Supply distinguishable real inputs through the existing native HDROP consumer.
    // Inputs: Bounded count below 64. Outputs: Ordered queue rows and source files with independent indexed payloads.
    void add(int count) {
        REQUIRE_TRUE(count > 0 && count < 64);
        for (int index = 0; index < count; ++index) {
            const auto path = ui.fixture.root / ("row-" + std::to_string(index) + ".txt");
            {
                std::ofstream output(path, std::ios::binary);
                output << "Queue fixture " << index << '\n';
                REQUIRE_TRUE(output.good());
            }
            inputs.push_back(path);
            ControllerFixture::queue_path(ui.window, path);
        }
    }

    // Purpose: Locate a header or visible row checkbox without private fields.
    // Inputs: Minus one for the header, otherwise visible row index. Outputs: A small interior hit rectangle.
    RECT tick(int row) const {
        const int y = row < 0 ? table.top + ui.px(18) : table.top + ui.px(36) + row * ui.px(34) + ui.px(17);
        const int x = table.left + ui.px(22);
        return RECT{x - ui.px(10), y - ui.px(10), x + ui.px(10), y + ui.px(10)};
    }

    // Purpose: Observe the checkbox's real fill, paired with positive and negative state controls.
    // Inputs: Visible row/header and expected state. Outputs: Exact accent presence away from the checkmark and border.
    void require_tick(int row, bool checked) {
        const auto rect = tick(row);
        ui.paint();
        const auto fill =
            GetPixel(ui.frame.dc(), (rect.left + rect.right) / 2 - ui.px(4), (rect.top + rect.bottom) / 2 - ui.px(4));
        REQUIRE_TRUE(fill != CLR_INVALID);
        REQUIRE_EQ(fill == flitzzip::app::kAccent, checked);
    }

    // Purpose: Resolve a name cell using the public column layout and current expected row count.
    // Inputs: Visible row and independently tracked expected count. Outputs: Rendered filename cell bounds.
    RECT name_cell(int row, int count) const {
        using namespace flitzzip::app;
        const int right = table.right - (count > visible() ? ui.px(20) : 0);
        const int available = right - table.left - ui.px(44 + 22);
        const auto widths = fit_table_columns(std::array<double, 4>{276, 100, 96, 468}, kQueueColumnMinimums, available,
                                              GetDpiForWindow(ui.window));
        const int top = table.top + ui.px(36) + row * ui.px(34);
        return RECT{table.left + ui.px(44 + 8), top, table.left + ui.px(44) + widths[0] - ui.px(8), top + ui.px(34)};
    }

    // Purpose: Read literal filenames from native glyphs instead of calling a Queue accessor.
    // Inputs: Visible row, expected original index and row count. Outputs: Exact rendered filename match.
    void require_name(int row, int original_index, int count) {
        const auto name = L"row-" + std::to_wstring(original_index) + L".txt";
        require_native_observation(
            [&] {
                ui.paint();
                return native_cell_text_matches(ui.frame.dc(), name_cell(row, count), GetDpiForWindow(ui.window), name,
                                                flitzzip::app::kText);
            },
            "Queue filename differs from the independent row model");
    }

    // Purpose: Activate Remove Selected through normal keyboard focus without copying its adaptive button width.
    // Inputs: At least one checked row. Outputs: One native command on the first non-navigation target.
    void remove() {
        ui.key(VK_HOME);
        for (int index = 0; index < 8; ++index) {
            ui.key(VK_RIGHT);
        }
        ui.key(VK_RETURN);
    }

    // Purpose: Verify Queue operations never modify or delete the admitted source files.
    // Inputs: Literal payload model and live session. Outputs: Exact unchanged bytes and joined controller.
    void finish() {
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            REQUIRE_EQ(read_native_fixture(inputs[index]), "Queue fixture " + std::to_string(index) + "\n");
        }
        ui.finish();
    }
};

}  // namespace

// Purpose: Make header selection reflect all, none and partial row choices without changing queue membership.
// Inputs: Three native rows and header/row clicks. Outputs: Exact checkbox truth table and unchanged row order.
TEST_CASE(main_window_queue_header_selection_truth_table) {
    QueueSession s;
    s.add(3);
    s.require_tick(-1, true);
    click_native_control(s.ui.window, s.tick(1));
    s.require_tick(-1, false);
    s.require_tick(0, true);
    s.require_tick(1, false);
    s.require_tick(2, true);
    click_native_control(s.ui.window, s.tick(-1));
    for (int row = -1; row < 3; ++row) {
        s.require_tick(row, true);
    }
    click_native_control(s.ui.window, s.tick(-1));
    for (int row = -1; row < 3; ++row) {
        s.require_tick(row, false);
    }
    for (int row = 0; row < 3; ++row) {
        s.require_name(row, row, 3);
    }
    s.finish();
}

// Purpose: Remove only checked rows, preserving unchecked state, order and all source file bytes.
// Inputs: Four files with two nonadjacent unchecked rows. Outputs: Exact retained membership and later header reuse.
TEST_CASE(main_window_queue_remove_checked_preserves_unchecked_and_files) {
    QueueSession s;
    s.add(4);
    click_native_control(s.ui.window, s.tick(1));
    click_native_control(s.ui.window, s.tick(3));
    s.remove();
    s.require_name(0, 1, 2);
    s.require_name(1, 3, 2);
    s.require_tick(-1, false);
    s.require_tick(0, false);
    s.require_tick(1, false);
    click_native_control(s.ui.window, s.tick(-1));
    s.require_tick(0, true);
    s.require_tick(1, true);
    s.remove();
    ControllerFixture::queue_path(s.ui.window, s.inputs[2]);
    s.require_name(0, 2, 1);
    s.require_tick(0, true);
    s.finish();
}

// Purpose: Keep row highlighting separate from operation inclusion.
// Inputs: A deselected row clicked in its filename cell. Outputs: It remains unchecked and survives Remove Selected.
TEST_CASE(main_window_queue_row_click_does_not_change_inclusion) {
    QueueSession s;
    s.add(3);
    click_native_control(s.ui.window, s.tick(1));
    click_native_control(s.ui.window, s.name_cell(1, 3));
    s.require_tick(1, false);
    s.remove();
    s.require_name(0, 1, 1);
    s.require_tick(0, false);
    s.finish();
}

// Purpose: Keep keyboard row focus clamped, and make Space affect exactly the focused row.
// Inputs: End, repeated Down/Up, Space and a real removal command. Outputs: Independent final membership, no
// wraparound.
TEST_CASE(main_window_queue_keyboard_rows_clamp_and_toggle_exact_target) {
    QueueSession s;
    s.add(3);
    s.ui.key(VK_END);
    s.ui.key(VK_DOWN);
    s.ui.key(VK_DOWN);
    s.ui.key(VK_SPACE);
    s.require_tick(2, false);
    for (int repeat = 0; repeat < 4; ++repeat) {
        s.ui.key(VK_UP);
    }
    s.ui.key(VK_SPACE);
    s.require_tick(0, false);
    s.require_tick(1, true);
    s.remove();
    s.require_name(0, 0, 2);
    s.require_name(1, 2, 2);
    s.finish();
}

// Purpose: Accumulate high-resolution wheel deltas only inside an overflowing Queue while retaining its header.
// Inputs: Two half-detents, outside-table wheel input and reverse motion. Outputs: Exact row identities and header
// position.
TEST_CASE(main_window_queue_wheel_fractional_and_outside_hit_regions) {
    QueueSession s;
    const int count = s.visible() + 6;
    s.add(count);
    const POINT point{s.table.left + s.ui.px(80), s.table.top + s.ui.px(80)};
    const RECT header{s.table.left + s.ui.px(52), s.table.top + s.ui.px(3), s.table.left + s.ui.px(130),
                      s.table.top + s.ui.px(31)};
    const auto original_header = s.ui.pixels(header);
    s.ui.wheel(-WHEEL_DELTA / 2, point);
    s.require_name(0, 0, count);
    s.ui.wheel(-WHEEL_DELTA, POINT{s.table.left - s.ui.px(10), s.table.top + s.ui.px(80)});
    s.require_name(0, 0, count);
    s.ui.wheel(-WHEEL_DELTA / 2, point);
    s.require_name(0, 3, count);
    REQUIRE_TRUE(s.ui.pixels(header) == original_header);
    s.ui.wheel(WHEEL_DELTA, point);
    s.require_name(0, 0, count);
    s.finish();
}

// Purpose: Map visible checkbox coordinates to scrolled rows and preserve the intended original file.
// Inputs: Overflowing queue, one detent, first visible checkbox and Remove Selected. Outputs: Only original row three
// remains.
TEST_CASE(main_window_queue_scrolled_checkbox_maps_to_original_row) {
    QueueSession s;
    const int count = s.visible() + 6;
    s.add(count);
    s.ui.wheel(-WHEEL_DELTA, POINT{s.table.left + s.ui.px(80), s.table.top + s.ui.px(80)});
    s.require_name(0, 3, count);
    click_native_control(s.ui.window, s.tick(0));
    s.require_tick(0, false);
    s.remove();
    s.require_name(0, 3, 1);
    s.require_tick(0, false);
    s.finish();
}

// Purpose: Reset scroll, selection and fractional-wheel state when Clear removes an overflowing queue.
// Inputs: Scrolled queue with a pending half-detent, Clear and fresh small admission. Outputs: First-row reuse and
// correct selection.
TEST_CASE(main_window_queue_clear_resets_scroll_and_reuses_metadata) {
    QueueSession s;
    const int count = s.visible() + 6;
    s.add(count);
    const POINT point{s.table.left + s.ui.px(80), s.table.top + s.ui.px(80)};
    s.ui.wheel(-WHEEL_DELTA, point);
    s.ui.wheel(-WHEEL_DELTA / 2, point);
    ControllerFixture::clear_queue(s.ui.window);
    ControllerFixture::clear_queue(s.ui.window);
    ControllerFixture::queue_path(s.ui.window, s.inputs.back());
    s.require_name(0, count - 1, 1);
    s.require_tick(0, true);
    s.require_tick(-1, true);
    s.ui.wheel(-WHEEL_DELTA, point);
    s.require_name(0, count - 1, 1);
    s.finish();
}
