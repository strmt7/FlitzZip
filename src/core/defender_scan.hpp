#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace flitzzip {

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

// Purpose: Preserve the exact scanner and target arguments across the Windows command-line boundary.
// Inputs: Resolved executable and caller-selected target paths; neither may be empty or contain a null character.
// Outputs: Returns the bounded quoted command, retaining non-remediation policy, or throws ArchiveError.
std::wstring defender_scan_command_line(std::wstring_view executable, std::wstring_view target);

// Purpose: Optionally scan a file or directory with Microsoft Defender without opening a visible console window.
// Inputs: `path` is the target to scan and `mode` controls whether scanning is disabled or full-path scanning is
// requested. Outputs: Returns scan attempt/clean/timeout/exit-code state; throws `ArchiveError` when the enabled
// target or scanner command cannot be admitted.
DefenderScanResult scan_with_windows_defender(const std::filesystem::path& path, DefenderScanMode mode);

// Purpose: Apply the fail-closed product policy to one completed Defender result.
// Inputs: `result` is returned by `scan_with_windows_defender` for an enabled scan.
// Outputs: Returns true only for an attempted, non-timeout, zero-exit, clean scan.
[[nodiscard]] bool defender_scan_passed(const DefenderScanResult& result) noexcept;

// Purpose: Reject any enabled Defender scan that did not positively report a clean target.
// Inputs: `result` is completed scan state and `path` identifies the user-visible target for diagnostics.
// Outputs: Returns normally only for a positive clean result; otherwise throws `SecurityError`.
void require_clean_defender_scan(const DefenderScanResult& result, const std::filesystem::path& path);

}  // namespace flitzzip
