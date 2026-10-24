#include "core/defender_scan.hpp"
#include "core/path_text.hpp"

#include "core/com_task_string.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace flitzzip {
namespace {

constexpr int kDefenderUnavailableExitCode = -1;
constexpr int kDefenderTimeoutExitCode = -2;

// Purpose: Quote one admitted Windows argument without turning trailing backslashes into quote escapes.
// Inputs: A null-free argument within the process command-line limit.
// Outputs: Returns one quoted argument with exact backslashes and literal quotes preserved by the Windows parser.
std::wstring quote_windows_argument(std::wstring_view argument) {
    std::wstring quoted(1U, L'"');
    std::size_t backslashes = 0U;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        quoted.append(backslashes * (character == L'"' ? 2U : 1U), L'\\');
        if (character == L'"') {
            quoted.push_back(L'\\');
        }
        quoted.push_back(character);
        backslashes = 0U;
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

#ifdef _WIN32
constexpr DWORD kDefenderScanTimeoutMs = 30U * 60U * 1000U;
constexpr DWORD kDefenderTerminateGraceMs = 5000U;
constexpr UINT kDefenderTimeoutTerminateCode = 0xE0000002U;

class UniqueHandle {
  public:
    // Purpose: Take ownership of a Win32 handle returned by process creation.
    // Inputs: `handle` may be null; ownership transfers to this object.
    // Outputs: Stores the handle for deterministic cleanup.
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // Purpose: Close the owned Win32 handle once.
    // Inputs: Uses the handle captured by the constructor.
    // Outputs: Releases the OS resource; never throws.
    ~UniqueHandle() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    // Purpose: Expose the owned handle to Win32 wait/query APIs.
    // Inputs: None.
    // Outputs: Returns the raw handle without transferring ownership.
    HANDLE get() const noexcept {
        return handle_;
    }

  private:
    HANDLE handle_ = nullptr;
};

// Purpose: Resolve a trusted Windows Known Folder path.
// Inputs: `folder_id` is the requested known folder and `fallback` is used when Windows cannot resolve it.
// Outputs: Returns a nonempty absolute folder path without trusting mutable process environment variables.
std::filesystem::path known_folder_path(REFKNOWNFOLDERID folder_id, const std::filesystem::path& fallback) {
    CoTaskMemWideString raw_path;
    const HRESULT result = SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, raw_path.put());
    if (FAILED(result) || raw_path.get() == nullptr) {
        return fallback;
    }
    std::filesystem::path path(raw_path.get());
    return path.empty() ? fallback : path;
}

// Purpose: Locate the newest Microsoft Defender platform MpCmdRun executable.
// Inputs: None; uses ProgramData and the versioned Defender Platform directory.
// Outputs: Returns the latest version-named MpCmdRun path, or empty when no platform copy is available.
std::optional<std::filesystem::path> latest_platform_defender_executable() {
    const auto platform_root =
        known_folder_path(FOLDERID_ProgramData, L"C:\\ProgramData") / L"Microsoft" / L"Windows Defender" / L"Platform";
    std::error_code ec;
    if (!std::filesystem::is_directory(platform_root, ec) || ec) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> candidates;
    std::filesystem::directory_iterator iterator(platform_root, ec);
    if (ec) {
        return std::nullopt;
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto directory = iterator->path();
        const auto executable = directory / L"MpCmdRun.exe";
        std::error_code directory_ec;
        std::error_code executable_ec;
        if (std::filesystem::is_directory(directory, directory_ec) && !directory_ec &&
            std::filesystem::exists(executable, executable_ec) && !executable_ec) {
            candidates.push_back(directory);
        }
        iterator.increment(ec);
        if (ec) {
            return std::nullopt;
        }
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        return left.filename().wstring() > right.filename().wstring();
    });
    return candidates.front() / L"MpCmdRun.exe";
}

// Purpose: Locate the best available Microsoft Defender MpCmdRun executable.
// Inputs: None; uses official Platform-first lookup with Program Files fallback.
// Outputs: Returns an executable path when present, or empty when Defender is unavailable.
std::optional<std::filesystem::path> defender_executable_path() {
    if (auto platform = latest_platform_defender_executable(); platform.has_value()) {
        return platform;
    }
    const auto fallback =
        known_folder_path(FOLDERID_ProgramFiles, L"C:\\Program Files") / L"Windows Defender" / L"MpCmdRun.exe";
    std::error_code ec;
    if (std::filesystem::exists(fallback, ec) && !ec) {
        return fallback;
    }
    return std::nullopt;
}
#endif

// Purpose: Build a result for Defender being unavailable before a scan starts.
// Inputs: `exit_code` identifies the unavailable/error condition.
// Outputs: Returns a non-clean, non-attempted scan result.
DefenderScanResult defender_unavailable_result(int exit_code) {
    return DefenderScanResult{
        .attempted = false,
        .clean = false,
        .timed_out = false,
        .exit_code = exit_code,
    };
}

// Purpose: Validate the user-selected scan target before invoking any scanner.
// Inputs: `path` is the file or directory requested by the caller.
// Outputs: Throws `ArchiveError` when the target is absent or cannot be inspected.
void ensure_scan_target_exists(const std::filesystem::path& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec || !exists) {
        throw ArchiveError("Defender scan target does not exist: " + path_diagnostic_utf8(path));
    }
}

#ifdef _WIN32
// Purpose: Convert a completed Defender process exit code into product scan state.
// Inputs: `process_handle` is an active process handle owned by the caller.
// Outputs: Waits for completion up to the scan timeout and returns clean, failed, or timed-out state.
DefenderScanResult wait_for_defender_scan(HANDLE process_handle) {
    const DWORD wait_result = WaitForSingleObject(process_handle, kDefenderScanTimeoutMs);
    if (wait_result == WAIT_TIMEOUT) {
        (void)TerminateProcess(process_handle, kDefenderTimeoutTerminateCode);
        (void)WaitForSingleObject(process_handle, kDefenderTerminateGraceMs);
        return DefenderScanResult{
            .attempted = true,
            .clean = false,
            .timed_out = true,
            .exit_code = kDefenderTimeoutExitCode,
        };
    }
    if (wait_result != WAIT_OBJECT_0) {
        return DefenderScanResult{
            .attempted = true,
            .clean = false,
            .timed_out = false,
            .exit_code = static_cast<int>(GetLastError()),
        };
    }

    DWORD exit_code = 1;
    if (GetExitCodeProcess(process_handle, &exit_code) == 0) {
        return DefenderScanResult{
            .attempted = true,
            .clean = false,
            .timed_out = false,
            .exit_code = static_cast<int>(GetLastError()),
        };
    }
    return DefenderScanResult{
        .attempted = true,
        .clean = exit_code == 0,
        .timed_out = false,
        .exit_code = static_cast<int>(exit_code),
    };
}
#endif

}  // namespace

// Purpose: Build a complete Defender invocation without losing path boundaries or truncating at embedded nulls.
// Inputs: Nonempty executable and target paths resolved by the caller; maximum length excludes the final null.
// Outputs: Returns exact Windows arguments or throws before launching a scanner.
std::wstring defender_scan_command_line(std::wstring_view executable, std::wstring_view target) {
    constexpr std::size_t maximum_command_units = 32766U;
    for (const auto argument : {executable, target}) {
        if (argument.empty() || argument.size() > maximum_command_units ||
            argument.find(L'\0') != std::wstring_view::npos) {
            throw ArchiveError("Defender command path is empty, too long or contains a null character");
        }
    }
    auto command = quote_windows_argument(executable) + L" -Scan -ScanType 3 -File " + quote_windows_argument(target) +
                   L" -DisableRemediation";
    if (command.size() > maximum_command_units) {
        throw ArchiveError("Defender command exceeds the Windows command-line limit");
    }
    return command;
}

// Purpose: Optionally scan an existing file or directory with Microsoft Defender.
// Inputs: `path` is the caller-selected target and `mode` disables or enables the scan policy.
// Outputs: Returns scan attempt, cleanliness, timeout, and exit-code state; throws `ArchiveError` for missing targets
// or invalid scanner commands when scanning is enabled.
DefenderScanResult scan_with_windows_defender(const std::filesystem::path& path, DefenderScanMode mode) {
    if (mode == DefenderScanMode::Disabled) {
        return {};
    }
    ensure_scan_target_exists(path);
#ifndef _WIN32
    return {};
#else
    const auto defender = defender_executable_path();
    if (!defender.has_value()) {
        return defender_unavailable_result(kDefenderUnavailableExitCode);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    auto command = defender_scan_command_line(defender->native(), path.native());
    const BOOL created = CreateProcessW(defender->c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    if (!created) {
        return defender_unavailable_result(static_cast<int>(GetLastError()));
    }
    UniqueHandle thread_handle(process.hThread);
    UniqueHandle process_handle(process.hProcess);
    return wait_for_defender_scan(process_handle.get());
#endif
}

// Purpose: Apply the fail-closed product policy to one completed Defender result.
// Inputs: `result` is returned by `scan_with_windows_defender` for an enabled scan.
// Outputs: Returns true only for an attempted, non-timeout, zero-exit, clean scan.
bool defender_scan_passed(const DefenderScanResult& result) noexcept {
    return result.attempted && result.clean && !result.timed_out && result.exit_code == 0;
}

// Purpose: Reject any enabled Defender scan that did not positively report a clean target.
// Inputs: `result` is completed scan state and `path` identifies the user-visible target for diagnostics.
// Outputs: Returns normally only for a positive clean result; otherwise throws `SecurityError`.
void require_clean_defender_scan(const DefenderScanResult& result, const std::filesystem::path& path) {
    if (!defender_scan_passed(result)) {
        throw SecurityError("Microsoft Defender did not positively report the target as clean: " +
                            path_diagnostic_utf8(path));
    }
}

}  // namespace flitzzip
