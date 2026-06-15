#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create an uncompressed POSIX TAR archive with SuperZip source-path validation.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination TAR path, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, or TAR writer failures.
OperationStats compress_tar(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract an uncompressed TAR archive with SuperZip path-safety checks.
// Inputs: `archive_path` is the TAR file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed TAR data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_tar(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
