#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace superzip {

struct CabEntryInfo {
    std::string path;
    std::uint64_t size = 0;
};

struct CabMetadata {
    std::vector<CabEntryInfo> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Normalize a CAB entry path before archive-wide path validation.
// Inputs: `raw_path` is untrusted CAB metadata and may contain backslashes.
// Outputs: Returns a slash-separated safe relative path key or throws `SecurityError`.
std::string normalize_cab_entry_path(std::string raw_path);

// Purpose: Parse and validate CAB metadata without decompressing file payloads.
// Inputs: `archive_path` is a CAB file candidate.
// Outputs: Returns validated file names/sizes; throws on malformed headers, spanning cabinets, unsafe paths, or resource-limit violations.
CabMetadata scan_cab_metadata(const std::filesystem::path& archive_path);

}  // namespace superzip
