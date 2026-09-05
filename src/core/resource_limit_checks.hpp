#pragma once

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace superzip {

// Purpose: Add extracted-output bytes while enforcing SuperZip's decoded output policy.
// Inputs: `total` is the current decoded byte count, `bytes` is the next decoded span, and `context` labels errors.
// Outputs: Returns the updated total; throws `ArchiveError` on overflow or policy-limit excess.
inline std::uint64_t checked_add_extracted_output_bytes(std::uint64_t total, std::uint64_t bytes,
                                                        std::string_view context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    const auto updated = total + bytes;
    if (updated > kMaxExtractedOutputBytes) {
        throw ArchiveError(std::string(context) + " exceeds SuperZip resource limit");
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
        throw ArchiveError(std::string(context) + " exceeds SuperZip path metadata limit");
    }
    return updated;
}

}  // namespace superzip
