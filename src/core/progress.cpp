#include "core/progress.hpp"

namespace superzip {

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

void ProgressState::set_current(std::string entry) {
    std::lock_guard lock(mutex_);
    current_entry_ = std::move(entry);
}

void ProgressState::add_bytes(std::uint64_t bytes) {
    std::lock_guard lock(mutex_);
    processed_bytes_ += bytes;
}

void ProgressState::finish_entry() {
    std::lock_guard lock(mutex_);
    ++completed_entries_;
}

void ProgressState::set_note(std::string note) {
    std::lock_guard lock(mutex_);
    note_ = std::move(note);
}

void ProgressState::request_cancel() {
    std::lock_guard lock(mutex_);
    cancel_requested_ = true;
}

bool ProgressState::cancelled() const {
    std::lock_guard lock(mutex_);
    return cancel_requested_;
}

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

void publish_progress(const ProgressState& progress, const ProgressCallback& callback) {
    if (callback) {
        callback(progress.snapshot());
    }
}

}  // namespace superzip
