#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace superzip {

enum class OperationKind {
    Idle,
    Compress,
    Extract,
    Verify,
};

struct ProgressSnapshot {
    OperationKind operation = OperationKind::Idle;
    std::uint64_t total_bytes = 0;
    std::uint64_t processed_bytes = 0;
    std::uint64_t total_entries = 0;
    std::uint64_t completed_entries = 0;
    double throughput_bytes_per_second = 0.0;
    bool cancel_requested = false;
    std::string current_entry;
    std::string note;
};

class ProgressState {
public:
    // Purpose: Reset progress state for a new operation.
    // Inputs: `operation` identifies the workflow, `total_bytes` is the expected byte count, and `total_entries` is the expected entry count.
    // Outputs: Mutates internal counters and timestamps; does not throw unless string allocation fails.
    void start(OperationKind operation, std::uint64_t total_bytes, std::uint64_t total_entries);

    // Purpose: Update the current archive entry label.
    // Inputs: `entry` is a display/progress label copied into the state.
    // Outputs: Mutates current-entry state for future snapshots.
    void set_current(std::string entry);

    // Purpose: Add processed byte count to the current operation.
    // Inputs: `bytes` is the incremental byte count just completed.
    // Outputs: Mutates processed byte state for future snapshots.
    void add_bytes(std::uint64_t bytes);

    // Purpose: Mark one archive entry complete.
    // Inputs: None.
    // Outputs: Increments completed-entry state for future snapshots.
    void finish_entry();

    // Purpose: Attach an informational note to progress.
    // Inputs: `note` is copied into state and should not contain secrets.
    // Outputs: Mutates note state for future snapshots.
    void set_note(std::string note);

    // Purpose: Request cancellation from a producer or UI.
    // Inputs: None.
    // Outputs: Sets the cancellation flag read by callers that poll progress.
    void request_cancel();

    // Purpose: Query whether cancellation has been requested.
    // Inputs: None.
    // Outputs: Returns the current cancellation flag.
    [[nodiscard]] bool cancelled() const;

    // Purpose: Capture a thread-safe immutable progress snapshot.
    // Inputs: None.
    // Outputs: Returns counters, current entry, note, cancellation flag, and measured throughput.
    [[nodiscard]] ProgressSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    OperationKind operation_ = OperationKind::Idle;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t processed_bytes_ = 0;
    std::uint64_t total_entries_ = 0;
    std::uint64_t completed_entries_ = 0;
    bool cancel_requested_ = false;
    std::string current_entry_;
    std::string note_;
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
};

using ProgressCallback = std::function<void(const ProgressSnapshot&)>;

// Purpose: Deliver a progress snapshot when a callback is present.
// Inputs: `progress` is the state to snapshot and `callback` is optional.
// Outputs: Invokes `callback` synchronously with a snapshot; no-op when callback is empty.
void publish_progress(const ProgressState& progress, const ProgressCallback& callback);

}  // namespace superzip
