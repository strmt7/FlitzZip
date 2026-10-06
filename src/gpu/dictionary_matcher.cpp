#include "gpu/dictionary_matcher.hpp"

#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"

#include <array>
#include <cmath>

namespace superzip::dictionary {

#if SUPERZIP_ENABLE_HIP
// Purpose: Execute the bounded HIP dictionary implementation after host-side admission checks.
// Inputs: A validated batch of at least four bytes and a validated effort budget.
// Outputs: Returns deterministic match records and measured HIP stage/resource statistics.
MatchBatch find_matches_hip(std::span<const std::byte> input, const Effort& effort);

// Purpose: Encode validated input using GPU-resident dictionary matches.
// Inputs: A nonempty bounded batch, validated effort, and explicit search strategy.
// Outputs: Returns encoded blocks, or throws on HIP/resource failure.
EncodedBatch encode_segments_hip(std::span<const std::byte> input, const Effort& effort);

// Purpose: Execute admitted deeper search and minimum-byte segment parsing on HIP.
// Inputs: Nonempty bounded source, cancellation checkpoint and admitted group limits.
// Outputs: Returns GPU-encoded segments; propagates resource/runtime/cancellation failures.
EncodedBatch encode_neutron_segments_hip(std::span<const std::byte> input, const EncodeCheckpoint& checkpoint,
                                         std::span<const NeutronParseBudget> budgets);

// Purpose: Execute HIP decoding after host-side segment-size admission.
// Inputs: Nonempty bounded segments and their exact total decoded byte count.
// Outputs: Returns complete decoded bytes or throws before exposing partial output.
DecodedBatch decode_segments_hip(std::span<const EncodedSegment> segments, std::size_t decoded_bytes);
#endif

// Purpose: Resolve increasing dictionary search budgets without unchecked shift counts.
// Inputs: Requested compression level.
// Outputs: Returns bounded effort or throws ArchiveError before GPU work for an invalid level.
Effort effort_for_level(int level) {
    if (level < 1 || level > 9) {
        throw ArchiveError("dictionary compression level must be between 1 and 9");
    }
    const auto depth = 1U << static_cast<unsigned int>(level - 1);
    constexpr std::array<std::uint32_t, 9> comparison_budgets{192U,  256U,  384U,  512U,  768U,
                                                              1536U, 3072U, 6144U, 12288U};
    return {.max_candidates = depth, .max_byte_comparisons = comparison_budgets[static_cast<std::size_t>(level - 1)]};
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

// Purpose: Admit complete parse groups before checkpoints, empty returns or GPU admission.
// Inputs: Exact source size and optional bounded group byte limits.
// Outputs: Accepts complete canonical groups or throws without allocating or dispatching device work.
void validate_neutron_budgets(std::size_t input_bytes, std::span<const NeutronParseBudget> budgets) {
    if (budgets.empty()) {
        return;
    }
    if (input_bytes == 0U || input_bytes > kMaxNeutronBatchBytes ||
        budgets.size() > kMaxNeutronBatchBytes / kSegmentBytes) {
        throw GpuError("Neutron parse budget extent is invalid");
    }
    std::size_t offset = 0U;
    for (std::size_t index = 0U; index < budgets.size(); ++index) {
        const auto& budget = budgets[index];
        if (budget.input_bytes == 0U || budget.input_bytes > input_bytes - offset ||
            budget.maximum_payload_bytes > budget.input_bytes ||
            (index + 1U < budgets.size() && budget.input_bytes % kSegmentBytes != 0U)) {
            throw GpuError("Neutron parse budgets do not describe canonical groups");
        }
        offset += budget.input_bytes;
    }
    if (offset != input_bytes) {
        throw GpuError("Neutron parse budgets do not cover their source");
    }
}

// Purpose: Admit bounded minimum-byte parsing without permitting CPU fallback or overlarge workspaces.
// Inputs: Immutable source, throwing cancellation checkpoint and optional complete group limits.
// Outputs: Returns empty input unchanged or verified GPU segments; throws on invalid size or unavailable HIP.
EncodedBatch encode_neutron_segments(std::span<const std::byte> input, const EncodeCheckpoint& checkpoint,
                                     std::span<const NeutronParseBudget> budgets) {
    if (input.size() > kMaxNeutronBatchBytes) {
        throw ArchiveError("Neutron dictionary batch exceeds its bounded input limit");
    }
    validate_neutron_budgets(input.size(), budgets);
    if (checkpoint) {
        checkpoint();
    }
    if (input.empty()) {
        return {};
    }
#if SUPERZIP_ENABLE_HIP
    const auto info = query_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    return encode_neutron_segments_hip(input, checkpoint, budgets);
#else
    throw GpuError("AMD HIP Neutron dictionary encoding is not compiled into this build");
#endif
}

// Purpose: Validate dictionary block extents before uploading encoded data or allocating GPU output.
// Inputs: Borrowed independent blocks whose size fields are not trusted.
// Outputs: Returns empty output or fully validated HIP decoding; rejects resource violations and unavailable HIP.
DecodedBatch decode_segments(std::span<const EncodedSegment> segments) {
    if (segments.size() > kMaxBatchBytes / kSegmentBytes) {
        throw ArchiveError("dictionary decode batch exceeds the segment limit");
    }
    std::size_t decoded_bytes = 0;
    for (const auto& segment : segments) {
        if (segment.input_bytes == 0U || segment.input_bytes > kSegmentBytes || segment.payload.empty() ||
            segment.payload.size() > kEncodedSegmentCapacity) {
            throw ArchiveError("dictionary decode segment has invalid byte extents");
        }
        decoded_bytes += segment.input_bytes;
    }
    if (segments.empty()) {
        return {};
    }
#if SUPERZIP_ENABLE_HIP
    const auto info = query_gpu_info();
    if (!info.available) {
        throw GpuError(info.status);
    }
    return decode_segments_hip(segments, decoded_bytes);
#else
    (void)decoded_bytes;
    throw GpuError("AMD HIP dictionary decoding is not compiled into this build");
#endif
}

}  // namespace superzip::dictionary
