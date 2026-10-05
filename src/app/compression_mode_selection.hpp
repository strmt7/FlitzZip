#pragma once

#include "app/main_window_state.hpp"

namespace superzip::app {

// Purpose: Gate the separate Neutron option using actual capability and the user's required-GPU policy.
// Inputs: Native-format selection, cached HIP availability, and the user's GPU requirement.
// Outputs: Returns true only when every Neutron prerequisite is satisfied.
inline bool neutron_mode_eligible(bool native_format, bool gpu_available, bool gpu_required) {
    return native_format && gpu_available && gpu_required;
}

// Purpose: Share the selectable row count between rendering, hit testing, keyboard input, and cycling.
// Inputs: Whether Neutron is eligible for the current selection.
// Outputs: Returns the nine unchanged numeric efforts plus the separate eligible Neutron row.
inline int compression_selection_count(bool neutron_eligible) {
    return kCompressionLevelOptionCount + (neutron_eligible ? 1 : 0);
}

// Purpose: Remove an unavailable Neutron selection without silently running its work on the CPU.
// Inputs: Mutable UI state and whether its selected format is native SUZIP.
// Outputs: Returns whether selection changed; selects ordinary level 9 with an explicit status when necessary.
inline bool normalize_neutron_selection(UiState& state, bool native_format) {
    if (state.compression_level_index != kNeutronCompressionLevelIndex ||
        neutron_mode_eligible(native_format, state.gpu_available, state.gpu_required)) {
        return false;
    }
    state.compression_level_index = kCompressionLevelOptionCount - 1;
    state.status = "Neutron Star Mode requires SUZIP, a compatible AMD GPU, and AMD HIP required; level 9 selected";
    return true;
}

}  // namespace superzip::app
