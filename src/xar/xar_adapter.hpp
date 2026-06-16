#pragma once

#include "core/archive.hpp"
#include "core/progress.hpp"

#include <filesystem>

namespace superzip {

// Purpose: Extract files from a XAR archive with SuperZip path-safety checks.
// Inputs: `archive_path` is a XAR archive, `destination` is the extraction root, `overwrite` allows existing targets only when true, and `progress_callback` receives extraction progress snapshots.
// Outputs: Returns operation statistics; throws on malformed XAR headers/TOC XML, nonzero TOC checksum modes, unsafe paths, unsupported entry types/encodings, resource-limit violations, refused overwrite, decode failure, or verified-file publication failure.
OperationStats extract_xar(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback = {});

}  // namespace superzip
