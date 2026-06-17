#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a single-file Gzip stream with bounded in-process raw deflate.
// Inputs: `source_file` is an existing regular file, `output_archive` is the destination `.gz`, `compression_level` is
// a miniz 1-9 level, and `progress_callback` receives synchronous progress snapshots. Outputs: Returns operation
// statistics; throws `ArchiveError`/`SecurityError` on invalid source, unsafe target, or Gzip writer failure.
OperationStats compress_gzip_file(const std::filesystem::path& source_file, const std::filesystem::path& output_archive,
                                  int compression_level = kDefaultCompressionLevel,
                                  const ProgressCallback& progress_callback = {});

// Purpose: Create a single-file Gzip stream from exactly one source path.
// Inputs: `sources` must contain one existing regular file, `output_archive` is the destination `.gz`,
// `compression_level` is a miniz 1-9 level, and `progress_callback` receives synchronous progress snapshots. Outputs:
// Returns operation statistics; throws when the source set is empty, has multiple paths, or is not a regular file.
OperationStats compress_gzip(const std::vector<std::filesystem::path>& sources,
                             const std::filesystem::path& output_archive,
                             int compression_level = kDefaultCompressionLevel,
                             const ProgressCallback& progress_callback = {});

// Purpose: Extract a single-member Gzip stream with CRC32/ISIZE verification before final publication.
// Inputs: `archive_path` is the `.gz` stream, `destination` is the extraction root, `overwrite` controls replacement,
// and `progress_callback` receives synchronous progress snapshots. Outputs: Returns operation statistics; throws on
// malformed Gzip data, checksum mismatch, unsafe output name, or refused overwrite.
OperationStats extract_gzip_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                 bool overwrite, const ProgressCallback& progress_callback = {});

}  // namespace superzip
