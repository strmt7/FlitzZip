#include "native_window_history_fixture.hpp"
#include "zip/zip_adapter.hpp"

using namespace flitzzip_test::native_window;

// Purpose: Prove that the shared text oracle rejects wrong glyphs while a real keyboard-focus outline is present.
// Inputs: Actual Settings history row, correct literals and same-color wrong operation/name/status controls.
// Outputs: Positive recognition and three independent rejections without changing product state for coverage.
TEST_CASE(main_window_history_text_oracle_rejects_wrong_values_under_focus) {
    HistorySession s;
    s.settings();
    s.open();
    s.ui.key(VK_END);
    s.ui.key(VK_RETURN);
    s.require_selected(0);
    s.require_row(0, 0);
    const auto cells = s.cells(0);
    const auto dpi = GetDpiForWindow(s.ui.window);
    REQUIRE_TRUE(!native_cell_text_matches(s.ui.frame.dc(), cells[1], dpi, L"Compress", flitzzip::app::kText));
    REQUIRE_TRUE(!native_cell_text_matches(s.ui.frame.dc(), cells[2], dpi, L"Settingx"));
    REQUIRE_TRUE(!native_cell_text_matches(s.ui.frame.dc(), cells[4], dpi, L"Failure", flitzzip::app::kOk));
    s.finish();
}

// Purpose: Record a real archive SHA-256 check only after successful ZIP publication.
// Inputs: Saved integrity preference, a Windows publication refusal, then a valid small source.
// Outputs: Failed work has no Security result; successful ZIP readback has its own successful Security row.
TEST_CASE(main_window_history_integrity_result_requires_published_archive) {
    HistorySession s(true, true);
    s.compress(false);
    s.open();
    s.filter(3, 0);
    s.require_placeholder(L"No operations match the current filters.");

    s.compress(true);
    const auto restored = s.ui.fixture.root / L"restored";
    flitzzip::extract_zip(s.artifacts.back().first, restored, false);
    REQUIRE_EQ(read_native_fixture(restored / L"source.txt"), "controller ownership fixture\n");
    s.rows.push_back({L"Security", L"FlitzZip-output.zip", true});
    s.open();
    s.filter(3, 1);
    s.require_row(0, 2);
    s.finish();
}

// Purpose: Keep empty-session and empty-filter presentation distinct and Clear safe before any completed operation.
// Inputs: Every ordinary operation/status combination with no jobs. Outputs: Literal empty session and reusable
// filters.
TEST_CASE(main_window_history_empty_session_filters_and_clear) {
    HistorySession s;
    s.open();
    for (const int operation : {0, 1, 2, 3, 4}) {
        for (const int status : {0, 1, 2}) {
            s.filter(operation, status);
            s.require_placeholder(L"No completed operations in this session yet.");
        }
    }
    s.clear();
    s.ui.require_field(s.layout().operation_filter, L"Settings");
    s.ui.require_field(s.layout().status_filter, L"Failure");
    s.finish();
}

// Purpose: Preserve insertion order and actual result status across independent Settings and ZIP consumers.
// Inputs: Five known public operations. Outputs: Exact visible operation/name/status glyphs and retained archive bytes.
TEST_CASE(main_window_history_real_results_match_independent_rows) {
    HistorySession s;
    s.settings();
    s.compress(true);
    s.compress(false);
    s.settings();
    s.compress(true);
    s.open();
    for (std::size_t index = 0; index < s.rows.size(); ++index) {
        s.require_row(static_cast<int>(index), index);
    }
    s.finish();
}

// Purpose: Apply operation and outcome as a conjunction without dropping or leaking rows from another category.
// Inputs: Mixed results and the complete ordinary filter matrix. Outputs: Independent row identities or empty-filter
// text.
TEST_CASE(main_window_history_filter_conjunction_truth_table) {
    HistorySession s;
    s.seed(7);
    constexpr std::array<std::array<std::size_t, 3>, 5> counts{{{7, 5, 2}, {3, 1, 2}, {0, 0, 0}, {0, 0, 0}, {4, 4, 0}}};
    for (const int operation : {0, 1, 2, 3, 4}) {
        for (const int status : {0, 1, 2}) {
            s.filter(operation, status);
            const auto visible = s.visible_rows();
            REQUIRE_EQ(visible.size(), counts.at(operation).at(status));
            if (visible.empty()) {
                s.require_placeholder(L"No operations match the current filters.");
            } else {
                for (std::size_t index = 0; index < visible.size(); ++index) {
                    s.require_row(static_cast<int>(index), visible[index]);
                }
            }
        }
    }
    s.finish();
}

// Purpose: Discard uncommitted filter highlights on Escape and outside clicks.
// Inputs: Nondefault filters and speculative first/last choices. Outputs: Original literal filters and row population.
TEST_CASE(main_window_history_dropdown_cancel_preserves_filters) {
    HistorySession s;
    s.seed(6);
    s.filter(1, 2);
    click_native_control(s.ui.window, s.layout().operation_filter);
    s.ui.key(VK_END);
    s.ui.key(VK_ESCAPE);
    click_native_control(s.ui.window, s.layout().status_filter);
    s.ui.key(VK_HOME);
    click_native_control(s.ui.window, RECT{s.ui.px(200), s.ui.px(12), s.ui.px(220), s.ui.px(32)});
    s.ui.require_field(s.layout().operation_filter, L"Compress");
    s.ui.require_field(s.layout().status_filter, L"Failure");
    s.require_row(0, s.visible_rows().front());
    s.finish();
}

// Purpose: Bind a clicked visible row to its real details while header clicks preserve that selection.
// Inputs: Two distinct operations and a fixed-header click. Outputs: Row selection and different native detail text.
TEST_CASE(main_window_history_pointer_selection_and_header_isolation) {
    HistorySession s;
    s.seed(5);
    click_native_control(s.ui.window, s.row_rect(0));
    s.require_selected(0);
    const auto first = s.ui.pixels(history_detail_region(s));
    click_native_control(s.ui.window, s.row_rect(1));
    s.require_selected(1);
    REQUIRE_TRUE(s.ui.pixels(history_detail_region(s)) != first);
    const auto second = s.ui.pixels(history_detail_region(s));
    click_native_control(s.ui.window, s.row_rect(-1));
    s.require_selected(1);
    REQUIRE_TRUE(s.ui.pixels(history_detail_region(s)) == second);
    s.finish();
}

// Purpose: Clamp keyboard row navigation to the filtered list rather than adjacent hidden original indices.
// Inputs: Compress-only rows, both boundaries and both activation keys. Outputs: Stable filtered identities and
// selection.
TEST_CASE(main_window_history_keyboard_filtered_rows_clamp_and_activate) {
    HistorySession s;
    s.seed(8);
    s.filter(1, 0);
    const auto visible = s.visible_rows();
    s.ui.key(VK_END);
    s.ui.key(VK_RETURN);
    s.require_selected(static_cast<int>(visible.size()) - 1);
    for (int index = 0; index < 12; ++index) {
        s.ui.key(VK_UP);
    }
    s.require_selected(0);
    s.require_row(0, visible.front());
    s.ui.key(VK_SPACE);
    for (int index = 0; index < 12; ++index) {
        s.ui.key(VK_DOWN);
    }
    s.require_selected(static_cast<int>(visible.size()) - 1);
    s.ui.key(VK_RETURN);
    s.require_row(static_cast<int>(visible.size()) - 1, visible.back());
    s.finish();
}

// Purpose: Keep row navigation from consuming arrows while a navigation-rail target owns keyboard focus.
// Inputs: Selected History row followed by Home and vertical arrows. Outputs: Unchanged History selection and filters.
TEST_CASE(main_window_history_navigation_focus_does_not_move_rows) {
    HistorySession s;
    s.seed(5);
    click_native_control(s.ui.window, s.row_rect(2));
    s.ui.key(VK_HOME);
    s.ui.key(VK_DOWN);
    s.ui.key(VK_UP);
    s.require_selected(2);
    require_native_navigation_selection(s.ui.window, s.ui.frame.dc(), 4);
    s.finish();
}

// Purpose: Accumulate high-resolution wheel input only over an overflowing History table.
// Inputs: Two half-detents, outside input and header snapshot. Outputs: Exact three-row movement and fixed header.
TEST_CASE(main_window_history_table_wheel_fractional_and_outside_regions) {
    HistorySession s;
    s.seed(24);
    const auto table = s.layout().table;
    const auto header = s.ui.pixels(s.row_rect(-1));
    const auto point = native_rect_center(s.row_rect(0));
    s.ui.wheel(-60, point);
    s.require_row(0, 0);
    s.ui.wheel(-120, POINT{s.ui.px(10), s.ui.px(12)});
    s.require_row(0, 0);
    s.ui.wheel(-60, point);
    s.require_row(0, 3);
    REQUIRE_TRUE(s.ui.pixels(s.row_rect(-1)) == header);
    s.ui.wheel(120, point);
    s.require_row(0, 0);
    REQUIRE_TRUE(table.top < point.y);
    s.finish();
}

// Purpose: Reset an overflowing table offset when a filter changes its membership.
// Inputs: Scrolled mixed list and small failure-only subset. Outputs: First matching original row remains visible.
TEST_CASE(main_window_history_filter_resets_table_offset) {
    HistorySession s;
    s.seed(24);
    const auto point = native_rect_center(s.row_rect(0));
    s.ui.wheel(-360, point);
    s.require_row(0, 9);
    s.require_row(1, 10);
    s.require_row(2, 11);
    s.filter(1, 2);
    const auto visible = s.visible_rows();
    s.require_row(0, visible.front());
    s.filter(0, 0);
    s.require_row(0, 0);
    s.finish();
}

// Purpose: Retain filter and row selection across ordinary page navigation without recording spurious work.
// Inputs: Selected filtered row and About/History transitions. Outputs: Identical details and expected filters.
TEST_CASE(main_window_history_navigation_preserves_filters_and_selection) {
    HistorySession s;
    s.seed(6);
    s.filter(1, 2);
    click_native_control(s.ui.window, s.row_rect(0));
    const auto before = s.ui.pixels(history_detail_region(s));
    click_native_navigation(s.ui.window, 7);
    s.open();
    s.require_selected(0);
    s.ui.require_field(s.layout().operation_filter, L"Compress");
    s.ui.require_field(s.layout().status_filter, L"Failure");
    REQUIRE_TRUE(s.ui.pixels(history_detail_region(s)) == before);
    s.finish();
}

// Purpose: Keep clearing session presentation separate from saved preferences and created archive data.
// Inputs: Real successful and failed archive operations, filter and selected row. Outputs: Empty reusable History only.
TEST_CASE(main_window_history_clear_preserves_artifacts_preferences_and_filters) {
    HistorySession s;
    s.seed(8);
    const auto settings = read_native_fixture(flitzzip::app::settings_file_path());
    s.filter(1, 2);
    click_native_control(s.ui.window, s.row_rect(0));
    s.clear();
    s.require_placeholder(L"Selected operation details will appear here", true);
    s.ui.require_field(s.layout().operation_filter, L"Compress");
    s.ui.require_field(s.layout().status_filter, L"Failure");
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), settings);
    s.filter(0, 0);
    s.settings();
    s.open();
    s.require_row(0, 0);
    s.require_selected(0);
    s.finish();
}

// Purpose: Prevent a fractional pre-clear wheel event from moving a freshly populated session.
// Inputs: Half-detent, Clear, repopulation and one half-detent. Outputs: Fresh first row until a complete new detent.
TEST_CASE(main_window_history_clear_resets_wheel_remainder) {
    HistorySession s;
    s.seed(24);
    s.ui.wheel(-60, native_rect_center(s.row_rect(0)));
    s.clear();
    s.seed(24);
    const auto point = native_rect_center(s.row_rect(0));
    s.ui.wheel(-60, point);
    s.require_row(0, 0);
    s.ui.wheel(-60, point);
    s.require_row(0, 3);
    s.finish();
}

// Purpose: Reject a stale hidden selection after a filter removes the previously selected original row.
// Inputs: Selected Settings row, Compress filter and first visible pointer selection. Outputs: Correct reset and
// details.
TEST_CASE(main_window_history_filter_hides_selection_then_pointer_rebinds) {
    HistorySession s;
    s.seed(7);
    click_native_control(s.ui.window, s.row_rect(0));
    s.filter(1, 0);
    s.require_placeholder(L"Selected operation details will appear here", true);
    click_native_control(s.ui.window, s.row_rect(0));
    s.require_selected(0);
    s.require_row(0, s.visible_rows().front());
    s.finish();
}

// Purpose: Contain wheel input when a filter leaves fewer rows than the table can display.
// Inputs: Short Compress subset and extreme wheel deltas. Outputs: Identical first/last visible row identities.
TEST_CASE(main_window_history_nonoverflow_wheel_is_inert) {
    HistorySession s;
    s.seed(8);
    s.filter(1, 0);
    const auto visible = s.visible_rows();
    for (const short delta : {-12000, 12000, -60, 60}) {
        s.ui.wheel(delta, native_rect_center(s.row_rect(0)));
        s.require_row(0, visible.front());
        s.require_row(static_cast<int>(visible.size()) - 1, visible.back());
    }
    s.finish();
}

// Purpose: Make details text scroll independently of the table, and reset its position on reselection.
// Inputs: A long real archive path and bounded wheel input. Outputs: Changed details, fixed table, exact reset pixels.
TEST_CASE(main_window_history_details_scroll_isolated_and_selection_resets) {
    HistorySession s;
    s.compress(true, true);
    s.open();
    click_native_control(s.ui.window, s.row_rect(0));
    const auto area = history_detail_region(s);
    const auto top = settled_history_pixels(s, area);
    const auto row = s.ui.pixels(s.row_rect(0));
    s.ui.wheel(-12000, native_rect_center(area));
    const auto bottom = settled_history_pixels(s, area);
    REQUIRE_TRUE(bottom != top);
    REQUIRE_TRUE(s.ui.pixels(s.row_rect(0)) == row);
    click_native_control(s.ui.window, s.row_rect(0));
    REQUIRE_TRUE(settled_history_pixels(s, area) == top);
    s.finish();
}

// Purpose: Reset details scrolling when filters hide and then restore an operation.
// Inputs: Long details, end scrolling and an empty Extract filter. Outputs: Honest placeholder and original top pixels.
TEST_CASE(main_window_history_details_filter_reset_restores_top) {
    HistorySession s;
    s.compress(true, true);
    s.open();
    const auto area = history_detail_region(s);
    const auto top = settled_history_pixels(s, area);
    s.ui.wheel(-12000, native_rect_center(area));
    REQUIRE_TRUE(settled_history_pixels(s, area) != top);
    s.filter(2, 0);
    s.require_placeholder(L"Selected operation details will appear here", true);
    s.filter(0, 0);
    REQUIRE_TRUE(settled_history_pixels(s, area) == top);
    s.finish();
}

// Purpose: Keep complete and split high-resolution details-wheel input equivalent across selection resets.
// Inputs: One detent versus two half-detents on long actual details. Outputs: Exact final text pixels.
TEST_CASE(main_window_history_details_fractional_wheel_matches_full_detent) {
    HistorySession s;
    s.compress(true, true);
    s.open();
    const auto area = history_detail_region(s);
    const auto point = native_rect_center(area);
    s.ui.wheel(-120, point);
    const auto full = settled_history_pixels(s, area);
    click_native_control(s.ui.window, s.row_rect(0));
    s.ui.wheel(-60, point);
    s.ui.wheel(-60, point);
    REQUIRE_TRUE(settled_history_pixels(s, area) == full);
    s.finish();
}
