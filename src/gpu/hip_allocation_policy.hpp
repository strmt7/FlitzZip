#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace superzip {

using HipRuntimeVersion = std::array<std::uint16_t, 4>;

// Purpose: Exclude missing or older Windows runtime metadata from stream-ordered allocation admission.
// Inputs: Numeric file version of the exact signed DLL loaded by the trusted loader, not the SDK version.
// Outputs: Returns eligibility at or above the tested 10.0.3679.0 runtime; device/pool checks remain mandatory.
inline bool hip_runtime_allows_stream_allocations(const std::optional<HipRuntimeVersion>& version) noexcept {
    constexpr HipRuntimeVersion minimum{10U, 0U, 3679U, 0U};
    return version.has_value() && *version >= minimum;
}

// Purpose: Format loaded-runtime metadata without exposing installation paths.
// Inputs: Optional four-component numeric Windows file version.
// Outputs: Returns dotted components or "unavailable" when metadata could not be read.
inline std::string hip_runtime_version_text(const std::optional<HipRuntimeVersion>& version) {
    if (!version) {
        return "unavailable";
    }
    return std::to_string((*version)[0]) + "." + std::to_string((*version)[1]) + "." + std::to_string((*version)[2]) +
           "." + std::to_string((*version)[3]);
}

}  // namespace superzip
