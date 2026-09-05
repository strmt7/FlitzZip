#pragma once

#include "app/main_window_state.hpp"

#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace superzip::app {

// Purpose: Select a completed job's primary result without selecting an older job or an auxiliary check.
// Inputs: history contains session rows, first is the job's starting row count, operation is its primary category.
// Outputs: Returns the latest matching row index, or no selection when the job produced no matching representable row.
inline std::optional<int> operation_summary_index(std::span<const HistoryEntry> history, std::size_t first,
                                                  std::string_view operation) {
    if (first >= history.size() || history.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    for (auto end = history.size(); end > first; --end) {
        if (history[end - 1].operation == operation) {
            return static_cast<int>(end - 1);
        }
    }
    return std::nullopt;
}

// Purpose: Track job-local summary selection across History clears without retaining history data.
// Inputs: Callers serialize access with the same lock that protects their History rows.
// Outputs: Holds a row boundary and at most one pending selection; never mutates History.
class OperationSummarySelection {
  public:
    // Purpose: Begin a job or reset after History is cleared. Inputs: current row count. Outputs: Drops old selection.
    void begin(std::size_t row_count) noexcept {
        first_ = row_count;
        pending_.reset();
    }

    // Purpose: Select the job's final primary row. Inputs: current history, category, applied preference.
    // Outputs: Replaces pending selection; disabled presentation or no primary row leaves it empty.
    void complete(std::span<const HistoryEntry> history, std::string_view operation, bool enabled) {
        pending_ = enabled ? operation_summary_index(history, first_, operation) : std::nullopt;
    }

    // Purpose: Consume one pending summary. Inputs: None. Outputs: Returns the selection exactly once.
    std::optional<int> take() noexcept {
        return std::exchange(pending_, std::nullopt);
    }

  private:
    std::size_t first_ = 0;
    std::optional<int> pending_;
};

}  // namespace superzip::app
