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

    // Purpose: Observe mouse capture owned by the actual UI thread.
    // Inputs: This session's hidden window. Outputs: Captured HWND or a test failure on native query refusal.
    HWND capture() const {
        GUITHREADINFO state{};
        state.cbSize = sizeof(state);
        REQUIRE_TRUE(GetGUIThreadInfo(GetWindowThreadProcessId(ui.window, nullptr), &state) != FALSE);
        return state.hwndCapture;
    }

    // Purpose: Locate the public Queue scrollbar body without reading the controller's scroll offset.
    // Inputs: This session's DPI-scaled table. Outputs: The visible native track bounds.
    RECT scrollbar_track() const {
        return RECT{table.right - ui.px(12) - ui.px(6), table.top + ui.px(36) + ui.px(8), table.right - ui.px(6),
                    table.bottom - ui.px(8)};
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
    // Inputs: Visible row, expected filename and row count. Outputs: Exact rendered filename match.
    void require_literal_name(int row, std::wstring_view name, int count) {
        require_native_observation(
            [&] {
                ui.paint();
                return native_cell_text_matches(ui.frame.dc(), name_cell(row, count), GetDpiForWindow(ui.window), name,
                                                flitzzip::app::kText);
            },
            "Queue filename differs from the independent row model");
    }

    // Purpose: Name an indexed Queue fixture without duplicating the native glyph oracle.
    // Inputs: Visible row, expected original index and row count. Outputs: Exact rendered filename match.
    void require_name(int row, int original_index, int count) {
        require_literal_name(row, L"row-" + std::to_wstring(original_index) + L".txt", count);
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
    // Inputs: Literal payload model, live session and expected background-worker accounting.
    // Outputs: Exact unchanged bytes and joined controller.
    void finish(unsigned attempts = 0, unsigned completions = 0) {
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            REQUIRE_EQ(read_native_fixture(inputs[index]), "Queue fixture " + std::to_string(index) + "\n");
        }
        ui.finish(attempts, completions);
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

// Purpose: Keep Queue thumb dragging bounded at both ends and stop moving after native capture is released.
// Inputs: More real queued files than fit in the viewport and out-of-track pointer movements.
// Outputs: Requires exact visible row identities, active drag capture, released capture, and unchanged source bytes.
TEST_CASE(main_window_queue_scrollbar_drag_clamps_and_releases_capture) {
    QueueSession s;
    const int count = s.visible() + 6;
    s.add(count);
    const RECT track = s.scrollbar_track();
    const int x = (track.left + track.right) / 2;
    const auto top = MAKELPARAM(x, track.top + s.ui.px(15));
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, top);
    REQUIRE_EQ(s.capture(), s.ui.window);
    const auto below = MAKELPARAM(x, track.bottom + s.ui.px(100));
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, below);
    HiddenController::send(s.ui.window, WM_LBUTTONUP, 0, below);
    REQUIRE_TRUE(s.capture() != s.ui.window);
    s.require_name(0, count - s.visible(), count);
    s.require_name(1, count - s.visible() + 1, count);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, 0, top);
    s.require_name(0, count - s.visible(), count);

    const auto bottom = MAKELPARAM(x, track.bottom - s.ui.px(15));
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, bottom);
    REQUIRE_EQ(s.capture(), s.ui.window);
    const auto above = MAKELPARAM(x, track.top - s.ui.px(100));
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, above);
    HiddenController::send(s.ui.window, WM_LBUTTONUP, 0, above);
    REQUIRE_TRUE(s.capture() != s.ui.window);
    s.require_name(0, 0, count);
    s.require_name(1, 1, count);
    s.finish();
}

// Purpose: Cancel a Queue thumb drag without retaining stale capture or preventing later page scrolling.
// Inputs: Overflowing real queue, WM_CANCELMODE, stale pressed motion and an ordinary scrollbar track click.
// Outputs: Requires no movement after cancellation and the exact expected first row after a new page click.
TEST_CASE(main_window_queue_scrollbar_cancel_restores_page_clicks) {
    QueueSession s;
    const int count = s.visible() + 6;
    s.add(count);
    const RECT track = s.scrollbar_track();
    const int x = (track.left + track.right) / 2;
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, track.top + s.ui.px(15)));
    REQUIRE_EQ(s.capture(), s.ui.window);
    HiddenController::send(s.ui.window, WM_CANCELMODE);
    REQUIRE_TRUE(s.capture() != s.ui.window);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, track.bottom));
    s.require_name(0, 0, count);
    click_native_point(s.ui.window, POINT{x, track.bottom - s.ui.px(2)});
    s.require_name(0, std::min(s.visible(), 6), count);
    s.finish();
}

// Purpose: Clamp Queue column resizing to a readable width and discard a cancelled second drag.
// Inputs: Real header grip, out-of-bounds pointer motion, native cancellation and one rendered file row.
// Outputs: Requires exact minimum separator, intact filename text, released capture and stable post-cancel pixels.
TEST_CASE(main_window_queue_column_resize_minimum_and_capture_cancel) {
    QueueSession s;
    s.add(2);
    const auto name = s.name_cell(0, 2);
    const int header_y = s.table.top + s.ui.px(18);
    const int initial_separator = name.right + s.ui.px(8);
    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(initial_separator, header_y));
    REQUIRE_EQ(s.capture(), s.ui.window);
    const auto left = MAKELPARAM(s.table.left - s.ui.px(1000), header_y);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, left);
    HiddenController::send(s.ui.window, WM_LBUTTONUP, 0, left);
    REQUIRE_TRUE(s.capture() != s.ui.window);

    const int minimum_separator = s.table.left + s.ui.px(44) + s.ui.px(90);
    s.ui.paint();
    REQUIRE_EQ(GetPixel(s.ui.frame.dc(), minimum_separator, s.table.top + s.ui.px(10)), flitzzip::app::kBorder);
    const RECT minimum_name{s.table.left + s.ui.px(44) + s.ui.px(8), s.table.top + s.ui.px(36),
                            minimum_separator - s.ui.px(8), s.table.top + s.ui.px(36) + s.ui.px(34)};
    REQUIRE_TRUE(native_cell_text_matches(s.ui.frame.dc(), minimum_name, GetDpiForWindow(s.ui.window), L"row-0.txt",
                                          flitzzip::app::kText));

    HiddenController::send(s.ui.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(minimum_separator, header_y));
    REQUIRE_EQ(s.capture(), s.ui.window);
    HiddenController::send(s.ui.window, WM_CANCELMODE);
    REQUIRE_TRUE(s.capture() != s.ui.window);
    const RECT header{s.table.left, s.table.top, s.table.right, s.table.top + s.ui.px(36)};
    const auto stable = s.ui.pixels(header);
    HiddenController::send(s.ui.window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(s.table.right + s.ui.px(100), header_y));
    REQUIRE_TRUE(s.ui.pixels(header) == stable);
    s.finish();
}

// Purpose: Keep native picker commands and Clear bound to the Queue's actual visible rows.
// Inputs: Two distinct file selections and one folder through scoped GUI picker values on an invisible window.
// Outputs: Exact row order and names, restored empty pixels and unchanged source bytes after Clear.
TEST_CASE(main_window_queue_picker_files_folder_and_clear_preserve_sources) {
    flitzzip_test::ScopedEnvironment selected_files(L"FLITZZIP_GUI_SMOKE_FILE_SELECTION");
    flitzzip_test::ScopedEnvironment selected_folder(L"FLITZZIP_GUI_SMOKE_FOLDER_SELECTION");
    QueueSession s;
    ReleaseOnExit release(s.ui.launcher.released);
    const auto first = s.ui.fixture.root / "alpha.txt";
    const auto second = s.ui.fixture.root / "beta.txt";
    const auto folder = s.ui.fixture.root / "selected-folder";
    std::filesystem::create_directory(folder);
    const auto child = folder / "child.txt";
    s.inputs = {first, second, child};
    for (std::size_t index = 0; index < s.inputs.size(); ++index) {
        std::ofstream output(s.inputs[index], std::ios::binary);
        output << "Queue fixture " << index << '\n';
        REQUIRE_TRUE(output.good());
    }
    const auto empty_name = s.ui.pixels(s.name_cell(0, 0));
    const int header_top = s.table.top - s.ui.px(54);
    const int header_bottom = header_top + s.ui.px(flitzzip::app::kPageHeaderHeight);
    const RECT clear{s.table.right - s.ui.px(72), header_top, s.table.right, header_bottom};
    const RECT add_folder{clear.left - s.ui.px(12) - s.ui.px(108), header_top, clear.left - s.ui.px(12), header_bottom};
    const RECT add_files{add_folder.left - s.ui.px(12) - s.ui.px(96), header_top, add_folder.left - s.ui.px(12),
                         header_bottom};

    const auto chosen = first.wstring() + L";" + second.wstring();
    selected_files.set(chosen.c_str());
    click_native_control(s.ui.window, add_files);
    s.require_literal_name(0, L"alpha.txt", 2);
    s.require_literal_name(1, L"beta.txt", 2);

    selected_folder.set(folder.c_str());
    click_native_control(s.ui.window, add_folder);
    s.require_literal_name(0, L"alpha.txt", 3);
    s.require_literal_name(1, L"beta.txt", 3);
    s.require_literal_name(2, L"selected-folder", 3);

    auto folder_worker_entered = s.ui.launcher.entered.get_future();
    require_native_observation(
        [&] {
            s.ui.paint();
            return folder_worker_entered.wait_for(0s) == std::future_status::ready;
        },
        "Queue folder-size work was not scheduled after rendering the selected folder");
    release.release();

    click_native_control(s.ui.window, clear);
    REQUIRE_TRUE(s.ui.pixels(s.name_cell(0, 0)) == empty_name);
    s.finish(1, 1);
}

// Purpose: Reject an entire invalid shell drop and preserve normal admission after each rejection.
// Inputs: Real source files, native outside/empty/mixed drops, another page and a final valid Queue drop.
// Outputs: No rejected item appears; both valid files appear once in order with unchanged bytes.
TEST_CASE(main_window_queue_drop_rejections_are_atomic_and_recoverable) {
    QueueSession s;
    const auto valid = s.ui.fixture.root / L"drop.txt";
    const auto second = s.ui.fixture.root / L"drop-2.txt";
    s.inputs = {valid, second};
    for (std::size_t index = 0; index < s.inputs.size(); ++index) {
        std::ofstream output(s.inputs[index], std::ios::binary);
        output << "Queue fixture " << index << '\n';
        REQUIRE_TRUE(output.good());
    }
    const auto native_valid = flitzzip::windows_regular_path(valid).native();
    const auto native_second = flitzzip::windows_regular_path(second).native();
    const POINT inside{s.ui.px(180), s.ui.px(220)};
    const POINT outside{inside.x, s.table.top - s.ui.px(10)};
    const auto empty_row = s.ui.pixels(s.name_cell(0, 0));
    const auto require_empty = [&] { REQUIRE_TRUE(s.ui.pixels(s.name_cell(0, 0)) == empty_row); };

    ControllerFixture::drop_paths(s.ui.window, {native_valid}, outside);
    require_empty();
    ControllerFixture::drop_paths(s.ui.window, {native_valid, L"relative.txt"}, inside);
    require_empty();
    ControllerFixture::drop_paths(s.ui.window, {}, inside);
    require_empty();
    ControllerFixture::show_compression(s.ui.window);
    ControllerFixture::drop_paths(s.ui.window, {native_valid}, inside);
    ControllerFixture::show_queue(s.ui.window);
    require_empty();

    ControllerFixture::drop_paths(s.ui.window, {native_valid, native_second}, inside);
    s.require_literal_name(0, L"drop.txt", 2);
    s.require_tick(0, true);
    s.require_literal_name(1, L"drop-2.txt", 2);
    s.require_tick(1, true);
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
