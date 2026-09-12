#pragma once

#include <filesystem>
#include <string>
#include <stdexcept>

namespace superzip {

// Purpose: Represent a native filesystem path as UTF-8 diagnostic text without the host ANSI code page.
// Inputs: `path` is a native path; this conversion neither validates nor changes its filesystem meaning.
// Outputs: Returns UTF-8 text with native separators; may throw for invalid native encoding or allocation failure.
inline std::string path_diagnostic_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

#ifdef _WIN32
// Purpose: Recover the ordinary drive/UNC representation when walking Windows parent components.
// Inputs: `path` may carry an extended filesystem prefix generated at an I/O boundary.
// Outputs: Removes only recognized drive/UNC prefixes; does not resolve links or validate archive metadata.
inline std::filesystem::path windows_regular_path(const std::filesystem::path& path) {
    const auto& text = path.native();
    if (text.starts_with(L"\\\\?\\UNC\\"))
        return std::filesystem::path(L"\\\\" + text.substr(8));
    if (text.starts_with(L"\\\\?\\") && text.size() >= 7 && text[5] == L':' && text[6] == L'\\')
        return std::filesystem::path(text.substr(4));
    return path;
}

// Purpose: Adapt filesystem paths for Win32 I/O without depending on machine-wide long-path opt-in.
// Inputs: `path` is a filesystem path, not unvalidated archive metadata or a device namespace.
// Outputs: Returns an absolute normalized extended-length drive/UNC path; rejects device or oversized paths.
// This is a representation conversion, not a containment or reparse-point validation.
inline std::wstring windows_api_path(const std::filesystem::path& path) {
    if (path.native().size() >= 32767U || path.native().find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("filesystem path exceeds Windows representation limits");
    auto absolute = std::filesystem::absolute(windows_regular_path(path)).lexically_normal();
    const auto text = absolute.make_preferred().native();
    std::wstring result;
    if (text.starts_with(L"\\\\?\\")) {
        if (!text.starts_with(L"\\\\?\\UNC\\") && !(text.size() >= 7 && text[5] == L':' && text[6] == L'\\'))
            throw std::invalid_argument("device paths are not filesystem publication paths");
        result = text;
    } else if (text.starts_with(L"\\\\.\\")) {
        throw std::invalid_argument("device paths are not filesystem publication paths");
    } else if (text.starts_with(L"\\\\")) {
        result = L"\\\\?\\UNC\\" + text.substr(2);
    } else if (text.size() >= 3 && text[1] == L':' && text[2] == L'\\') {
        result = L"\\\\?\\" + text;
    } else {
        throw std::invalid_argument("filesystem path requires a drive or UNC root");
    }
    if (result.size() >= 32767U || result.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("filesystem path exceeds Windows representation limits");
    return result;
}
#endif

}  // namespace superzip
