#pragma once

#include <filesystem>
#include <string>

namespace superzip {

// Purpose: Represent a native filesystem path as UTF-8 diagnostic text without the host ANSI code page.
// Inputs: `path` is a native path; this conversion neither validates nor changes its filesystem meaning.
// Outputs: Returns UTF-8 text with native separators; may throw for invalid native encoding or allocation failure.
inline std::string path_diagnostic_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace superzip
