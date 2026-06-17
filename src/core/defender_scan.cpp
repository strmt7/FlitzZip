#include "core/defender_scan.hpp"

#include "core/result.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace superzip {
namespace {

constexpr int kDefenderUnavailableExitCode = -1;
constexpr int kDefenderTimeoutExitCode = -2;

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
        throw ArchiveError("Defender scan target does not exist: " + path.string());
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

// Purpose: Optionally scan an existing file or directory with Microsoft Defender.
// Inputs: `path` is the caller-selected target and `mode` disables or enables the scan policy.
// Outputs: Returns scan attempt, cleanliness, timeout, and exit-code state; throws `ArchiveError` for missing enabled
// targets.
DefenderScanResult scan_with_windows_defender(const std::filesystem::path& path, DefenderScanMode mode) {
    if (mode == DefenderScanMode::Disabled) {
        return {};
    }
    ensure_scan_target_exists(path);
#ifndef _WIN32
    return {};
#else
    const auto defender = std::filesystem::path("C:/Program Files/Windows Defender/MpCmdRun.exe");
    if (!std::filesystem::exists(defender)) {
        return defender_unavailable_result(kDefenderUnavailableExitCode);
    }

    std::wstring command = L"\"";
    command += defender.wstring();
    command += L"\" -Scan -ScanType 3 -File \"";
    command += path.wstring();
    command += L"\" -DisableRemediation";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command;
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    if (!created) {
        return defender_unavailable_result(static_cast<int>(GetLastError()));
    }
    UniqueHandle thread_handle(process.hThread);
    UniqueHandle process_handle(process.hProcess);
    return wait_for_defender_scan(process_handle.get());
#endif
}

}  // namespace superzip
