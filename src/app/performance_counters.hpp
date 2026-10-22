#pragma once

#include <cmath>
#include <optional>
#include <string_view>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

namespace flitzzip::app {

// Purpose: Admit both documented valid PDH counter states without accepting unavailable or stale error data.
// Inputs: One formatted counter's CStatus. Outputs: True for valid or newly observed data, false otherwise.
inline constexpr bool valid_pdh_counter_status(DWORD status) noexcept {
    return status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA;
}

// Purpose: Preserve individual counter availability rather than replacing missing data with invented zeroes.
// Inputs: PDH call status, formatted sample status and a nonnegative counter value.
// Outputs: The observed finite value, including actual zero, or no value on API/status/value failure.
inline std::optional<double> admitted_pdh_counter(PDH_STATUS call_status, DWORD sample_status, double value) noexcept {
    if (call_status != ERROR_SUCCESS || !valid_pdh_counter_status(sample_status) || !std::isfinite(value) ||
        value < 0.0) {
        return std::nullopt;
    }
    return value;
}

// Purpose: Prevent a drive selection change from relabeling another drive's current or historical measurements.
// Inputs: Independently available counter value, its captured LogicalDisk instance and the selected instance.
// Outputs: The original optional value only for a nonempty exact identity match; otherwise no measurement.
inline std::optional<double> matching_drive_counter(std::optional<double> value, std::wstring_view measured_drive,
                                                    std::wstring_view selected_drive) noexcept {
    return !selected_drive.empty() && measured_drive == selected_drive ? value : std::nullopt;
}

}  // namespace flitzzip::app
