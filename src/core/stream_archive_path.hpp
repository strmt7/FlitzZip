#pragma once

#include "core/path_safety.hpp"
#include "core/path_text.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

namespace superzip {

// One trusted lowercase ASCII suffix and its replacement for a single-stream output filename.
struct StreamArchiveSuffix {
    std::string_view suffix;
    std::string_view replacement;
};

// Purpose: Derive a validated UTF-8 output filename from a native single-stream archive path.
// Inputs: `archive_path` supplies the filename; trusted `suffixes` are ordered first-match rules, longest first.
// Outputs: Returns a normalized relative name or throws on unsafe names; extraction must use `Utf8` path decoding.
inline std::string single_stream_entry_name(const std::filesystem::path& archive_path,
                                            std::initializer_list<StreamArchiveSuffix> suffixes) {
    auto filename = path_diagnostic_utf8(archive_path.filename());
    auto lower = filename;
    for (auto& byte : lower) {
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    bool matched = false;
    for (const auto& rule : suffixes) {
        if (lower.size() > rule.suffix.size() && lower.ends_with(rule.suffix)) {
            filename.resize(filename.size() - rule.suffix.size());
            filename.append(rule.replacement);
            matched = true;
            break;
        }
    }
    if (!matched) {
        filename = path_diagnostic_utf8(archive_path.stem());
    }
    if (filename.empty()) {
        filename = "payload";
    }
    return normalize_archive_path_key(filename);
}

}  // namespace superzip
