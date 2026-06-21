#pragma once

#include "app/main_window_state.hpp"

#include <string_view>

namespace superzip::app {

// Purpose: Decide whether a configured log level should record one event.
// Inputs: `log_level_index` is the Settings dropdown row and `severity` is the event category.
// Outputs: Returns true for Information+Warning, Warning-only, or all Debug-level events.
inline bool log_level_allows(int log_level_index, LogSeverity severity) {
    switch (log_level_index) {
    case 1:
        return severity == LogSeverity::Warning;
    case 2:
        return true;
    default:
        return severity == LogSeverity::Information || severity == LogSeverity::Warning;
    }
}

// Purpose: Return a stable ASCII label for a log severity.
// Inputs: `severity` is the event category.
// Outputs: Returns the label written to the persistent per-user log file.
inline std::string_view log_severity_text(LogSeverity severity) {
    switch (severity) {
    case LogSeverity::Information:
        return "Information";
    case LogSeverity::Warning:
        return "Warning";
    case LogSeverity::Debug:
        return "Debug";
    }
    return "Information";
}

}  // namespace superzip::app
