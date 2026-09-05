#include "app/form_layout.hpp"
#include "app/dropdown_layout.hpp"
#include "test_util.hpp"

#include <array>

namespace {
// Purpose: Assert usable contained geometry; inputs: child and parent rectangles; outputs: fails invalid dimensions.
void require_contained(const RECT& child, const RECT& parent) {
    REQUIRE_TRUE(child.right > child.left && child.bottom > child.top);
    REQUIRE_TRUE(child.left >= parent.left && child.right <= parent.right);
    REQUIRE_TRUE(child.top >= parent.top && child.bottom <= parent.bottom);
}

// Purpose: Assert independent controls; inputs: physical rectangles; outputs: fails any intersecting hit targets.
template <std::size_t N> void require_separate(const std::array<RECT, N>& controls, const RECT& area) {
    for (std::size_t i = 0; i < N; ++i) {
        require_contained(controls[i], area);
        for (std::size_t j = i + 1; j < N; ++j) {
            RECT overlap{};
            REQUIRE_TRUE(!IntersectRect(&overlap, &controls[i], &controls[j]));
        }
    }
}

// Purpose: Check a compression form; inputs: production layout; outputs: fails clipped groups or overlapping controls.
void require_compress(const superzip::app::CompressLayout& layout) {
    require_contained(layout.advanced, layout.area);
    require_contained(layout.security, layout.area);
    for (const auto& rect :
         {layout.solid_archive, layout.store_timestamps, layout.delete_after_compression, layout.verify}) {
        require_contained(rect, layout.advanced);
    }
    require_contained(layout.sha, layout.security);
    require_contained(layout.defender, layout.security);
    REQUIRE_TRUE(layout.security.bottom < layout.start.top);
    REQUIRE_TRUE(layout.advanced.bottom < layout.start.top);
    require_separate(std::array{layout.archive_name, layout.destination, layout.format, layout.compression_level,
                                layout.method, layout.block_size, layout.solid_archive, layout.store_timestamps,
                                layout.delete_after_compression, layout.verify, layout.sha, layout.defender,
                                layout.stop, layout.start},
                     layout.area);
}

// Purpose: Check extraction geometry; inputs: production layout; outputs: fails clipped toggles or overlapping
// controls.
void require_extract(const superzip::app::ExtractLayout& layout) {
    require_contained(layout.checks, layout.area);
    for (const auto& rect :
         {layout.verify_metadata, layout.open_destination_after_extract, layout.sha, layout.defender}) {
        require_contained(rect, layout.checks);
    }
    REQUIRE_TRUE(layout.checks.bottom < layout.start.top);
    require_separate(std::array{layout.archive, layout.destination, layout.path_mode, layout.overwrite_policy,
                                layout.verify_metadata, layout.open_destination_after_extract, layout.sha,
                                layout.defender, layout.stop, layout.start},
                     layout.area);
}

// Purpose: Check settings geometry; inputs: production layout; outputs: fails clipped groups or overlapping controls.
void require_settings(const superzip::app::SettingsLayout& layout) {
    for (const auto& panel : {layout.general, layout.security, layout.performance, layout.logging}) {
        require_contained(panel, layout.area);
        REQUIRE_TRUE(panel.bottom < layout.apply.top);
    }
    for (const auto& rect :
         {layout.open_destination_after_operation, layout.confirm_before_deleting, layout.show_operation_summary}) {
        require_contained(rect, layout.general);
    }
    for (const auto& rect : {layout.sha, layout.defender, layout.gpu}) {
        require_contained(rect, layout.security);
    }
    for (const auto& rect : {layout.verify, layout.memory_policy}) {
        require_contained(rect, layout.performance);
    }
    for (const auto& rect : {layout.log_level, layout.log_retention, layout.open_log_file}) {
        require_contained(rect, layout.logging);
    }
    require_separate(std::array{layout.open_destination_after_operation, layout.confirm_before_deleting,
                                layout.show_operation_summary, layout.sha, layout.defender, layout.gpu, layout.verify,
                                layout.memory_policy, layout.log_level, layout.log_retention, layout.open_log_file,
                                layout.restore_defaults, layout.apply},
                     layout.area);
}
}  // namespace

TEST_CASE(form_layout_contains_controls_without_overlap_across_dpi_and_compact_boundaries) {
    for (const UINT dpi : {96U, 110U, 120U, 144U, 168U, 192U, 240U, 288U}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        for (const int width : {960, 1000, 1085, 1086, 1087, 1200, 1600}) {
            for (const int height : {600, 640, 700, 729, 730, 731, 760, 1080}) {
                const RECT content{scale(86), scale(52), scale(width), scale(height - 34)};
                require_compress(superzip::app::make_compress_layout(content, dpi));
                require_extract(superzip::app::make_extract_layout(content, dpi));
                require_settings(superzip::app::make_settings_layout(content, dpi));
            }
        }
    }
}

TEST_CASE(form_layout_preserves_default_control_and_dropdown_coordinates) {
    const RECT content{86, 52, 1200, 726};
    const auto compress = superzip::app::make_compress_layout(content, 96);
    REQUIRE_EQ(compress.format.top, 198);
    REQUIRE_EQ(compress.format.bottom, 248);
    REQUIRE_EQ(compress.destination.left, 657);
    REQUIRE_EQ(compress.advanced.top, 344);
    REQUIRE_EQ(compress.security.top, 484);
    REQUIRE_EQ(compress.start.top, 650);
    const auto extract = superzip::app::make_extract_layout(content, 96);
    REQUIRE_EQ(extract.overwrite_policy.left, 657);
    REQUIRE_EQ(extract.sha.left, 586);
    const auto settings = superzip::app::make_settings_layout(content, 96);
    REQUIRE_EQ(settings.general.right, 586);
    REQUIRE_EQ(settings.log_level.left, 622);
    REQUIRE_EQ(settings.log_level.right, 887);
    REQUIRE_EQ(settings.open_log_file.left, 1018);
    REQUIRE_EQ(settings.apply.top, 650);
}

TEST_CASE(form_layout_compact_controls_keep_native_usable_sizes) {
    const RECT content{86, 52, 960, 566};
    const auto compress = superzip::app::make_compress_layout(content, 96);
    REQUIRE_EQ(compress.advanced.top, compress.security.top);
    REQUIRE_TRUE(compress.advanced.right < compress.security.left);
    REQUIRE_TRUE(compress.archive_name.bottom - compress.archive_name.top >= 46);
    REQUIRE_TRUE(compress.verify.right - compress.verify.left >= 360);
    REQUIRE_EQ(compress.start.bottom - compress.start.top, 36);
    const auto settings = superzip::app::make_settings_layout(content, 96);
    REQUIRE_TRUE(settings.log_level.right - settings.log_level.left >= 210);
    REQUIRE_TRUE(settings.memory_policy.right - settings.memory_policy.left >= 360);
    REQUIRE_TRUE(settings.log_level.right < settings.open_log_file.left);
}

TEST_CASE(dropdown_layout_keeps_complete_native_rows_and_every_option_reachable) {
    for (const UINT dpi : {96U, 110U, 120U, 144U, 168U, 192U, 240U, 288U}) {
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        for (const int height : {600, 640, 760, 1080}) {
            const RECT content{scale(86), scale(52), scale(960), scale(height - 34)};
            const auto form = superzip::app::make_compress_layout(content, dpi);
            for (const int count : {1, 2, 3, 5, 7, 19, 26}) {
                auto menu = superzip::app::make_dropdown_layout(form.format, content, dpi, count, 0);
                require_contained(menu.menu, content);
                REQUIRE_EQ(menu.row_height, scale(count > 10 ? 28 : 32));
                REQUIRE_TRUE(menu.visible_rows > 0 && menu.visible_rows <= count);
                for (int selected = 0; selected < count; ++selected) {
                    const int first = superzip::app::dropdown_first_row_for_selection(menu, selected);
                    menu = superzip::app::make_dropdown_layout(form.format, content, dpi, count, first);
                    const int row = selected - menu.first_row;
                    REQUIRE_TRUE(row >= 0 && row < menu.visible_rows);
                    const int y = menu.row_top + row * menu.row_height + menu.row_height / 2;
                    REQUIRE_EQ(superzip::app::dropdown_option_at(menu, menu.menu.left + scale(20), y), selected);
                    REQUIRE_TRUE(y < menu.menu.bottom);
                }
                if (menu.max_first_row > 0) {
                    require_contained(menu.up, menu.menu);
                    require_contained(menu.down, menu.menu);
                    REQUIRE_EQ(superzip::app::dropdown_option_at(menu, menu.up.left + 5, menu.up.top + 1), -1);
                    REQUIRE_EQ(superzip::app::dropdown_option_at(menu, menu.down.left + 5, menu.down.top + 1), -1);
                }
                REQUIRE_EQ(superzip::app::dropdown_option_at(menu, menu.menu.right, menu.row_top), -1);
                REQUIRE_EQ(superzip::app::dropdown_option_at(menu, menu.menu.left + 10, menu.menu.bottom), -1);
            }
        }
    }
}

TEST_CASE(dropdown_layout_clamps_offsets_and_rejects_unusable_viewports) {
    const RECT anchor{116, 186, 497, 232};
    const RECT content{86, 52, 960, 566};
    const auto first = superzip::app::make_dropdown_layout(anchor, content, 96, 19, -100);
    const auto last = superzip::app::make_dropdown_layout(anchor, content, 96, 19, 100);
    REQUIRE_EQ(first.first_row, 0);
    REQUIRE_EQ(first.visible_rows, 16);
    REQUIRE_EQ(last.first_row, 3);
    REQUIRE_EQ(first.row_height, 28);
    REQUIRE_EQ(superzip::app::dropdown_first_row_for_selection(last, 0), 0);
    REQUIRE_EQ(superzip::app::dropdown_first_row_for_selection(first, 18), 3);
    REQUIRE_EQ(superzip::app::make_dropdown_layout(anchor, content, 96, 0, 0).visible_rows, 0);
    REQUIRE_EQ(superzip::app::make_dropdown_layout(anchor, RECT{86, 52, 960, 60}, 96, 19, 0).visible_rows, 0);
}
