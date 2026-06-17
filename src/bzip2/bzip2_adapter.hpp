#pragma once

#include "core/archive.hpp"

#include <filesystem>
#include <vector>

namespace superzip {

// Purpose: Create a single-file Bzip2 `.bz2` stream with bounded in-process libbzip2.
// Inputs: `source_file` is an existing regular file, `output_archive` is the destination `.bz2`, `compression_level` is
// a libbzip2 1-9 block-size level, and `progress_callback` receives synchronous progress snapshots. Outputs: Returns
// operation statistics; throws `ArchiveError`/`SecurityError` on invalid source, unsafe target, or writer failure.
OperationStats compress_bzip2_file(const std::filesystem::path& source_file,
                                   const std::filesystem::path& output_archive,
                                   int compression_level = kDefaultCompressionLevel,
                                   const ProgressCallback& progress_callback = {});

// Purpose: Create a single-file Bzip2 `.bz2` stream from exactly one source path.
// Inputs: `sources` must contain one existing regular file, `output_archive` is the destination `.bz2`,
// `compression_level` is a libbzip2 1-9 block-size level, and `progress_callback` receives synchronous progress
// snapshots. Outputs: Returns operation statistics; throws when the source set is empty, has multiple paths, or is not
// a regular file.
OperationStats compress_bzip2(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& output_archive,
                              int compression_level = kDefaultCompressionLevel,
                              const ProgressCallback& progress_callback = {});

// Purpose: Extract a single-member Bzip2 `.bz2` stream with path-safe publication.
// Inputs: `archive_path` is the `.bz2` stream, `destination` is the extraction root, `overwrite` controls replacement,
// and `progress_callback` receives synchronous progress snapshots. Outputs: Returns operation statistics; throws on
// malformed Bzip2 data, unsafe output name, refused overwrite, or verified-file publication failures.
OperationStats extract_bzip2_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                  bool overwrite, const ProgressCallback& progress_callback = {});

}  // namespace superzip
