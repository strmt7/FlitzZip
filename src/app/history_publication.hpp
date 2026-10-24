#pragma once

#include "app/main_window_state.hpp"

#include <utility>

namespace flitzzip::app {

// Purpose: Publish a typed History result without touching window-thread presentation state.
// Inputs: Shared state exclusively locked by its caller and one owned result.
// Outputs: Appends/selects the result and requests one UI-thread reset; failed insertion changes neither.
inline void publish_history_result(UiState& state, HistoryEntry entry) {
    state.history.push_back(std::move(entry));
    state.selected_history_index = static_cast<int>(state.history.size()) - 1;
    state.history_details_reset_pending = true;
}

// Purpose: Consume presentation requests together with the exact shared state they describe.
// Inputs: Shared state exclusively locked by its window-thread caller.
// Outputs: Returns an owned frame snapshot; clears requests only after copying succeeds.
inline UiState consume_ui_snapshot(UiState& state) {
    UiState snapshot = state;
    state.history_details_reset_pending = false;
    return snapshot;
}

}  // namespace flitzzip::app
