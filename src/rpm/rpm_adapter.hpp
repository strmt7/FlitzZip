#pragma once

#include "core/archive.hpp"
#include "core/archive_name_encoding.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract the CPIO payload from an RPM package with SuperZip path-safety checks.
// Inputs: `archive_path` is an RPM package, `destination` is the extraction root, `overwrite` allows existing targets
// only when true, and `progress_callback` receives CPIO extraction progress snapshots. Outputs: Returns operation
// statistics; throws on malformed RPM metadata, unsupported payload compression, unsafe CPIO paths, refused overwrite,
// or verified-file publication failures.
OperationStats extract_rpm(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback = {});

// Purpose: Extract an RPM CPIO payload using explicitly encoded member names.
// Inputs: Archive, destination, overwrite policy, progress callback, and the selected name encoding.
// Outputs: Returns extraction statistics or throws on invalid encoding, metadata, or payload.
OperationStats extract_rpm(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback, ArchivePathEncoding encoding);

}  // namespace superzip
