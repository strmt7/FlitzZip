#include "app/drop_payload.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
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

// Purpose: Frame arbitrary shell metadata without allowing allocator padding to supply missing terminators.
// Inputs: The wire offset, character mode and deliberately bounded raw list bytes.
// Outputs: Returns caller-owned global memory whose unused allocation bytes are nonzero.
TestGlobalHandle make_raw_drop_payload(DWORD offset, bool wide, std::span<const unsigned char> list) {
    using Header = flitzzip::app::detail::DropFilesHeader;
    REQUIRE_TRUE(list.size() <= flitzzip::app::kMaxShellDropPayloadBytes - sizeof(Header));
    const auto required_bytes = sizeof(Header) + list.size();
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE, required_bytes));
    REQUIRE_TRUE(handle.get() != nullptr);
    const flitzzip::app::detail::GlobalLockView view(handle.get());
    REQUIRE_TRUE(view.data() != nullptr);
    auto* bytes = const_cast<unsigned char*>(static_cast<const unsigned char*>(view.data()));
    const auto allocation_bytes = GlobalSize(handle.get());
    REQUIRE_TRUE(allocation_bytes >= required_bytes);
    const std::span allocation(bytes, allocation_bytes);
    std::ranges::fill(allocation, static_cast<unsigned char>(0x7f));
    std::construct_at(reinterpret_cast<Header*>(bytes), Header{.pFiles = offset, .fWide = wide ? TRUE : FALSE});
    const auto destination = allocation.subspan(sizeof(Header), list.size());
    REQUIRE_EQ(std::ranges::copy(list, destination.begin()).out, destination.end());
    return handle;
}

// Purpose: Build a DROPFILES payload containing UTF-16 paths.
// Inputs: `paths` is the ordered path list to encode.
// Outputs: Returns a movable global-memory handle that the caller owns.
TestGlobalHandle make_wide_drop_payload(const std::vector<std::wstring>& paths) {
    std::size_t characters = 1U;
    for (const auto& path : paths) {
        characters += path.size() + 1U;
    }
    const SIZE_T bytes = sizeof(flitzzip::app::detail::DropFilesHeader) + (characters * sizeof(wchar_t));
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes));
    REQUIRE_TRUE(handle.get() != nullptr);
    auto* raw = static_cast<unsigned char*>(GlobalLock(handle.get()));
    REQUIRE_TRUE(raw != nullptr);
    auto* header = reinterpret_cast<flitzzip::app::detail::DropFilesHeader*>(raw);
    header->pFiles = sizeof(flitzzip::app::detail::DropFilesHeader);
    header->fWide = TRUE;
    auto* cursor = reinterpret_cast<wchar_t*>(raw + sizeof(flitzzip::app::detail::DropFilesHeader));
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
    const SIZE_T bytes = sizeof(flitzzip::app::detail::DropFilesHeader) + characters;
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes));
    REQUIRE_TRUE(handle.get() != nullptr);
    auto* raw = static_cast<unsigned char*>(GlobalLock(handle.get()));
    REQUIRE_TRUE(raw != nullptr);
    auto* header = reinterpret_cast<flitzzip::app::detail::DropFilesHeader*>(raw);
    header->pFiles = sizeof(flitzzip::app::detail::DropFilesHeader);
    header->fWide = FALSE;
    auto* cursor = reinterpret_cast<char*>(raw + sizeof(flitzzip::app::detail::DropFilesHeader));
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
    TestGlobalHandle handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(flitzzip::app::detail::DropFilesHeader)));
    REQUIRE_TRUE(handle.get() != nullptr);
    auto* raw = static_cast<unsigned char*>(GlobalLock(handle.get()));
    REQUIRE_TRUE(raw != nullptr);
    auto* header = reinterpret_cast<flitzzip::app::detail::DropFilesHeader*>(raw);
    header->pFiles = sizeof(flitzzip::app::detail::DropFilesHeader);
    header->fWide = TRUE;
    GlobalUnlock(handle.get());
    return handle;
}

}  // namespace

TEST_CASE(drop_payload_parses_wide_file_list) {
    const auto handle = make_wide_drop_payload({L"C:\\FlitzZip\\alpha.txt", L"C:\\FlitzZip\\nested\\folder"});

    const auto paths = flitzzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_EQ(paths.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(paths[0].wstring(), std::wstring(L"C:\\FlitzZip\\alpha.txt"));
    REQUIRE_EQ(paths[1].wstring(), std::wstring(L"C:\\FlitzZip\\nested\\folder"));
}

TEST_CASE(drop_payload_parses_large_wide_file_list) {
    std::vector<std::wstring> expected;
    expected.reserve(512U);
    for (int index = 0; index < 512; ++index) {
        expected.emplace_back(std::wstring(L"C:\\FlitzZip\\bulk\\file-") + std::to_wstring(index) + L".txt");
    }
    const auto handle = make_wide_drop_payload(expected);

    const auto paths = flitzzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_EQ(paths.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_EQ(paths[index].wstring(), expected[index]);
    }
}

TEST_CASE(drop_payload_parses_ansi_file_list) {
    const auto handle = make_ansi_drop_payload({"C:\\FlitzZip\\ascii.txt", "C:\\FlitzZip\\plain-folder"});

    const auto paths = flitzzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_EQ(paths.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(paths[0].wstring(), std::wstring(L"C:\\FlitzZip\\ascii.txt"));
    REQUIRE_EQ(paths[1].wstring(), std::wstring(L"C:\\FlitzZip\\plain-folder"));
}

TEST_CASE(drop_payload_rejects_malformed_offset) {
    const auto handle = make_invalid_offset_drop_payload();

    const auto paths = flitzzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_TRUE(paths.empty());
}

TEST_CASE(drop_payload_rejects_path_count_over_queue_budget) {
    std::vector<std::wstring> paths(flitzzip::app::kMaxQueueItems + 1U, L"C:\\FlitzZip\\bounded.txt");
    const auto handle = make_wide_drop_payload(paths);

    const auto parsed = flitzzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_TRUE(parsed.empty());
}

TEST_CASE(drop_payload_rejects_overlong_path) {
    std::wstring path = L"C:\\";
    path.append(flitzzip::app::kMaxShellPathCharacters, L'a');
    const auto handle = make_wide_drop_payload({path});

    const auto parsed = flitzzip::app::paths_from_dropfiles_global(handle.get());

    REQUIRE_TRUE(parsed.empty());
}

TEST_CASE(drop_path_policy_rejects_remote_device_and_ambiguous_paths) {
    REQUIRE_TRUE(!flitzzip::app::is_supported_local_drop_path(L"\\\\server\\share\\input.bin"));
    REQUIRE_TRUE(!flitzzip::app::is_supported_local_drop_path(L"\\\\?\\C:\\input.bin"));
    REQUIRE_TRUE(!flitzzip::app::is_supported_local_drop_path(L"C:\\safe\\..\\input.bin"));
    REQUIRE_TRUE(!flitzzip::app::is_supported_local_drop_path(L"relative\\input.bin"));
}

TEST_CASE(drop_path_policy_accepts_local_absolute_path) {
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error) / L"FlitzZip-drop-input.bin";

    REQUIRE_TRUE(!error);
    REQUIRE_TRUE(flitzzip::app::is_supported_local_drop_path(path));
}

// Purpose: Reject unusable memory and hostile offsets before interpreting shell metadata.
// Inputs: Null/discarded handles, short allocations, out-of-range offsets and unaligned wide data.
// Outputs: Returns no paths and leaves every caller-owned allocation releasable.
TEST_CASE(drop_payload_rejects_invalid_memory_and_header_geometry) {
    REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(nullptr).empty());
    const TestGlobalHandle short_payload(GlobalAlloc(GMEM_MOVEABLE, 1));
    REQUIRE_TRUE(short_payload.get() != nullptr);
    REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(short_payload.get()).empty());
    const TestGlobalHandle discarded(GlobalAlloc(GMEM_MOVEABLE, 0));
    REQUIRE_TRUE(discarded.get() != nullptr);
    REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(discarded.get()).empty());
    using Header = flitzzip::app::detail::DropFilesHeader;
    const std::array<unsigned char, 8> list{'a', 0, 0};
    for (const DWORD offset : {0UL, static_cast<DWORD>(sizeof(Header) - 1U), std::numeric_limits<DWORD>::max()}) {
        const auto handle = make_raw_drop_payload(offset, false, list);
        REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(handle.get()).empty());
    }
    const auto unaligned = make_raw_drop_payload(sizeof(Header) + 1U, true, list);
    REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(unaligned.get()).empty());
}

// Purpose: Refuse partial path lists instead of exposing a valid prefix to the queue.
// Inputs: Independently framed ANSI/wide lists with a valid first path and an unterminated second.
// Outputs: Public parsing returns an empty vector for either encoding; a terminated control survives.
TEST_CASE(drop_payload_discards_prefix_when_final_path_is_unterminated) {
    using Header = flitzzip::app::detail::DropFilesHeader;
    const std::array<unsigned char, 8> ansi{'o', 'k', 0, 'b', 'r', 'o', 'k', 'e'};
    const auto broken_ansi = make_raw_drop_payload(sizeof(Header), false, ansi);
    REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(broken_ansi.get()).empty());
    const std::array<wchar_t, 8> wide{L'o', L'k', 0, L'b', L'r', L'o', L'k', L'e'};
    const auto wide_bytes = std::as_bytes(std::span(wide));
    const auto broken_wide = make_raw_drop_payload(
        sizeof(Header), true, {reinterpret_cast<const unsigned char*>(wide_bytes.data()), wide_bytes.size()});
    REQUIRE_TRUE(flitzzip::app::paths_from_dropfiles_global(broken_wide.get()).empty());
    const auto good = make_ansi_drop_payload({"ok", "broken"});
    const auto parsed = flitzzip::app::paths_from_dropfiles_global(good.get());
    REQUIRE_EQ(parsed.size(), 2U);
    REQUIRE_EQ(parsed.back(), std::filesystem::path("broken"));
}

// Purpose: Require complete multisz termination even for empty and truncated direct views.
// Inputs: Exact bounded arrays, avoiding any global allocator padding, in both character encodings.
// Outputs: Accepts only the two-zero empty form and rejects every truncated path boundary.
TEST_CASE(drop_payload_direct_views_require_complete_termination) {
    const std::array<char, 5> ansi{'a', 0, 'b', 0, 0};
    const std::array<wchar_t, 5> wide{L'a', 0, L'b', 0, 0};
    for (std::size_t size = 0; size <= ansi.size(); ++size) {
        std::vector<std::filesystem::path> paths;
        const bool valid = flitzzip::app::detail::append_ansi_drop_paths(ansi.data(), ansi.data() + size, paths);
        REQUIRE_EQ(valid, size == ansi.size());
        paths.clear();
        REQUIRE_EQ(flitzzip::app::detail::append_wide_drop_paths(wide.data(), wide.data() + size, paths),
                   size == wide.size());
    }
    const std::array<char, 2> empty_ansi{};
    const std::array<wchar_t, 2> empty_wide{};
    std::vector<std::filesystem::path> paths;
    REQUIRE_TRUE(!flitzzip::app::detail::append_ansi_drop_paths(empty_ansi.data(), empty_ansi.data() + 1, paths));
    REQUIRE_TRUE(flitzzip::app::detail::append_ansi_drop_paths(empty_ansi.data(), empty_ansi.data() + 2, paths));
    REQUIRE_TRUE(!flitzzip::app::detail::append_wide_drop_paths(empty_wide.data(), empty_wide.data() + 1, paths));
    REQUIRE_TRUE(flitzzip::app::detail::append_wide_drop_paths(empty_wide.data(), empty_wide.data() + 2, paths));
    REQUIRE_TRUE(paths.empty());
}

// Purpose: Reject invalid conversion extents without accessing attacker-controlled pointers.
// Inputs: Null, empty and greater-than-int conversion requests, plus a valid ASCII control.
// Outputs: Invalid requests yield no text; the control retains exact characters.
TEST_CASE(drop_payload_conversion_validates_extent_before_access) {
    REQUIRE_TRUE(flitzzip::app::detail::ansi_drop_path_to_wide(nullptr, 3).empty());
    REQUIRE_TRUE(flitzzip::app::detail::ansi_drop_path_to_wide("x", 0).empty());
    REQUIRE_TRUE(flitzzip::app::detail::ansi_drop_path_to_wide("x", static_cast<std::size_t>(INT_MAX) + 1U).empty());
    REQUIRE_EQ(flitzzip::app::detail::ansi_drop_path_to_wide("C:\\ascii", 8), L"C:\\ascii");
}

// Purpose: Enforce the aggregate shell-path budget separately from the path-count/per-path limits.
// Inputs: Valid exact-budget lists and otherwise identical lists exceeding that budget by one character.
// Outputs: Both encodings accept the boundary and reject overflow without retaining a partial result.
TEST_CASE(drop_payload_aggregate_character_budget_has_exact_boundary) {
    const auto limit = flitzzip::app::kMaxShellPathCharacters;
    const auto budget = flitzzip::app::kMaxShellDropPathCharacters;
    std::vector<std::wstring> wide(budget / limit, std::wstring(limit, L'a'));
    wide.emplace_back(budget % limit, L'b');
    std::vector<std::string> ansi(wide.size() - 1U, std::string(limit, 'a'));
    ansi.emplace_back(budget % limit, 'b');
    for (const bool excessive : {false, true}) {
        std::vector<std::string_view> views;
        for (const auto& path : ansi) {
            views.emplace_back(path);
        }
        {
            const auto handle = make_ansi_drop_payload(views);
            const auto parsed = flitzzip::app::paths_from_dropfiles_global(handle.get());
            REQUIRE_EQ(parsed.size(), excessive ? 0U : wide.size());
        }
        {
            const auto handle = make_wide_drop_payload(wide);
            const auto parsed = flitzzip::app::paths_from_dropfiles_global(handle.get());
            REQUIRE_EQ(parsed.size(), excessive ? 0U : wide.size());
        }
        wide.back().push_back(L'b');
        ansi.back().push_back('b');
    }
}
