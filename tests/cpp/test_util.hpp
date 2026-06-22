#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>

void register_test(std::string name, std::function<void()> fn);

#define TEST_CASE(name)                                                                                                \
    static void name();                                                                                                \
    namespace {                                                                                                        \
    struct name##_registrar {                                                                                          \
        name##_registrar() {                                                                                           \
            register_test(#name, name);                                                                                \
        }                                                                                                              \
    } name##_registrar_instance;                                                                                       \
    }                                                                                                                  \
    static void name()

#define REQUIRE_TRUE(expr)                                                                                             \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            throw std::runtime_error(std::string("require failed: ") + #expr);                                         \
        }                                                                                                              \
    } while (false)

#define REQUIRE_EQ(lhs, rhs)                                                                                           \
    do {                                                                                                               \
        const auto lhs_value = (lhs);                                                                                  \
        const auto rhs_value = (rhs);                                                                                  \
        if (!(lhs_value == rhs_value)) {                                                                               \
            throw std::runtime_error(std::string("require equal failed: ") + #lhs + " != " + #rhs);                    \
        }                                                                                                              \
    } while (false)

// Purpose: Create a clean per-process temporary directory for one test.
// Inputs: `name` is a stable test-specific label.
// Outputs: Removes any prior generated directory, recreates it, and returns the path.
inline std::filesystem::path test_temp_dir(const std::string& name) {
    auto dir = std::filesystem::temp_directory_path() /
               ("superzip-test-" + name + "-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

namespace superzip_test {

// Purpose: Create a Windows directory junction for reparse-point security tests.
// Inputs: `junction` is the empty directory to convert and `target` is an existing directory target.
// Outputs: Returns true when the junction was created; false when the host refuses the test setup.
inline bool try_create_test_directory_junction(const std::filesystem::path& junction,
                                               const std::filesystem::path& target) {
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        return false;
    }
    std::filesystem::create_directories(junction, ec);
    if (ec) {
        return false;
    }
    const auto absolute_target = std::filesystem::absolute(target, ec);
    if (ec) {
        return false;
    }

    const std::wstring substitute_name = L"\\??\\" + absolute_target.wstring();
    const std::wstring print_name = absolute_target.wstring();
    const auto substitute_bytes = static_cast<std::uint16_t>(substitute_name.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<std::uint16_t>(print_name.size() * sizeof(wchar_t));
    const auto print_offset = static_cast<std::uint16_t>(substitute_bytes + sizeof(wchar_t));
    const std::size_t path_buffer_bytes = static_cast<std::size_t>(print_offset) + print_bytes + sizeof(wchar_t);

    struct MountPointReparseData {
        DWORD reparse_tag;
        WORD reparse_data_length;
        WORD reserved;
        WORD substitute_name_offset;
        WORD substitute_name_length;
        WORD print_name_offset;
        WORD print_name_length;
        wchar_t path_buffer[1];
    };

    std::vector<unsigned char> buffer(offsetof(MountPointReparseData, path_buffer) + path_buffer_bytes);
    auto* reparse = reinterpret_cast<MountPointReparseData*>(buffer.data());
    reparse->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse->reparse_data_length = static_cast<WORD>(8U + path_buffer_bytes);
    reparse->reserved = 0;
    reparse->substitute_name_offset = 0;
    reparse->substitute_name_length = substitute_bytes;
    reparse->print_name_offset = print_offset;
    reparse->print_name_length = print_bytes;
    std::memcpy(reparse->path_buffer, substitute_name.data(), substitute_bytes);
    std::memcpy(reinterpret_cast<unsigned char*>(reparse->path_buffer) + print_offset, print_name.data(), print_bytes);

    const auto junction_text = junction.wstring();
    HANDLE handle = CreateFileW(junction_text.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(junction_text.c_str());
        return false;
    }

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                    nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    if (ok == 0) {
        RemoveDirectoryW(junction_text.c_str());
        return false;
    }
    return true;
}

// Purpose: Remove a test-created directory junction without recursively deleting the target.
// Inputs: `junction` is the reparse directory created by `try_create_test_directory_junction`.
// Outputs: Removes the junction entry when present; ignores cleanup failures.
inline void remove_test_directory_junction(const std::filesystem::path& junction) {
    const auto junction_text = junction.wstring();
    const DWORD attributes = GetFileAttributesW(junction_text.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        (void)RemoveDirectoryW(junction_text.c_str());
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(junction, ignored);
}

}  // namespace superzip_test
