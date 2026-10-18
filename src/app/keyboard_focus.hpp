#pragma once

#include "app/main_window_state.hpp"

#include <optional>
#include <span>

namespace flitzzip::app {

// Identifies a control independently of conditional controls earlier in the tab order.
// Row indices belong to the current collection; its owner resets focus when rows are removed.
class KeyboardFocus {
  public:
    // Purpose: Locate the focused control in the current layout without activating a replacement.
    // Inputs: `targets` contains the visible controls in traversal order.
    // Outputs: Returns the matching position, or no position when the control is no longer visible.
    std::optional<std::size_t> position(std::span<const FocusTarget> targets) const noexcept {
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (targets[i].kind == kind_ && targets[i].index == index_) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Purpose: Select a control by identity rather than its transient layout position.
    // Inputs: `target` is the control selected by navigation.
    // Outputs: Stores its kind and row/navigation index; geometry remains owned by the current layout.
    void select(const FocusTarget& target) noexcept {
        kind_ = target.kind;
        index_ = target.index;
    }

    // Purpose: Traverse visible controls, wrapping at the ends of the tab order.
    // Inputs: `targets` is the current order; `backward` selects reverse traversal.
    // Outputs: Selects the next control, or returns false for an empty list without accessing it.
    bool move(std::span<const FocusTarget> targets, bool backward) noexcept {
        if (targets.empty()) {
            return false;
        }
        const auto current = position(targets);
        std::size_t next = backward ? targets.size() - 1 : 0;
        if (current) {
            next = backward ? (*current == 0 ? targets.size() - 1 : *current - 1)
                            : (*current == targets.size() - 1 ? 0 : *current + 1);
        }
        select(targets[next]);
        return true;
    }

  private:
    FocusTargetKind kind_ = FocusTargetKind::Navigation;
    int index_ = 0;
};

}  // namespace flitzzip::app
