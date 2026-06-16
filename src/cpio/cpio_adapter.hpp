#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a portable SVR4 new ASCII CPIO archive from files/directories.
// Inputs: `sources` are existing filesystem roots, `output_archive` is the destination `.cpio`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, or writer failures.
OperationStats compress_cpio(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a SVR4 new ASCII CPIO archive with SuperZip path-safety checks.
// Inputs: `archive_path` is a `.cpio` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed CPIO metadata, unsafe paths, special files, refused overwrite, or verified-file publication failures.
OperationStats extract_cpio(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
