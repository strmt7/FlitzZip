#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a single-file UUencoded stream with bounded text output.
// Inputs: `source_file` is an existing regular file, `output_archive` is the destination `.uue`/`.uu`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on invalid source, unsafe header name, or writer failure.
OperationStats compress_uue_file(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Create a single-file UUencoded stream from exactly one source path.
// Inputs: `sources` must contain one existing regular file, `output_archive` is the destination `.uue`/`.uu`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws when the source set is empty, has multiple paths, or is not a regular file.
OperationStats compress_uue(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract one UUencoded member with path-safe publication.
// Inputs: `archive_path` is the `.uue`/`.uu` stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed UUE data, unsafe header path, refused overwrite, or verified-file publication failures.
OperationStats extract_uue_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
