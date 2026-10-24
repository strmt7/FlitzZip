#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace flitzzip_test {

// Purpose: Check complete UTF-8 path diagnostics without assuming a Windows alias spelling.
// Inputs: Exact diagnostic prefix and caller-selected native path; existing aliases must name the same object.
// Outputs: True only for a valid, complete absolute path with the expected filename and spelling or file identity.
inline bool path_diagnostic_matches(std::string_view message, std::string_view prefix,
                                    const std::filesystem::path& expected) {
    if (!message.starts_with(prefix)) {
        return false;
    }
    const auto suffix = message.substr(prefix.size());
    if (suffix.empty() || suffix.find('\0') != std::string_view::npos) {
        return false;
    }
    try {
        const std::filesystem::path reported(
            std::u8string_view(reinterpret_cast<const char8_t*>(suffix.data()), suffix.size()));
        const auto encoded = reported.u8string();
        if (std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()) != suffix ||
            !reported.is_absolute() || reported.filename() != expected.filename()) {
            return false;
        }
        if (reported == expected) {
            return true;
        }
        std::error_code error;
        const auto same_object = std::filesystem::equivalent(reported, expected, error);
        return !error && same_object;
    } catch (const std::system_error&) {
        return false;
    }
}

}  // namespace flitzzip_test
