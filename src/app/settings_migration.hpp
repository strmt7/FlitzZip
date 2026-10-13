#pragma once

#include <filesystem>

namespace flitzzip::app {

// Purpose: Select the current settings file or the one-time predecessor migration source.
// Inputs: `current` is the trusted per-user settings path; smoke and other filenames never migrate.
// Outputs: Returns an existing current file, the legacy source only when current is absent, or current for defaults;
// filesystem errors propagate. No file is changed, and malformed current contents never trigger migration.
inline std::filesystem::path settings_read_path(const std::filesystem::path& current) {
    if (std::filesystem::exists(current) || current.filename() != L"settings.json" ||
        current.parent_path().filename() != L"FlitzZip") {
        return current;
    }
    // This literal identifies existing installations and is never a new write destination.
    const auto predecessor = current.parent_path().parent_path() / L"SuperZip" / L"settings.json";
    return std::filesystem::exists(predecessor) ? predecessor : current;
}

}  // namespace flitzzip::app
