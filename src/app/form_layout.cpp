#include "app/form_layout.hpp"

namespace superzip::app {
namespace {
// Purpose: Inset the shell content; inputs: physical content rectangle and positive DPI; outputs: form work area.
RECT page_area(const RECT& rect, UINT dpi) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    return {rect.left + scale(30), rect.top + scale(22), rect.right - scale(30), rect.bottom - scale(22)};
}

// Purpose: Place a primary command; inputs: work area and positive DPI; outputs: a bottom-right 110x36-DIP button.
RECT primary_action_rect(const RECT& area, UINT dpi) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    return {area.right - scale(110), area.bottom - scale(54), area.right, area.bottom - scale(18)};
}

// Purpose: Place a secondary command; inputs: primary button and positive DPI; outputs: same-sized button with gap.
RECT secondary_action_rect_left_of(const RECT& primary, UINT dpi) {
    const int gap = MulDiv(12, static_cast<int>(dpi), 96);
    const int width = primary.right - primary.left;
    return {primary.left - gap - width, primary.top, primary.left - gap, primary.bottom};
}
}  // namespace

// Purpose: Compute Compress page rectangles shared by rendering and hit testing.
// Inputs: rect is the content area in physical pixels; dpi is the positive monitor DPI.
// Outputs: Returns DPI-scaled Compress page control rectangles.
CompressLayout make_compress_layout(const RECT& rect, UINT dpi) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    CompressLayout layout{};
    layout.area = page_area(rect, dpi);
    layout.start = primary_action_rect(layout.area, dpi);
    layout.stop = secondary_action_rect_left_of(layout.start, dpi);
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive_name = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.format = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.compression_level = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.method = RECT{left, layout.area.top + scale(194), left + field_w, layout.area.top + scale(244)};
    layout.block_size = RECT{mid, layout.area.top + scale(194), mid + field_w, layout.area.top + scale(244)};
    layout.advanced = RECT{left, layout.area.top + scale(270), layout.area.right, layout.area.top + scale(390)};
    layout.solid_archive = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(48),
                                layout.advanced.left + scale(310), layout.advanced.top + scale(76)};
    layout.store_timestamps = RECT{layout.advanced.left + scale(18), layout.advanced.top + scale(80),
                                   layout.advanced.left + scale(310), layout.advanced.top + scale(108)};
    layout.delete_after_compression = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(48),
                                           layout.advanced.left + scale(680), layout.advanced.top + scale(76)};
    layout.verify = RECT{layout.advanced.left + scale(342), layout.advanced.top + scale(80),
                         layout.advanced.left + scale(710), layout.advanced.top + scale(108)};
    layout.security = RECT{left, layout.area.top + scale(410), layout.area.right, layout.area.top + scale(528)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(46),
                      layout.security.left + scale(420), layout.security.top + scale(78)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(82),
                           layout.security.left + scale(420), layout.security.top + scale(114)};

    if (layout.area.bottom - layout.area.top < scale(600)) {
        const int half = (layout.area.right - layout.area.left - scale(18)) / 2;
        layout.archive_name.bottom = layout.area.top + scale(100);
        layout.destination.bottom = layout.archive_name.bottom;
        layout.format.top = layout.compression_level.top = layout.area.top + scale(112);
        layout.format.bottom = layout.compression_level.bottom = layout.area.top + scale(158);
        layout.method.top = layout.block_size.top = layout.area.top + scale(170);
        layout.method.bottom = layout.block_size.bottom = layout.area.top + scale(216);
        layout.advanced = {left, layout.area.top + scale(230), left + half, layout.area.top + scale(400)};
        layout.security = {layout.advanced.right + scale(18), layout.advanced.top, layout.area.right,
                           layout.advanced.bottom};
        int top = layout.advanced.top + scale(42);
        for (RECT* toggle :
             {&layout.solid_archive, &layout.store_timestamps, &layout.delete_after_compression, &layout.verify}) {
            *toggle = {layout.advanced.left + scale(18), top, layout.advanced.right - scale(18), top + scale(28)};
            top += scale(30);
        }
        layout.sha = {layout.security.left + scale(18), layout.security.top + scale(42),
                      layout.security.right - scale(18), layout.security.top + scale(70)};
        layout.defender = {layout.sha.left, layout.sha.bottom + scale(6), layout.sha.right,
                           layout.sha.bottom + scale(34)};
    }
    return layout;
}

// Purpose: Compute Extract page rectangles shared by rendering and hit testing.
// Inputs: rect is the content area in physical pixels; dpi is the positive monitor DPI.
// Outputs: Returns DPI-scaled Extract page control rectangles.
ExtractLayout make_extract_layout(const RECT& rect, UINT dpi) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    ExtractLayout layout{};
    layout.area = page_area(rect, dpi);
    layout.start = primary_action_rect(layout.area, dpi);
    layout.stop = secondary_action_rect_left_of(layout.start, dpi);
    const int left = layout.area.left;
    const int mid = layout.area.left + (layout.area.right - layout.area.left) / 2 + scale(14);
    const int field_w = (layout.area.right - layout.area.left) / 2 - scale(26);
    layout.archive = RECT{left, layout.area.top + scale(54), left + field_w, layout.area.top + scale(104)};
    layout.destination = RECT{mid, layout.area.top + scale(54), mid + field_w, layout.area.top + scale(104)};
    layout.path_mode = RECT{left, layout.area.top + scale(124), left + field_w, layout.area.top + scale(174)};
    layout.overwrite_policy = RECT{mid, layout.area.top + scale(124), mid + field_w, layout.area.top + scale(174)};
    layout.checks =
        RECT{layout.area.left, layout.area.top + scale(200), layout.area.right, layout.area.top + scale(332)};
    layout.verify_metadata = RECT{layout.checks.left + scale(18), layout.checks.top + scale(48),
                                  layout.checks.left + scale(420), layout.checks.top + scale(78)};
    layout.open_destination_after_extract = RECT{layout.checks.left + scale(18), layout.checks.top + scale(80),
                                                 layout.checks.left + scale(420), layout.checks.top + scale(110)};
    layout.sha = RECT{layout.checks.left + scale(470), layout.checks.top + scale(48), layout.checks.right - scale(20),
                      layout.checks.top + scale(80)};
    layout.defender = RECT{layout.checks.left + scale(470), layout.checks.top + scale(84),
                           layout.checks.right - scale(20), layout.checks.top + scale(116)};

    if (layout.area.right - layout.area.left < scale(940)) {
        const int split = layout.checks.left + (layout.checks.right - layout.checks.left) / 2;
        layout.verify_metadata.right = layout.open_destination_after_extract.right = split - scale(10);
        layout.sha.left = layout.defender.left = split + scale(10);
    }
    return layout;
}

// Purpose: Compute Settings page rectangles shared by rendering and hit testing.
// Inputs: rect is the content area in physical pixels; dpi is the positive monitor DPI.
// Outputs: Returns DPI-scaled Settings page control rectangles.
SettingsLayout make_settings_layout(const RECT& rect, UINT dpi) {
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    SettingsLayout layout{};
    layout.area = page_area(rect, dpi);
    layout.restore_defaults = RECT{layout.area.right - scale(260), layout.area.bottom - scale(54),
                                   layout.area.right - scale(126), layout.area.bottom - scale(18)};
    layout.apply = primary_action_rect(layout.area, dpi);
    const int panel_top = layout.area.top + scale(54);
    const int panel_bottom = panel_top + scale(168);
    layout.general = RECT{layout.area.left, panel_top, layout.area.left + scale(470), panel_bottom};
    layout.security = RECT{layout.general.left, layout.general.bottom + scale(16), layout.general.right,
                           layout.general.bottom + scale(176)};
    layout.performance = RECT{layout.general.right + scale(18), layout.general.top, layout.area.right, panel_bottom};
    layout.logging = RECT{layout.performance.left, layout.performance.bottom + scale(16), layout.area.right,
                          layout.performance.bottom + scale(176)};
    layout.sha = RECT{layout.security.left + scale(18), layout.security.top + scale(48),
                      layout.security.right - scale(16), layout.security.top + scale(80)};
    layout.defender = RECT{layout.security.left + scale(18), layout.security.top + scale(84),
                           layout.security.right - scale(16), layout.security.top + scale(116)};
    layout.gpu = RECT{layout.security.left + scale(18), layout.security.top + scale(120),
                      layout.security.right - scale(16), layout.security.top + scale(152)};
    layout.verify = RECT{layout.performance.left + scale(18), layout.performance.top + scale(48),
                         layout.performance.right - scale(18), layout.performance.top + scale(80)};
    const int performance_half_right =
        layout.performance.left + (layout.performance.right - layout.performance.left) / 2;
    const int logging_half_right = layout.logging.left + (layout.logging.right - layout.logging.left) / 2;
    layout.memory_policy = RECT{layout.performance.left + scale(18), layout.performance.top + scale(94),
                                performance_half_right, layout.performance.top + scale(140)};
    layout.log_level = RECT{layout.logging.left + scale(18), layout.logging.top + scale(48), logging_half_right,
                            layout.logging.top + scale(94)};
    layout.log_retention = RECT{layout.logging.left + scale(18), layout.logging.top + scale(106), logging_half_right,
                                layout.logging.top + scale(152)};
    const int log_button_width = layout.restore_defaults.right - layout.restore_defaults.left;
    layout.open_log_file = RECT{layout.logging.right - scale(18) - log_button_width, layout.logging.top + scale(82),
                                layout.logging.right - scale(18), layout.logging.top + scale(118)};
    layout.open_destination_after_operation = RECT{layout.general.left + scale(18), layout.general.top + scale(48),
                                                   layout.general.right - scale(16), layout.general.top + scale(78)};
    layout.confirm_before_deleting = RECT{layout.general.left + scale(18), layout.general.top + scale(82),
                                          layout.general.right - scale(16), layout.general.top + scale(112)};
    layout.show_operation_summary = RECT{layout.general.left + scale(18), layout.general.top + scale(116),
                                         layout.general.right - scale(16), layout.general.top + scale(146)};

    if (layout.area.right - layout.area.left < scale(940)) {
        const int general_right = layout.area.left + (layout.area.right - layout.area.left - scale(18)) / 2;
        const int shift = general_right - layout.general.right;
        layout.general.right = layout.security.right = general_right;
        layout.performance.left += shift;
        layout.logging.left += shift;
        for (RECT* toggle : {&layout.open_destination_after_operation, &layout.confirm_before_deleting,
                             &layout.show_operation_summary, &layout.sha, &layout.defender, &layout.gpu}) {
            toggle->right = general_right - scale(16);
        }
        layout.verify.left = layout.performance.left + scale(18);
        layout.memory_policy.left = layout.performance.left + scale(18);
        layout.memory_policy.right = layout.performance.right - scale(18);
        layout.log_level.left = layout.log_retention.left = layout.logging.left + scale(18);
        layout.log_level.right = layout.log_retention.right = layout.open_log_file.left - scale(12);
    }
    return layout;
}

}  // namespace superzip::app
