#include "core/defender_scan.hpp"

#include "core/result.hpp"

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace superzip {

DefenderScanResult scan_with_windows_defender(
    const std::filesystem::path& path,
    DefenderScanMode mode) {
    if (mode == DefenderScanMode::Disabled) {
        return {};
    }
#ifndef _WIN32
    (void)path;
    return {};
#else
    const auto defender = std::filesystem::path(
        "C:/Program Files/Windows Defender/MpCmdRun.exe");
    if (!std::filesystem::exists(defender)) {
        return DefenderScanResult{.attempted = false, .clean = false, .exit_code = -1};
    }
    if (!std::filesystem::exists(path)) {
        throw ArchiveError("Defender scan target does not exist: " + path.string());
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
    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);
    if (!created) {
        return DefenderScanResult{.attempted = false, .clean = false, .exit_code = static_cast<int>(GetLastError())};
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return DefenderScanResult{
        .attempted = true,
        .clean = exit_code == 0,
        .exit_code = static_cast<int>(exit_code),
    };
#endif
}

}  // namespace superzip
