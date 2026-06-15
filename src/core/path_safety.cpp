#include "core/path_safety.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>

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
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    static constexpr std::array reserved = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
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
    return std::ranges::any_of(component, [](unsigned char ch) {
        return ch < 0x20U;
    });
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

}  // namespace

std::filesystem::path safe_join_archive_path(
    const std::filesystem::path& destination_root,
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
    return target;
}

std::string normalize_archive_path_key(const std::string& archive_path) {
    if (archive_path.empty()) {
        throw SecurityError("archive entry path is empty");
    }
    if (archive_path.starts_with('/') || archive_path.starts_with('\\') ||
        archive_path.starts_with("//") || archive_path.starts_with("\\\\") ||
        has_drive_prefix(archive_path)) {
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
