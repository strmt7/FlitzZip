#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace superzip_test {

// Purpose: Export a passing compatibility fixture for an independent CLI roundtrip in the format matrix.
// Inputs: archive and expected contain small, test-owned, already verified files; the harness owns the export root.
// Outputs: Does nothing in ordinary tests; copies fixtures when explicitly requested, throwing on any copy failure.
inline void export_compat_fixture(const std::filesystem::path& archive, const std::filesystem::path& expected) {
    std::wstring root(32768, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(L"SUPERZIP_TEST_FIXTURE_EXPORT", root.data(), static_cast<DWORD>(root.size()));
    if (length == 0) {
        return;
    }
    if (length >= root.size()) {
        throw std::runtime_error("compatibility fixture export path exceeds environment limit");
    }
    root.resize(length);
    const auto destination = std::filesystem::path(root);
    const auto archives = destination / "archive";
    std::filesystem::create_directories(archives);
    std::filesystem::copy_file(archive, archives / archive.filename());
    std::filesystem::copy(expected, destination / "expected", std::filesystem::copy_options::recursive);
}

}  // namespace superzip_test
