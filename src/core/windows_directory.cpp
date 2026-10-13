#include "core/windows_directory.hpp"

#ifdef _WIN32
#include "core/result.hpp"

#include <winternl.h>

#include <limits>
#include <memory>
#include <string>

namespace flitzzip::windows_detail {

// Purpose: Convert an explicit directory mode into its native, non-fallback creation disposition.
// Inputs: One internally selected directory mode.
// Outputs: Returns the corresponding NT disposition or rejects an invalid enum value.
static ULONG directory_disposition(DirectoryOpenMode mode) {
    switch (mode) {
    case DirectoryOpenMode::Existing:
        return FILE_OPEN;
    case DirectoryOpenMode::OpenOrCreate:
        return FILE_OPEN_IF;
    case DirectoryOpenMode::CreateNew:
        return FILE_CREATE;
    }
    throw ArchiveError("invalid relative directory open mode");
}

// Purpose: Bind directory creation and admission to an existing handle instead of resolving its mutable path again.
// Inputs: A synchronous parent handle, a single bounded UTF-16 component, creation mode and optional private DACL.
// Outputs: Returns a new owned non-delete-sharing handle, rejects reparses, and reports only exclusive name
// collisions as INVALID_HANDLE_VALUE. The synchronous NT operation completes before local name storage is released.
HANDLE open_directory_relative(HANDLE parent, std::wstring_view component, DirectoryOpenMode mode,
                               PSECURITY_DESCRIPTOR security_descriptor) {
    if (parent == nullptr || parent == INVALID_HANDLE_VALUE || component.empty() || component == L"." ||
        component == L".." || component.find_first_of(L"/\\:") != std::wstring_view::npos ||
        component.find(L'\0') != std::wstring_view::npos ||
        component.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t)) {
        throw SecurityError("relative directory requires one bounded filename component and a valid parent");
    }
    std::wstring name(component);
    const auto bytes = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    UNICODE_STRING native_name{bytes, bytes, name.data()};
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &native_name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
    attributes.SecurityDescriptor = security_descriptor;
    IO_STATUS_BLOCK completion{};
    HANDLE raw = nullptr;
    const auto status = NtCreateFile(
        &raw, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &completion,
        nullptr, FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, directory_disposition(mode),
        FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
    const auto error = status < 0 ? RtlNtStatusToDosError(status) : ERROR_SUCCESS;
    if (mode == DirectoryOpenMode::CreateNew && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)) {
        return INVALID_HANDLE_VALUE;
    }
    if (status < 0) {
        throw SecurityError("unable to open a relative output directory safely (Windows error " +
                            std::to_string(error) + ")");
    }
    std::unique_ptr<void, decltype(&CloseHandle)> handle(raw, &CloseHandle);
    FILE_ATTRIBUTE_TAG_INFO information{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &information, sizeof(information)) == 0 ||
        (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        throw SecurityError("relative output directory is not an ordinary directory");
    }
    return handle.release();
}

}  // namespace flitzzip::windows_detail
#endif
