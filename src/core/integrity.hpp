#pragma once

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
    std::string hex_digest;
};

// Purpose: Optionally compute a file integrity hash.
// Inputs: `path` is the existing file to hash and `mode` selects disabled or SHA-256 hashing.
// Outputs: Returns attempted/algorithm/digest data; throws `ArchiveError` when hashing is enabled and the file/provider cannot be opened.
IntegrityResult hash_file(const std::filesystem::path& path, IntegrityMode mode);

}  // namespace superzip
