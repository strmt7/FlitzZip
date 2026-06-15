#pragma once

#include <filesystem>

namespace superzip {

struct ReservedFilePublishTarget {
    std::filesystem::path directory;
    std::filesystem::path file;
};

// Purpose: Reserve a private temporary file target beside a final output file.
// Inputs: `target` is the final file path that will later receive verified bytes.
// Outputs: Returns a same-directory temporary directory and payload file path, or throws on reservation failure.
ReservedFilePublishTarget reserve_file_publish_target(const std::filesystem::path& target);

// Purpose: Remove a reserved temporary publication target after success or failure.
// Inputs: `temporary` is the reserved temporary directory and payload path.
// Outputs: Best-effort removal of only the known payload and its immediate temporary directory.
void cleanup_file_publish_target(const ReservedFilePublishTarget& temporary);

// Purpose: Publish a fully verified temporary file into its final path.
// Inputs: `temporary` is the verified payload file, `target` is the final path, and `overwrite` controls replacement.
// Outputs: Atomically moves or links the file into place, or throws without deleting the caller-owned temporary file.
void commit_verified_file(
    const std::filesystem::path& temporary,
    const std::filesystem::path& target,
    bool overwrite);

}  // namespace superzip
