#pragma once

#include <array>
#include <string>
#include <string_view>

namespace flitzzip::app {

// This is a storage generation, not an application version. Breaking changes
// require an explicit decoder/migration, new filename and compatibility fixtures.
inline constexpr int kSettingsProfileGeneration = 4;

// Newest first, excluding the current profile. The evolution gate requires
// every earlier versioned profile; generations one through three used settings.json.
inline constexpr std::array<std::wstring_view, 0> kPreviousSettingsFilenames{};

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
