#include "core/path_safety.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <cwctype>
#include <vector>

namespace superzip {
namespace {

// Purpose: Detect Windows-reserved device names in archive path components.
// Inputs: `name` is a single path component and may include an extension.
// Outputs: Returns true when the base name is reserved by Windows.
bool is_windows_reserved_name(std::string name) {
    const auto dot = name.find('.');
    if (dot != std::string::npos) {
        name.resize(dot);
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    static constexpr std::array reserved = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    return std::find(reserved.begin(), reserved.end(), name) != reserved.end();
}

// Purpose: Detect drive-rooted paths such as `C:`.
// Inputs: `path` is an untrusted archive path string.
// Outputs: Returns true when the string begins with an ASCII drive prefix.
bool has_drive_prefix(const std::string& path) {
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':';
}

// Purpose: Detect Win32-disallowed control characters in one archive path component.
// Inputs: `component` is an untrusted path component after separator splitting.
// Outputs: Returns true when any byte is below ASCII space, including embedded NUL.
bool contains_windows_control_character(const std::string& component) {
    return std::ranges::any_of(component, [](unsigned char ch) { return ch < 0x20U; });
}

// Purpose: Verify that a normalized child path remains under a normalized root.
// Inputs: `child` and `root` are filesystem paths after normalization.
// Outputs: Returns true when `root` is a path-component prefix of `child`.
bool starts_with_path(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto child_it = child.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *child_it != *root_it) {
            return false;
        }
    }
    return true;
}

// Purpose: Ensure the existing filesystem parent chain for an output path still resolves below the destination root.
// Inputs: `target` is the joined archive output path and `root` is the canonical extraction root.
// Outputs: Throws `SecurityError` when an existing parent component escapes through a reparse point or mount point.
void validate_existing_parent_containment(const std::filesystem::path& target, const std::filesystem::path& root) {
    std::error_code ec;
    const auto parent = std::filesystem::weakly_canonical(target.parent_path(), ec);
    if (ec) {
        throw SecurityError("archive entry parent path cannot be canonicalized: " + ec.message());
    }
    if (!starts_with_path(parent, root)) {
        throw SecurityError("archive entry parent resolves outside destination root: " + target.string());
    }
}

struct NormalizedArchivePath {
    std::string key;
    std::string original_path;
    bool directory = false;
};

// Purpose: Normalize an archive entry path into the key used for archive-wide collision checks.
// Inputs: `path` is untrusted archive metadata.
// Outputs: Returns a normalized relative key; on Windows the key is ASCII-folded for case-insensitive collision
// detection.
std::string normalize_archive_collision_key(const std::string& path) {
    auto key = normalize_archive_path_key(path);
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#endif
    return key;
}

// Purpose: Detect whether one normalized archive path is below another normalized archive path.
// Inputs: `child` and `parent` are slash-separated normalized archive path keys.
// Outputs: Returns true only for strict descendants such as `dir/file` under `dir`.
bool is_archive_path_descendant(const std::string& child, const std::string& parent) {
    return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 && child[parent.size()] == '/';
}

}  // namespace

// Purpose: Resolve one untrusted archive path below a destination root without following escaping parent reparses.
// Inputs: `destination_root` is the extraction root and `archive_path` is untrusted archive metadata.
// Outputs: Returns the validated target path or throws `SecurityError`/`ArchiveError` when containment cannot be
// proven.
std::filesystem::path safe_join_archive_path(const std::filesystem::path& destination_root,
                                             const std::string& archive_path) {
    const auto normalized_archive_path = normalize_archive_path_key(archive_path);
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(destination_root, ec);
    if (ec) {
        throw SecurityError("destination root cannot be canonicalized: " + ec.message());
    }
    const auto target = (root / std::filesystem::path(normalized_archive_path)).lexically_normal();
    if (!starts_with_path(target, root)) {
        throw SecurityError("archive entry resolves outside destination root: " + archive_path);
    }
    validate_existing_parent_containment(target, root);
    return target;
}

// Purpose: Normalize an untrusted archive entry path into SuperZip's slash-separated safe key.
// Inputs: `archive_path` is attacker-controlled archive metadata.
// Outputs: Returns a safe relative key, or throws `SecurityError` for traversal, absolute, reserved, or invalid forms.
std::string normalize_archive_path_key(const std::string& archive_path) {
    if (archive_path.empty()) {
        throw SecurityError("archive entry path is empty");
    }
    if (archive_path.starts_with('/') || archive_path.starts_with('\\') || archive_path.starts_with("//") ||
        archive_path.starts_with("\\\\") || has_drive_prefix(archive_path)) {
        throw SecurityError("archive entry uses an absolute or drive-rooted path: " + archive_path);
    }

    std::string normalized;
    std::string component;
    auto flush_component = [&]() {
        if (component.empty() || component == ".") {
            component.clear();
            return;
        }
        if (component == "..") {
            throw SecurityError("archive entry attempts path traversal: " + archive_path);
        }
        if (contains_windows_control_character(component)) {
            throw SecurityError("archive entry component contains a Windows control character");
        }
        if (component.back() == ' ' || component.back() == '.') {
            throw SecurityError("archive entry component has unsafe trailing characters: " + component);
        }
        if (component.find_first_of("<>:\"|?*") != std::string::npos) {
            throw SecurityError("archive entry component contains Windows-invalid characters: " + component);
        }
        if (is_windows_reserved_name(component)) {
            throw SecurityError("archive entry component uses a reserved Windows name: " + component);
        }
        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized.append(component);
        component.clear();
    };

    for (const char ch : archive_path) {
        if (ch == '/' || ch == '\\') {
            flush_component();
        } else {
            component.push_back(ch);
        }
    }
    flush_component();

    if (normalized.empty()) {
        throw SecurityError("archive entry path normalizes to empty");
    }
    return normalized;
}

// Purpose: Validate archive-wide normalized path uniqueness and file/directory conflicts.
// Inputs: `entries` contains every archive path and whether it represents a directory.
// Outputs: Returns normally for a safe set or throws `SecurityError` for duplicate/conflicting entries.
void validate_archive_path_set(std::span<const ArchivePathValidationEntry> entries) {
    std::vector<NormalizedArchivePath> paths;
    paths.reserve(entries.size());
    for (const auto& entry : entries) {
        paths.push_back(NormalizedArchivePath{
            .key = normalize_archive_collision_key(entry.path),
            .original_path = entry.path,
            .directory = entry.directory,
        });
    }
    std::sort(paths.begin(), paths.end(),
              [](const NormalizedArchivePath& lhs, const NormalizedArchivePath& rhs) { return lhs.key < rhs.key; });
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i > 0 && paths[i - 1].key == paths[i].key) {
            throw SecurityError("archive contains duplicate entry path: " + paths[i].original_path);
        }
        if (!paths[i].directory && (i + 1U) < paths.size() &&
            is_archive_path_descendant(paths[i + 1U].key, paths[i].key)) {
            throw SecurityError("archive file entry conflicts with child entry: " + paths[i].original_path);
        }
    }
}

std::string normalize_entry_name(const std::filesystem::path& relative_path) {
    std::string out;
    for (const auto& part : relative_path) {
        const auto piece = part.generic_u8string();
        if (piece.empty() || piece == u8".") {
            continue;
        }
        if (piece == u8"..") {
            throw SecurityError("source relative path contains traversal");
        }
        if (!out.empty()) {
            out.push_back('/');
        }
        out.append(reinterpret_cast<const char*>(piece.c_str()));
    }
    if (out.empty()) {
        throw SecurityError("source relative path is empty");
    }
    return normalize_archive_path_key(out);
}

}  // namespace superzip
