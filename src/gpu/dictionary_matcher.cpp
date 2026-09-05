#include "gpu/dictionary_matcher.hpp"

#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"

#include <cmath>

namespace superzip::dictionary {

#if SUPERZIP_ENABLE_HIP
// Purpose: Execute the bounded HIP dictionary implementation after host-side admission checks.
// Inputs: A validated batch of at least four bytes and a validated effort budget.
// Outputs: Returns deterministic match records and measured HIP stage/resource statistics.
MatchBatch find_matches_hip(std::span<const std::byte> input, const Effort& effort);

// Purpose: Encode validated input using GPU-resident dictionary matches.
// Inputs: A nonempty bounded batch and validated effort.
// Outputs: Returns encoded blocks, or throws on HIP/resource failure.
EncodedBatch encode_segments_hip(std::span<const std::byte> input, const Effort& effort);
#endif

// Purpose: Resolve increasing dictionary search budgets without unchecked shift counts.
// Inputs: Requested compression level.
// Outputs: Returns bounded effort or throws ArchiveError before GPU work for an invalid level.
Effort effort_for_level(int level) {
    if (level < 1 || level > 9) {
        throw ArchiveError("dictionary compression level must be between 1 and 9");
    }
    const auto depth = 1U << static_cast<unsigned int>(level - 1);
    return {.max_candidates = depth, .max_byte_comparisons = 32U * depth};
}

// Purpose: Reject impossible device-event durations independently of successful codec execution.
// Inputs: Runtime-supplied elapsed milliseconds.
// Outputs: Returns a valid measurement or explicit absence for negative and non-finite values.
std::optional<double> validated_stage_milliseconds(double milliseconds) {
    if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
        return std::nullopt;
    }
    return milliseconds;
}

// Purpose: Admit bounded dictionary searches and enforce the HIP-only computation boundary.
// Inputs: Borrowed source bytes and requested effort.
// Outputs: Returns literal-only short-input records or HIP matches; throws on invalid limits or unavailable HIP.
MatchBatch find_matches(std::span<const std::byte> input, int level) {
    const auto effort = effort_for_level(level);
    if (input.size() > kMaxBatchBytes) {
        throw ArchiveError("dictionary batch exceeds the bounded GPU workspace input limit");
    }
    if (input.size() < kMinMatchBytes) {
        MatchBatch result;
        result.matches.resize(input.size());
        return result;
    }
#if SUPERZIP_ENABLE_HIP
    const auto info = query_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    return find_matches_hip(input, effort);
#else
    (void)effort;
    throw GpuError("AMD HIP dictionary search is not compiled into this build");
#endif
}

// Purpose: Validate dictionary encoding requests and enforce required-HIP execution.
// Inputs: Borrowed bytes and requested effort level.
// Outputs: Returns empty output for empty input or GPU-encoded blocks; rejects invalid limits and missing HIP.
EncodedBatch encode_segments(std::span<const std::byte> input, int level) {
    const auto effort = effort_for_level(level);
    if (input.size() > kMaxBatchBytes) {
        throw ArchiveError("dictionary batch exceeds the bounded GPU workspace input limit");
    }
    if (input.empty()) {
        return {};
    }
#if SUPERZIP_ENABLE_HIP
    const auto info = query_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    return encode_segments_hip(input, effort);
#else
    (void)effort;
    throw GpuError("AMD HIP dictionary encoding is not compiled into this build");
#endif
}

}  // namespace superzip::dictionary
