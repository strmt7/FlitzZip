#include "app/drop_payload.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TestGlobalHandle {
  public:
    // Purpose: Own a movable Win32 global-memory handle inside a test.
    // Inputs: `handle` is returned by `GlobalAlloc` and may be null.
    // Outputs: Stores ownership for deterministic release.
    explicit TestGlobalHandle(HGLOBAL handle) : handle_(handle) {}

    TestGlobalHandle(const TestGlobalHandle&) = delete;
    TestGlobalHandle& operator=(const TestGlobalHandle&) = delete;

    // Purpose: Transfer ownership of a test global-memory handle.
    // Inputs: `other` is the source wrapper.
    // Outputs: Leaves `other` empty and makes this wrapper responsible for release.
    TestGlobalHandle(TestGlobalHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    // Purpose: Replace this wrapper with another test global-memory handle.
    // Inputs: `other` is the source wrapper.
    // Outputs: Frees the current handle, transfers `other`, and leaves `other` empty.
    TestGlobalHandle& operator=(TestGlobalHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                GlobalFree(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    // Purpose: Release test global memory.
    // Inputs: None.
    // Outputs: Calls `GlobalFree` for a non-null handle.
    ~TestGlobalHandle() {
        if (handle_ != nullptr) {
            GlobalFree(handle_);
        }
    }

    // Purpose: Expose the owned handle for parser calls.
    // Inputs: None.
    // Outputs: Returns the owned HGLOBAL without transferring ownership.
    [[nodiscard]] HGLOBAL get() const noexcept {
        return handle_;
    }

  private:
    HGLOBAL handle_ = nullptr;
};

// Purpose: Build a DROPFILES payload containing UTF-16 paths.
// Inputs: `paths` is the ordered path list to encode.
// Outputs: Returns a movable global-memory handle that the caller owns.
TestGlobalHandle make_wide_drop_payload(const std::vector<std::wstring>& paths) {
    std::size_t characters = 1U;
    for (const auto& path : paths) {
        characters += path.size() + 1U;
    }
    const SIZE_T bytes = sizeof(superzip::app::detail::DropFilesHeader) + (characters * sizeof(wchar_t));
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes));
    REQUIRE_TRUE(handle.get() != nullptr);
    auto* raw = static_cast<unsigned char*>(GlobalLock(handle.get()));
    REQUIRE_TRUE(raw != nullptr);
    auto* header = reinterpret_cast<superzip::app::detail::DropFilesHeader*>(raw);
    header->pFiles = sizeof(superzip::app::detail::DropFilesHeader);
    header->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(raw + sizeof(superzip::app::detail::DropFilesHeader));
    for (const auto& path : paths) {
        cursor = std::copy(path.begin(), path.end(), cursor);
        ++cursor;
    }
    *cursor = L'\0';
    GlobalUnlock(handle.get());
    return handle;
}

// Purpose: Build a DROPFILES payload containing ANSI paths.
// Inputs: `paths` is the ordered path list to encode in the current ANSI code page.
// Outputs: Returns a movable global-memory handle that the caller owns.
TestGlobalHandle make_ansi_drop_payload(const std::vector<std::string_view>& paths) {
    std::size_t characters = 1U;
    for (const auto path : paths) {
        characters += path.size() + 1U;
    }
    const SIZE_T bytes = sizeof(superzip::app::detail::DropFilesHeader) + characters;
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes));
    REQUIRE_TRUE(handle.get() != nullptr);
    auto* raw = static_cast<unsigned char*>(GlobalLock(handle.get()));
    REQUIRE_TRUE(raw != nullptr);
    auto* header = reinterpret_cast<superzip::app::detail::DropFilesHeader*>(raw);
    header->pFiles = sizeof(superzip::app::detail::DropFilesHeader);
    header->fWide = FALSE;
    auto* cursor = reinterpret_cast<char*>(raw + sizeof(superzip::app::detail::DropFilesHeader));
    for (const auto path : paths) {
        cursor = std::copy(path.begin(), path.end(), cursor);
        ++cursor;
    }
    *cursor = '\0';
    GlobalUnlock(handle.get());
    return handle;
}

// Purpose: Build a malformed DROPFILES payload with an invalid path-list offset.
// Inputs: None.
// Outputs: Returns a movable global-memory handle that the caller owns.
TestGlobalHandle make_invalid_offset_drop_payload() {
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(superzip::app::detail::DropFilesHeader)));
    REQUIRE_TRUE(handle.get() != nullptr);
    auto* raw = static_cast<unsigned char*>(GlobalLock(handle.get()));
    REQUIRE_TRUE(raw != nullptr);
    auto* header = reinterpret_cast<superzip::app::detail::DropFilesHeader*>(raw);
    header->pFiles = sizeof(superzip::app::detail::DropFilesHeader);
    header->fWide = TRUE;
    GlobalUnlock(handle.get());
    return handle;
}

}  // namespace

TEST_CASE(drop_payload_parses_wide_file_list) {
    const auto handle = make_wide_drop_payload({L"C:\\SuperZip\\alpha.txt", L"C:\\SuperZip\\nested\\folder"});

    const auto paths = superzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_EQ(paths.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(paths[0].wstring(), std::wstring(L"C:\\SuperZip\\alpha.txt"));
    REQUIRE_EQ(paths[1].wstring(), std::wstring(L"C:\\SuperZip\\nested\\folder"));
}

TEST_CASE(drop_payload_parses_ansi_file_list) {
    const auto handle = make_ansi_drop_payload({"C:\\SuperZip\\ascii.txt", "C:\\SuperZip\\plain-folder"});

    const auto paths = superzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_EQ(paths.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(paths[0].wstring(), std::wstring(L"C:\\SuperZip\\ascii.txt"));
    REQUIRE_EQ(paths[1].wstring(), std::wstring(L"C:\\SuperZip\\plain-folder"));
}

TEST_CASE(drop_payload_rejects_malformed_offset) {
    const auto handle = make_invalid_offset_drop_payload();

    const auto paths = superzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_TRUE(paths.empty());
}
