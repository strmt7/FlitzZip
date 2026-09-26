#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a standard ZIP archive for compatibility workflows.
// Inputs: `sources` are existing files/directories, `output_archive` is the destination ZIP path, `compression_level`
// is a miniz 1-9 level, and `progress_callback` receives synchronous progress snapshots. `verify_after_write` requests
// bounded read-back before publication. Outputs: Returns statistics; source, path, writer, verification, or callback
// failures before publication preserve an existing destination.
OperationStats compress_zip(const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& output_archive,
                            int compression_level = kDefaultCompressionLevel,
                            const ProgressCallback& progress_callback = {}, bool verify_after_write = false);

// Purpose: Validate a ZIP archive without writing decompressed files.
// Inputs: `archive_path` is untrusted ZIP input; `progress_callback` observes entries and bounded decoded windows.
// Outputs: Returns verification statistics; throws on unsafe names, unsupported entries, malformed headers,
// decoded size/CRC mismatches, or callback cancellation. No extraction directory is created.
OperationStats verify_zip(const std::filesystem::path& archive_path, const ProgressCallback& progress_callback = {});

// Purpose: Extract a standard ZIP archive with SuperZip path-safety checks.
// Inputs: `archive_path` is the ZIP file, `destination` is the extraction root, `overwrite` allows existing targets
// only when true, and `progress_callback` receives synchronous progress snapshots. Outputs: Returns operation
// statistics; throws on malformed ZIP data, unsafe entry paths, refused overwrite, or verified-file publication
// failures.
OperationStats extract_zip(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback = {});

}  // namespace superzip
