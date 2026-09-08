#pragma once

#include "core/progress.hpp"

#include <filesystem>
#include <utility>

namespace superzip::app {

// Purpose: Resolve the folder requested by the job's captured presentation preferences.
// Inputs: operation, output path, applied global preference, and per-extraction preference.
// Outputs: Returns one destination folder, or an empty path for disabled or non-output operations.
inline std::filesystem::path operation_destination_path(OperationKind operation, const std::filesystem::path& output,
                                                        bool after_operation, bool after_extract) {
    if (output.empty()) {
        return {};
    }
    if (operation == OperationKind::Extract && (after_operation || after_extract)) {
        return output;
    }
    if (operation == OperationKind::Compress && after_operation) {
        return output.parent_path();
    }
    return {};
}

// Purpose: Hand one successful job's captured destination from the worker to the UI thread.
// Inputs: Callers serialize access with the job-state mutex.
// Outputs: Failed, cancelled, disabled, or superseded jobs never leave a consumable destination.
class OperationDestination {
  public:
    // Purpose: Start a job. Inputs: captured destination or empty path. Outputs: Drops stale completion state.
    void begin(std::filesystem::path destination) {
        requested_ = std::move(destination);
        pending_.clear();
    }

    // Purpose: Finish a job. Inputs: true only after every job step succeeds without cancellation.
    // Outputs: Publishes the captured destination once on success and clears the request in all cases.
    void complete(bool success) {
        pending_ = success ? std::exchange(requested_, {}) : std::filesystem::path{};
        requested_.clear();
    }

    // Purpose: Consume a completed destination. Inputs: None. Outputs: Returns its path exactly once.
    std::filesystem::path take() {
        return std::exchange(pending_, {});
    }

  private:
    std::filesystem::path requested_;
    std::filesystem::path pending_;
};

}  // namespace superzip::app
