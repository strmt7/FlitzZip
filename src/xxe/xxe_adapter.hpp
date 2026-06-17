#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a single-file XXEncoded stream with bounded text output.
// Inputs: `source_file` is an existing regular file, `output_archive` is the destination `.xxe`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws `ArchiveError`/`SecurityError` on invalid source, unsafe header name, or writer failure.
OperationStats compress_xxe_file(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Create a single-file XXEncoded stream from exactly one source path.
// Inputs: `sources` must contain one existing regular file, `output_archive` is the destination `.xxe`, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws when the source set is empty, has multiple paths, or is not a regular file.
OperationStats compress_xxe(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback = {});

// Purpose: Extract one XXEncoded member with path-safe publication.
// Inputs: `archive_path` is the `.xxe` stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed XXEncode data, unsafe header path, refused overwrite, or verified-file publication failures.
OperationStats extract_xxe_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
