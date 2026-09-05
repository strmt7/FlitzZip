#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace superzip::dictionary {

// Internal dictionary-codec building block; not an archive block kind or a production format change.
inline constexpr std::uint32_t kSegmentBytes = 65536U;
inline constexpr std::uint32_t kMinMatchBytes = 4U;
inline constexpr std::uint32_t kMaxMatchBytes = 8192U;
inline constexpr std::size_t kMaxBatchBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxWorkspaceBytes = 256U * 1024U * 1024U;
inline constexpr std::uint32_t kEncodedSegmentCapacity = kSegmentBytes + kSegmentBytes / 255U + 16U;

struct Effort {
    std::uint32_t max_candidates;
    std::uint32_t max_byte_comparisons;
};

struct Match {
    std::uint16_t distance = 0;
    std::uint16_t length = 0;
    std::uint16_t candidates_examined = 0;
    std::uint16_t bytes_compared = 0;

    // Purpose: Compare complete deterministic match records, including effort accounting.
    // Inputs: Another match record.
    // Outputs: Returns true when every field agrees.
    bool operator==(const Match&) const = default;
};

static_assert(sizeof(Match) == 8U);
static_assert(std::is_trivially_copyable_v<Match>);
static_assert(kSegmentBytes == (1U << 16U));
static_assert(kMaxBatchBytes / kSegmentBytes < (1U << 16U));

struct MatchBatch {
    std::vector<Match> matches;
    std::uint64_t device_workspace_bytes = 0;
    std::uint64_t h2d_bytes = 0;
    std::uint64_t d2h_bytes = 0;
    std::optional<double> index_ms;
    std::optional<double> search_ms;
    std::uint32_t primitive_version = 0;
    bool gpu_used = false;
};

// One independent LZ4-format block, without a frame or archive-container header.
struct EncodedSegment {
    std::vector<std::byte> payload;
    std::uint32_t input_bytes = 0;
};

// Bounded experimental output; not yet a public archive representation.
struct EncodedBatch {
    std::vector<EncodedSegment> segments;
    std::uint64_t device_workspace_bytes = 0;
    std::uint64_t h2d_bytes = 0;
    std::uint64_t d2h_bytes = 0;
    bool gpu_used = false;
};

// Verified concatenated segment bytes, with bounded decoder workspace and transfer evidence.
struct DecodedBatch {
    std::vector<std::byte> bytes;
    std::uint64_t device_workspace_bytes = 0;
    std::uint64_t h2d_bytes = 0;
    std::uint64_t d2h_bytes = 0;
    std::optional<double> decode_ms;
    bool gpu_used = false;
};

// Purpose: Map all nine compression efforts to strictly increasing, bounded dictionary-search work.
// Inputs: A compression level from 1 through 9.
// Outputs: Returns candidate and byte-comparison budgets; throws ArchiveError outside that range.
Effort effort_for_level(int level);

// Purpose: Distinguish valid HIP event durations from unavailable timing without inventing zero measurements.
// Inputs: Device-event elapsed milliseconds, including potentially invalid runtime output.
// Outputs: Returns finite nonnegative milliseconds unchanged, otherwise nullopt.
std::optional<double> validated_stage_milliseconds(double milliseconds);

// Purpose: Find deterministic earlier substring matches on HIP for a bounded dictionary-codec batch.
// Inputs: At most 4 MiB of borrowed immutable bytes and effort level 1..9; no references cross 64 KiB segments.
// Outputs: Returns one match per input byte and stage telemetry; throws on unavailable HIP or resource limits.
// Inputs shorter than four bytes need no search and return zero matches with gpu_used=false. No CPU search fallback.
MatchBatch find_matches(std::span<const std::byte> input, int level);

// Purpose: Encode independent dictionary blocks entirely on HIP without downloading the match table.
// Inputs: At most 4 MiB of immutable source and effort 1..9; each block covers at most 64 KiB.
// Outputs: Returns bounded LZ4-format block payloads and resource counts; throws if HIP is unavailable.
// Empty input produces no blocks and needs no GPU work. No frame or SUZIP metadata is emitted.
EncodedBatch encode_segments(std::span<const std::byte> input, int level);

// Purpose: Decode independent dictionary segments on HIP without CPU materialization or fallback.
// Inputs: At most 64 blocks, each declaring 1..65536 decoded bytes and a bounded nonempty payload.
// Outputs: Returns exact concatenated bytes only after every block validates; throws on invalid input or missing HIP.
// Empty input produces an empty GPU-free result. This is not yet a public archive-format reader.
DecodedBatch decode_segments(std::span<const EncodedSegment> segments);

}  // namespace superzip::dictionary
