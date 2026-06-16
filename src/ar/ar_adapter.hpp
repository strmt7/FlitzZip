#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a Unix AR archive from regular files in one or more source trees.
// Inputs: `sources` are existing filesystem roots, `output_archive` is the destination `.ar`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on source, path, or writer failures.
OperationStats compress_ar(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract a Unix AR archive with SuperZip path-safety checks.
// Inputs: `archive_path` is an `.ar` file, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed AR metadata, unsafe paths, refused overwrite, or verified-file publication failures.
OperationStats extract_ar(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
