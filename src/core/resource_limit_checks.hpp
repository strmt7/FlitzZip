#pragma once

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace flitzzip {

// Purpose: Add extracted-output bytes while enforcing FlitzZip's decoded output policy.
// Inputs: `total` is the current decoded byte count, `bytes` is the next decoded span, and `context` labels errors.
// Outputs: Returns the updated total; throws `ArchiveError` on overflow or policy-limit excess.
inline std::uint64_t checked_add_extracted_output_bytes(std::uint64_t total, std::uint64_t bytes,
                                                        std::string_view context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    const auto updated = total + bytes;
    if (updated > kMaxExtractedOutputBytes) {
        throw ArchiveError(std::string(context) + " exceeds FlitzZip resource limit");
    }
    return updated;
}

// Purpose: Add retained archive-path bytes while enforcing the shared metadata policy.
// Inputs: `total` is the current retained byte count, `bytes` is the next allocation/copy, and `context` labels errors.
// Outputs: Returns the updated total; throws `ArchiveError` on overflow or aggregate metadata-limit excess.
inline std::uint64_t checked_add_archive_path_metadata_bytes(std::uint64_t total, std::uint64_t bytes,
                                                             std::string_view context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " metadata byte count overflows");
    }
    const auto updated = total + bytes;
    if (updated > kMaxArchivePathMetadataBytes) {
        throw ArchiveError(std::string(context) + " exceeds FlitzZip path metadata limit");
    }
    return updated;
}

// Purpose: Enforce the native archive-index entry-count limit before vector reservation or serialization.
// Inputs: `entries` is the declared or materialized count and `diagnostic` identifies the caller boundary.
// Outputs: Returns normally through the exact limit; throws `ArchiveError` for the first excessive count.
inline void validate_archive_index_entry_count(std::uint64_t entries, const char* diagnostic) {
    if (entries > kMaxArchiveEntries) {
        throw ArchiveError(diagnostic);
    }
}

// Purpose: Add one entry's block count while enforcing both native archive-index limits.
// Inputs: `total` is the admitted archive count, `blocks` is the next entry count, and diagnostics identify the
// caller's per-entry and aggregate failures.
// Outputs: Returns the updated count; throws `ArchiveError` before subtraction underflow or policy-limit excess.
inline std::uint64_t checked_add_archive_index_blocks(std::uint64_t total, std::uint64_t blocks,
                                                      const char* per_entry_diagnostic, const char* total_diagnostic) {
    if (blocks > kMaxBlocksPerEntry) {
        throw ArchiveError(per_entry_diagnostic);
    }
    if (total > kMaxArchiveBlocks || blocks > kMaxArchiveBlocks - total) {
        throw ArchiveError(total_diagnostic);
    }
    return total + blocks;
}

}  // namespace flitzzip
