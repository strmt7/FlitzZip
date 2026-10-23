#pragma once

#include "core/archive_name_encoding.hpp"
#include "core/archive.hpp"

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace flitzzip::detail {

// Purpose: Own one validated TAR entry independently of its input stream.
// Inputs: Decoded path, name encoding, entry kind and bounded wire payload geometry.
// Outputs: Describes a regular file or directory without retaining borrowed storage.
struct TarEntryMetadata {
    std::string path;
    ArchivePathEncoding encoding = ArchivePathEncoding::HostCodePage;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t payload_offset = 0;
};

// Purpose: Retain the complete TAR publication plan before extraction begins.
// Inputs: Validated entries accumulated within output and metadata limits.
// Outputs: Owns entries, aggregate file bytes and the retained pathname budget.
struct TarScanResult {
    std::vector<TarEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
    std::uint64_t path_metadata_bytes = 0;
};

// Purpose: Validate TAR metadata through the same reader used by filesystem and filtered extraction.
// Inputs: A borrowed stream beginning at zero; seekable requires its independently established archive_size.
// Outputs: Owns the publication plan or throws on invalid metadata, incomplete input or a read failure.
TarScanResult scan_tar_stream(std::istream& input, bool seekable,
                              std::optional<std::uint64_t> archive_size = std::nullopt);

// Purpose: Restore a filtered TAR stream while independently checking every record against its admitted plan.
// Inputs: Borrowed input starts at zero; scanned came from scan_tar_stream; destination and overwrite select output.
// Outputs: Publishes verified files, reports synchronous progress or throws on changed metadata/read failure.
void extract_validated_tar_stream(std::istream& input, const TarScanResult& scanned,
                                  const std::filesystem::path& destination, bool overwrite,
                                  const ProgressCallback& progress_callback);

}  // namespace flitzzip::detail
