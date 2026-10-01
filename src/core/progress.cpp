#include "core/progress.hpp"

namespace superzip {

// Purpose: Reset the synchronized state and clock for a new operation.
// Inputs: operation identifies the job; total_bytes and total_entries are expected, nonnegative totals.
// Outputs: Clears prior counters, labels and cancellation; existing snapshots remain independent values.
void ProgressState::start(OperationKind operation, std::uint64_t total_bytes, std::uint64_t total_entries) {
    std::lock_guard lock(mutex_);
    operation_ = operation;
    total_bytes_ = total_bytes;
    processed_bytes_ = 0;
    total_entries_ = total_entries;
    completed_entries_ = 0;
    cancel_requested_ = false;
    current_entry_.clear();
    note_.clear();
    started_at_ = std::chrono::steady_clock::now();
}

// Purpose: Replace the displayed entry under the state lock.
// Inputs: entry is a non-secret label transferred by value; no reference is retained.
// Outputs: Updates future snapshots; allocation or synchronization errors may propagate.
void ProgressState::set_current(std::string entry) {
    std::lock_guard lock(mutex_);
    current_entry_ = std::move(entry);
}

// Purpose: Accumulate completed work under the state lock.
// Inputs: bytes is an incremental count; producers must keep the cumulative count representable in uint64_t.
// Outputs: Updates processed bytes for subsequent snapshots without clamping to the estimated total.
void ProgressState::add_bytes(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    processed_bytes_ += bytes;
}

// Purpose: Count one completed entry under the state lock.
// Inputs: No arguments; producers must keep the cumulative count representable in uint64_t.
// Outputs: Increments completed entries without changing the expected total.
void ProgressState::finish_entry() {
    std::lock_guard lock(mutex_);
    ++completed_entries_;
}

// Purpose: Replace the operation note under the state lock.
// Inputs: note is a non-secret display string transferred by value.
// Outputs: Updates future snapshots; no caller storage is retained.
void ProgressState::set_note(std::string note) {
    std::lock_guard lock(mutex_);
    note_ = std::move(note);
}

// Purpose: Publish a cooperative cancellation request under the state lock.
// Inputs: No arguments; running producers are responsible for polling cancelled().
// Outputs: Sets the flag until start() resets it; does not interrupt workers directly.
void ProgressState::request_cancel() {
    std::lock_guard lock(mutex_);
    cancel_requested_ = true;
}

// Purpose: Read the synchronized cooperative cancellation flag.
// Inputs: No arguments; concurrent producers and UI callers are supported.
// Outputs: Returns the flag as a value without changing the operation.
bool ProgressState::cancelled() const {
    std::lock_guard lock(mutex_);
    return cancel_requested_;
}

// Purpose: Copy a consistent progress view and compute elapsed-time average throughput.
// Inputs: No arguments; the steady clock and all counters are sampled under one lock.
// Outputs: Returns owned labels and byte/entry counts; copying labels can throw allocation errors.
ProgressSnapshot ProgressState::snapshot() const {
    std::lock_guard lock(mutex_);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at_).count();
    ProgressSnapshot snapshot;
    snapshot.operation = operation_;
    snapshot.total_bytes = total_bytes_;
    snapshot.processed_bytes = processed_bytes_;
    snapshot.total_entries = total_entries_;
    snapshot.completed_entries = completed_entries_;
    snapshot.cancel_requested = cancel_requested_;
    snapshot.current_entry = current_entry_;
    snapshot.note = note_;
    snapshot.throughput_bytes_per_second = elapsed > 0.0 ? static_cast<double>(processed_bytes_) / elapsed : 0.0;
    return snapshot;
}

// Purpose: Deliver a detached progress snapshot without holding the producer's state lock during callbacks.
// Inputs: progress is borrowed for this call and callback is optional and synchronous.
// Outputs: Invokes a present callback once; propagates snapshot/callback exceptions and retains neither input.
void publish_progress(const ProgressState& progress, const ProgressCallback& callback) {
    if (callback) {
        callback(progress.snapshot());
    }
}

}  // namespace superzip
