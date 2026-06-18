#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace superzip::app {

// Purpose: Return the visible log-retention label for a Settings dropdown row.
// Inputs: `index` is the zero-based retention selection.
// Outputs: Returns the exact GUI label, falling back to the shortest retention window for invalid indexes.
inline std::wstring_view log_retention_display_text(int index) {
    switch (index) {
    case 1:
        return L"2 weeks";
    case 2:
        return L"1 month";
    case 0:
    default:
        return L"1 week";
    }
}

// Purpose: Return the ASCII log message suffix for a Settings retention row.
// Inputs: `index` is the zero-based retention selection.
// Outputs: Returns the exact retention label for in-memory logs, matching the GUI label text.
inline std::string_view log_retention_log_text(int index) {
    switch (index) {
    case 1:
        return "2 weeks";
    case 2:
        return "1 month";
    case 0:
    default:
        return "1 week";
    }
}

// Purpose: Return the retention window length in whole days for one Settings dropdown row.
// Inputs: `index` is the zero-based retention selection.
// Outputs: Returns the configured day count; invalid indexes use the shortest safe window.
inline int log_retention_days(int index) {
    switch (index) {
    case 1:
        return 14;
    case 2:
        return 30;
    case 0:
    default:
        return 7;
    }
}

// Purpose: Remove expired and over-capacity log entries using the active retention policy.
// Inputs: `entries` contains records with a `timestamp` field, `now` is the pruning clock, `retention_index`
// selects the retention window, and `max_entries` is the hard count cap.
// Outputs: Mutates `entries` so only retained rows remain in original order.
template <typename Entry>
void prune_log_entries(std::vector<Entry>& entries, std::chrono::system_clock::time_point now, int retention_index,
                       std::size_t max_entries) {
    const auto retention = std::chrono::hours(24 * log_retention_days(retention_index));
    const auto cutoff = now - retention;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [cutoff](const Entry& entry) { return entry.timestamp < cutoff; }),
                  entries.end());
    if (entries.size() > max_entries) {
        entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(entries.size() - max_entries));
    }
}

}  // namespace superzip::app
