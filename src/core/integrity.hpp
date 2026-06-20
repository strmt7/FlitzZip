#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace superzip {

enum class IntegrityMode {
    Disabled,
    Sha256,
};

struct IntegrityResult {
    bool attempted = false;
    std::string algorithm;
    std::string target;
    std::string hex_digest;
    std::uint64_t bytes_hashed = 0;
    std::uint64_t files_hashed = 0;
    std::uint64_t directories_hashed = 0;
};

// Purpose: Optionally compute a file integrity hash.
// Inputs: `path` is the existing file to hash and `mode` selects disabled or SHA-256 hashing.
// Outputs: Returns attempted/algorithm/digest data; throws `ArchiveError` when hashing is enabled and the file/provider
// cannot be opened.
IntegrityResult hash_file(const std::filesystem::path& path, IntegrityMode mode);

// Purpose: Optionally compute a file or deterministic directory-tree integrity hash.
// Inputs: `path` is an existing regular file or directory and `mode` selects disabled or SHA-256 hashing.
// Outputs: Returns attempted/algorithm/target/digest/count data; throws `ArchiveError` for missing or unsupported
// targets.
IntegrityResult hash_path(const std::filesystem::path& path, IntegrityMode mode);

}  // namespace superzip
