#pragma once

#include "core/stream_byte_consumer.hpp"

#include <stdexcept>
#include <functional>
#include <utility>

namespace flitzzip {

// Optional synchronous checkpoints retain the observer's original exception.
inline void invoke_archive_checkpoint(const ProgressCheckpoint& checkpoint) {
    if (checkpoint) {
        checkpoint();
    }
}

// Own the observer and immutable phase status until the synchronous phase ends.
inline ProgressCheckpoint make_archive_phase_checkpoint(const ProgressCallback& callback, ProgressSnapshot snapshot) {
    if (!callback) {
        return {};
    }
    return [callback, snapshot = std::move(snapshot)] { callback(snapshot); };
}

inline ProgressCheckpoint make_archive_validation_checkpoint(const ProgressCallback& callback) {
    return make_archive_phase_checkpoint(callback, {.operation = OperationKind::Extract, .note = "Validating"});
}

// Borrow the exact stateful observer for a synchronous scan. The caller must
// outlive every checkpoint invocation; reference_wrapper requires an explicit
// lvalue borrow. A forwarding lambda prevents the owning phase helper from
// copying the original std::function target instead of its reference wrapper.
inline ProgressCheckpoint
make_borrowed_archive_validation_checkpoint(std::reference_wrapper<const ProgressCallback> callback) {
    if (!callback.get()) {
        return {};
    }
    return make_archive_validation_checkpoint(
        [callback](const ProgressSnapshot& snapshot) { callback.get()(snapshot); });
}

inline ProgressCheckpoint make_archive_finishing_checkpoint(const ProgressCallback& callback, std::uint64_t bytes,
                                                            std::uint64_t entries) {
    return make_archive_phase_checkpoint(callback, {.operation = OperationKind::Extract,
                                                    .total_bytes = bytes,
                                                    .processed_bytes = bytes,
                                                    .total_entries = entries,
                                                    .completed_entries = entries,
                                                    .note = "Finishing"});
}

// Observed archive decoders expose owned windows. Drain at bounded checkpoints
// before the unchanged finish validation, never silently omitting cancellation
// or final checksum/termination checks. Reader owns all storage through this call.
template <class Reader> void finish_archive_stream(Reader& input, const ProgressCheckpoint& checkpoint) {
    if (checkpoint) {
        auto* decoded = dynamic_cast<DecodedInputBuffer*>(input.rdbuf());
        if (decoded == nullptr) {
            throw std::logic_error("checkpointed archive decoder has no owned window interface");
        }
        for (;;) {
            checkpoint();
            if (decoded->consume(detail::kStreamConsumerWindowBytes, [](std::span<const char>) {}) == 0U) {
                break;
            }
        }
    }
    input.finish();
}

}  // namespace flitzzip
