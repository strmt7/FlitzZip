#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a standard ZIP archive for compatibility workflows.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination ZIP path, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, or ZIP writer failures.
OperationStats compress_zip(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a standard ZIP archive with SuperZip path-safety checks.
// Inputs: `archive_path` is the ZIP file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed ZIP data, unsafe entry paths, refused overwrite, or verified-file publication failures.
OperationStats extract_zip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
