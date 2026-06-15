#pragma once

#include <filesystem>
#include <string>

namespace superzip {

// Purpose: Validate and normalize an archive entry name without joining it to the host filesystem.
// Inputs: `archive_path` is untrusted metadata from an archive.
// Outputs: Returns a slash-separated relative path key; throws `SecurityError` for traversal, absolute paths, reserved names, or invalid Windows path forms.
std::string normalize_archive_path_key(const std::string& archive_path);

// Purpose: Resolve an archive entry name safely below a destination root.
// Inputs: `destination_root` is the extraction root and `archive_path` is untrusted metadata from an archive.
// Outputs: Returns a normalized destination path; throws `SecurityError` for traversal, absolute paths, reserved names, or invalid Windows path forms.
std::filesystem::path safe_join_archive_path(
    const std::filesystem::path& destination_root,
    const std::string& archive_path);

// Purpose: Convert a relative filesystem path into a portable archive entry name.
// Inputs: `relative_path` must be relative to the selected source root.
// Outputs: Returns a slash-separated entry name; throws `SecurityError` when the normalized name would be unsafe.
std::string normalize_entry_name(const std::filesystem::path& relative_path);

}  // namespace superzip
