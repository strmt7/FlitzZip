#include "app/keyboard_focus.hpp"
#include "test_util.hpp"

#include <array>

namespace {
using flitzzip::app::FocusTarget;
using flitzzip::app::FocusTargetKind;
using flitzzip::app::KeyboardFocus;
}  // namespace

TEST_CASE(keyboard_focus_retains_row_and_header_when_selection_actions_change) {
    const FocusTarget navigation{FocusTargetKind::Navigation};
    const FocusTarget remove{FocusTargetKind::QueueRemoveSelected};
    const FocusTarget header{FocusTargetKind::QueueHeaderCheckbox};
    const FocusTarget row{FocusTargetKind::QueueRow, {}, 4};
    const std::array checked{navigation, remove, header, row};
    const std::array unchecked{navigation, header, row};
    KeyboardFocus focus;
    for (const auto& target : {header, row}) {
        focus.select(target);
        const auto before = focus.position(checked);
        const auto after = focus.position(unchecked);
        REQUIRE_TRUE(before.has_value() && after.has_value());
        REQUIRE_TRUE(checked[*before].kind == target.kind);
        REQUIRE_TRUE(unchecked[*after].kind == target.kind);
        REQUIRE_TRUE(unchecked[*after].index == target.index);
    }
}

TEST_CASE(keyboard_focus_does_not_activate_replacement_or_other_row) {
    KeyboardFocus focus;
    focus.select(FocusTarget{FocusTargetKind::CompressStart});
    const std::array running{FocusTarget{FocusTargetKind::Navigation, {}, 1},
                             FocusTarget{FocusTargetKind::CompressStop}};
    REQUIRE_TRUE(!focus.position(running));
    REQUIRE_TRUE(focus.move(running, false));
    REQUIRE_TRUE(focus.position(running) == 0);
    focus.select(FocusTarget{FocusTargetKind::QueueRow, {}, 7});
    const std::array scrolled{FocusTarget{FocusTargetKind::QueueRow, {}, 8},
                              FocusTarget{FocusTargetKind::QueueRow, {}, 9}};
    REQUIRE_TRUE(!focus.position(scrolled));
    REQUIRE_TRUE(focus.move(scrolled, true));
    REQUIRE_TRUE(focus.position(scrolled) == 1);
}

TEST_CASE(keyboard_focus_wraps_both_directions_and_handles_empty_singleton_lists) {
    KeyboardFocus focus;
    REQUIRE_TRUE(!focus.position({}));
    REQUIRE_TRUE(!focus.move({}, false));
    REQUIRE_TRUE(!focus.move({}, true));
    const std::array targets{FocusTarget{FocusTargetKind::Navigation}, FocusTarget{FocusTargetKind::SettingsApply},
                             FocusTarget{FocusTargetKind::SettingsLogLevel}};
    for (const bool reverse : {false, true}) {
        focus = KeyboardFocus{};
        for (const auto expected : (reverse ? std::array{2U, 1U, 0U} : std::array{1U, 2U, 0U})) {
            REQUIRE_TRUE(focus.move(targets, reverse));
            REQUIRE_TRUE(focus.position(targets) == expected);
        }
    }
    const std::array singleton{FocusTarget{FocusTargetKind::AboutLicenses}};
    REQUIRE_TRUE(focus.move(singleton, false));
    REQUIRE_TRUE(focus.position(singleton) == 0);
    REQUIRE_TRUE(focus.move(singleton, true));
    REQUIRE_TRUE(focus.position(singleton) == 0);
}

TEST_CASE(keyboard_focus_uses_current_geometry_and_resets_collection_identity) {
    KeyboardFocus focus;
    focus.select(FocusTarget{FocusTargetKind::HistoryRow, {10, 20, 30, 40}, 3});
    const std::array resized{FocusTarget{FocusTargetKind::Navigation},
                             FocusTarget{FocusTargetKind::HistoryRow, {100, 200, 300, 400}, 3}};
    const auto position = focus.position(resized);
    REQUIRE_TRUE(position == 1);
    REQUIRE_TRUE(resized[*position].rect.left == 100);
    focus = KeyboardFocus{};
    REQUIRE_TRUE(focus.position(resized) == 0);
}
