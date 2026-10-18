#pragma once

#include "core/progress.hpp"

#include <filesystem>
#include <utility>

namespace flitzzip::app {

// Purpose: Preserve Ask-before-overwriting when an extraction destination cannot be proven empty.
// Inputs: The captured extraction root; filesystem errors do not authorize unattended continuation.
// Outputs: False only for an absent root or a successfully enumerated empty directory.
inline bool extraction_destination_needs_confirmation(const std::filesystem::path& destination) {
    std::error_code error;
    if (!std::filesystem::exists(destination, error) && !error) {
        return false;
    }
    if (!error && std::filesystem::is_directory(destination, error) && !error) {
        const std::filesystem::directory_iterator iterator(destination, std::filesystem::directory_options::none,
                                                           error);
        if (!error && iterator == std::filesystem::directory_iterator{}) {
            return false;
        }
    }
    return true;
}

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

}  // namespace flitzzip::app
