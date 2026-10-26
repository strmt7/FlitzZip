#pragma once

#include <string>

namespace flitzzip::app {

// This is a storage generation, not an application version. Breaking changes
// require an explicit decoder/migration, new filename and compatibility fixtures.
inline constexpr int kSettingsProfileGeneration = 4;

// Purpose: Identify the current profile independently of C++ member names and release versions.
// Inputs: None. Outputs: Stable UTF-16 schema identifier for the registered current generation.
inline std::wstring current_settings_schema() {
    return L"flitzzip.settings.v" + std::to_wstring(kSettingsProfileGeneration);
}

// Purpose: Isolate incompatible profile generations so older binaries cannot overwrite newer data.
// Inputs: None. Outputs: Current profile basename; older generation files remain unchanged during migration.
inline std::wstring current_settings_filename() {
    return L"settings-v" + std::to_wstring(kSettingsProfileGeneration) + L".json";
}

}  // namespace flitzzip::app
