#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace superzip {

struct ManifestEntry {
    std::filesystem::path source_path;
    std::string archive_path;
    bool directory = false;
    std::uint64_t size = 0;
};

struct Manifest {
    std::vector<ManifestEntry> entries;
    std::uint64_t total_file_bytes = 0;
    std::uint64_t file_count = 0;
};

// Purpose: Convert filesystem sources into deterministic archive entries.
// Inputs: `sources` are user-selected files or directories; symlinks and unsupported file types are rejected.
// Outputs: Returns sorted manifest metadata with normalized archive names; throws `ArchiveError` or `SecurityError` for invalid sources.
Manifest build_manifest(const std::vector<std::filesystem::path>& sources);

}  // namespace superzip
