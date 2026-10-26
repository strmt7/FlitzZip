#pragma once

#include "app/settings_profile.hpp"
#include "core/path_text.hpp"

#include <filesystem>

namespace flitzzip::app {

// Purpose: Select the current settings file or the one-time predecessor migration source.
// Inputs: `input` is the trusted per-user settings path; smoke and other filenames never migrate.
// Outputs: Returns the normalized current file, the legacy source only when current is absent, or current for defaults;
// filesystem errors propagate. No file is changed, and malformed current contents never trigger migration.
inline std::filesystem::path settings_read_path(const std::filesystem::path& input) {
    const auto current = std::filesystem::path(windows_api_path(input));
    if (std::filesystem::exists(current) ||
        (current.filename() != L"settings.json" && current.filename() != current_settings_filename()) ||
        current.parent_path().filename() != L"FlitzZip") {
        return current;
    }
    if (current.filename() == current_settings_filename()) {
        for (const auto filename : kPreviousSettingsFilenames) {
            const auto previous = current.parent_path() / filename;
            if (std::filesystem::exists(previous))
                return previous;
        }
        const auto previous = current.parent_path() / L"settings.json";
        if (std::filesystem::exists(previous))
            return previous;
    }
    // This literal identifies existing installations and is never a new write destination.
    const auto predecessor = current.parent_path().parent_path() / L"SuperZip" / L"settings.json";
    return std::filesystem::exists(predecessor) ? predecessor : current;
}

}  // namespace flitzzip::app
