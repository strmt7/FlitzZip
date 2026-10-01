#pragma once

#include "core/path_text.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <system_error>

namespace superzip {

// Purpose: Query a file byte length without relying on the host ANSI code page for diagnostics.
// Inputs: `path` names a file; callers remain responsible for source locking and path validation.
// Outputs: Returns its representable byte length or throws `ArchiveError`; does not lock or open the file.
inline std::uint64_t regular_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read file size: " + path_diagnostic_utf8(path));
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("file size exceeds SuperZip limits: " + path_diagnostic_utf8(path));
    }
    return static_cast<std::uint64_t>(size);
}

}  // namespace superzip
