#pragma once

#include <filesystem>

namespace superzip {

enum class DefenderScanMode {
    Disabled,
    QuickMetadata,
    FullPath,
};

// Purpose: Report the outcome of an optional Microsoft Defender scan attempt.
// Inputs: Populated by `scan_with_windows_defender`; no caller-owned resources are stored.
// Outputs: Carries attempt, cleanliness, timeout, and scanner exit-code state for CLI/GUI policy decisions.
struct DefenderScanResult {
    bool attempted = false;
    bool clean = false;
    bool timed_out = false;
    int exit_code = 0;
};

// Purpose: Optionally scan a file or directory with Microsoft Defender without opening a visible console window.
// Inputs: `path` is the target to scan and `mode` controls whether scanning is disabled or full-path scanning is
// requested. Outputs: Returns scan attempt/clean/timeout/exit-code state; throws `ArchiveError` only when an enabled
// scan target does not exist.
DefenderScanResult scan_with_windows_defender(const std::filesystem::path& path, DefenderScanMode mode);

}  // namespace superzip
