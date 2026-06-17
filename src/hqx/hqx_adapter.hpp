#pragma once

#include "core/archive.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract the data fork from one BinHex 4.0 `.hqx` stream with path-safe publication.
// Inputs: `archive_path` is the `.hqx` stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed HQX text, CRC mismatch, unsafe header path, refused overwrite, or verified-file publication failure.
OperationStats extract_hqx_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
