#include "test_util.hpp"

#include <memory>
#include <string_view>
#include <type_traits>

namespace {

// Purpose: Inspect the embedded manifest without executing the inspected application's code.
// Inputs: An already-built sibling executable under the current test binary's directory.
// Outputs: Returns a copy of its bounded manifest bytes; throws on missing/invalid resource metadata.
std::string embedded_manifest(const std::filesystem::path& executable) {
    const auto unload = [](HMODULE module) { (void)FreeLibrary(module); };
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(unload)> module(
        LoadLibraryExW(executable.c_str(), nullptr,
                       LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE),
        unload);
    REQUIRE_TRUE(module != nullptr);
    const auto resource = FindResourceW(module.get(), MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    REQUIRE_TRUE(resource != nullptr);
    const auto length = SizeofResource(module.get(), resource);
    REQUIRE_TRUE(length > 0U && length <= 64U * 1024U);
    const auto loaded = LoadResource(module.get(), resource);
    REQUIRE_TRUE(loaded != nullptr);
    const auto* data = static_cast<const char*>(LockResource(loaded));
    REQUIRE_TRUE(data != nullptr);
    return {data, length};
}

}  // namespace

// Purpose: Prevent CLI/test compatibility shims from diverging from the shipped graphical application.
// Inputs: The three actual built executable resources, inspected without launching the GUI.
// Outputs: Requires exactly one Windows 10/11 compatibility identity in each and preserved GUI DPI metadata.
TEST_CASE(windows_embedded_compatibility_manifest_parity) {
    std::wstring executable(32768U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE_TRUE(length > 0U && length < executable.size());
    executable.resize(length);
    const auto directory = std::filesystem::path(executable).parent_path();
    constexpr std::string_view identity = "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}";
    for (const auto* name : {L"superzip_tests.exe", L"superzip_cli.exe", L"SuperZip.exe"}) {
        const auto manifest = embedded_manifest(directory / name);
        const auto position = manifest.find(identity);
        REQUIRE_TRUE(position != std::string::npos);
        REQUIRE_EQ(manifest.find(identity, position + identity.size()), std::string::npos);
        if (std::wstring_view(name) == L"SuperZip.exe") {
            REQUIRE_TRUE(manifest.find("PerMonitorV2") != std::string::npos);
        }
    }
}
