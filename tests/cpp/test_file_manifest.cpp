#include "core/file_manifest.hpp"
#include "core/path_text.hpp"
#include "core/result.hpp"
#include "core/windows_handle.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <new>
#include <utility>

namespace {

// Purpose: Observe source-write exclusion without truncating or modifying the selected file.
// Inputs: A test-owned file and whether a write-capable handle should currently be admitted.
// Outputs: Closes any admitted handle and requires the expected sharing result.
void require_source_write_access(const std::filesystem::path& source, bool admitted) {
    const auto handle =
        CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const auto error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (handle != INVALID_HANDLE_VALUE)
        REQUIRE_TRUE(CloseHandle(handle) != 0);
    REQUIRE_EQ(handle != INVALID_HANDLE_VALUE, admitted);
    if (!admitted)
        REQUIRE_EQ(error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
}

template <typename T> struct SourceHandleFailingAllocator {
    using value_type = T;
    bool* fail = nullptr;

    // Purpose: Bind one controlled allocation failure. Inputs: A live test flag. Outputs: A non-owning allocator.
    explicit SourceHandleFailingAllocator(bool* flag) noexcept : fail(flag) {}

    // Purpose: Preserve state during allocator rebinding. Inputs: A sibling allocator. Outputs: Shared failure state.
    template <typename U>
    SourceHandleFailingAllocator(const SourceHandleFailingAllocator<U>& other) noexcept : fail(other.fail) {}

    // Purpose: Fail one requested allocation without exhausting memory. Inputs: Element count. Outputs: Storage or
    // bad_alloc.
    T* allocate(std::size_t count) {
        if (std::exchange(*fail, false))
            throw std::bad_alloc{};
        return std::allocator<T>{}.allocate(count);
    }

    // Purpose: Release paired allocator storage. Inputs: Allocation and count. Outputs: Deallocates without throwing.
    void deallocate(T* storage, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(storage, count);
    }

    // Purpose: Compare allocator ownership. Inputs: Another specialization. Outputs: Whether state is shared.
    template <typename U> bool operator==(const SourceHandleFailingAllocator<U>& other) const noexcept {
        return fail == other.fail;
    }
};

}  // namespace

// Purpose: Reject embedded NUL before Windows path resolution can select a truncated source name.
// Inputs: An existing ordinary file followed by a NUL and extra filename text in the native path value.
// Outputs: Fails manifest construction rather than reading the existing prefix file.
TEST_CASE(file_manifest_rejects_nul_before_path_resolution) {
    const auto root = test_temp_dir("manifest-nul");
    const auto source = root / "source.txt";
    std::ofstream(source, std::ios::binary) << "prefix file";
    auto name = source.native();
    name.push_back(L'\0');
    name += L"suffix";
    bool refused = false;
    try {
        static_cast<void>(flitzzip::build_manifest({std::filesystem::path(name)}));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    std::filesystem::remove_all(root);
}

// Purpose: Keep source inventory and reader locks usable beyond the legacy Windows path-length limit.
// Inputs: A long Unicode source passed in both ordinary and extended drive-path representations.
// Outputs: Checks identical names/identity/bytes and refuses a replaced source even at a long pathname.
TEST_CASE(file_manifest_long_paths_preserve_source_identity) {
    const auto root = test_temp_dir("manifest-long-path");
    auto parent = root;
    while (parent.native().size() < 320U)
        parent /= std::wstring(60U, L'p');
    const auto source = parent / L"\u65e5\u672c-\U0001f680.txt";
    const auto source_io = std::filesystem::path(flitzzip::windows_api_path(source));
    std::filesystem::create_directories(source_io.parent_path());
    std::ofstream(source_io, std::ios::binary) << "long source bytes";
    const auto manifest = flitzzip::build_manifest({source});
    const auto extended = flitzzip::build_manifest({source_io});
    REQUIRE_EQ(manifest.entries.size(), 1U);
    REQUIRE_EQ(extended.entries.size(), 1U);
    const auto& entry = manifest.entries.front();
    REQUIRE_EQ(entry.archive_path, flitzzip::path_diagnostic_utf8(source.filename()));
    REQUIRE_EQ(entry.identity.file_id, extended.entries.front().identity.file_id);
    {
        const auto pinned = flitzzip::pin_source_file(source);
        REQUIRE_EQ(pinned.size(), 17U);
        std::ifstream input(pinned.path(), std::ios::binary);
        REQUIRE_TRUE(input.good());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "long source bytes");
        REQUIRE_TRUE(MoveFileExW(source_io.c_str(), (source_io.parent_path() / "moved.txt").c_str(), 0) == 0);
    }
    std::filesystem::rename(source_io, source_io.parent_path() / "original.txt");
    std::ofstream(source_io, std::ios::binary) << "replacement";
    bool refused = false;
    try {
        static_cast<void>(flitzzip::lock_manifest_source(entry));
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    std::filesystem::remove_all(std::filesystem::path(flitzzip::windows_api_path(root)));
}

// Purpose: Preserve a source-open failure instead of replacing it with a locale conversion exception.
// Inputs: A Unicode source held open with an exclusive test-owned handle.
// Outputs: Reports the actual lock failure and UTF-8 path; releases the handle without changing source data.
TEST_CASE(file_manifest_unicode_lock_failure_keeps_original_diagnostic) {
    const auto root = test_temp_dir("manifest-unicode-lock");
    const auto source = root / L"\u65e5\u672c-\U0001f680.txt";
    std::ofstream(source, std::ios::binary) << "source bytes";
    const auto handle =
        CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
    std::string message;
    try {
        static_cast<void>(flitzzip::pin_source_file(source));
    } catch (const flitzzip::SecurityError& error) {
        message = error.what();
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
    REQUIRE_TRUE(message.starts_with("cannot lock source file: "));
    REQUIRE_TRUE(message.find(flitzzip::path_diagnostic_utf8(source)) != std::string::npos);
    REQUIRE_TRUE(message.find("Windows error 32") != std::string::npos);
    std::filesystem::remove_all(root);
}

// Purpose: Verify archive creation refuses Windows junctions inside selected source trees.
// Inputs: A source directory containing a real directory junction to a separate temporary tree.
// Outputs: Throws if the manifest follows the junction and accepts out-of-tree files.
TEST_CASE(file_manifest_rejects_directory_junction_sources) {
    const auto root = test_temp_dir("manifest-reparse-root");
    const auto outside = test_temp_dir("manifest-reparse-outside");
    const auto source = root / "source";
    std::filesystem::create_directories(source);
    std::ofstream(outside / "secret.txt", std::ios::binary) << "outside";

    const auto junction = source / "linked";
    if (!flitzzip_test::try_create_test_directory_junction(junction, outside)) {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        return;
    }

    bool rejected = false;
    try {
        (void)flitzzip::build_manifest({source});
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }

    flitzzip_test::remove_test_directory_junction(junction);
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify a source replaced after manifest construction cannot be reopened by a format writer.
// Inputs: A captured regular file whose pathname is moved aside and replaced with different bytes.
// Outputs: `lock_manifest_source` throws `SecurityError` before any replacement bytes can be read.
TEST_CASE(file_manifest_source_lock_rejects_path_replacement) {
    const auto root = test_temp_dir("manifest-source-replacement");
    const auto source = root / "source.txt";
    std::ofstream(source, std::ios::binary) << "original";
    const auto manifest = flitzzip::build_manifest({source});
    REQUIRE_EQ(manifest.entries.size(), static_cast<std::size_t>(1));

    std::filesystem::rename(source, root / "original.txt");
    std::ofstream(source, std::ios::binary) << "replacement with different identity and size";
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::lock_manifest_source(manifest.entries.front()));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Reject invalid inventory roots and single-file sessions before accepting any source.
// Inputs: Empty source selection, a missing Unicode file and empty/nonempty directories.
// Outputs: Requires specific errors without creating missing paths or changing the valid sibling.
TEST_CASE(file_manifest_rejects_invalid_source_shapes_without_side_effects) {
    const auto root = test_temp_dir("manifest-invalid-shapes");
    const auto source = root / "source.txt";
    const auto missing = root / L"\u65e5\u672c-missing.txt";
    const auto empty = root / "empty";
    const auto nonempty = root / "nonempty";
    std::ofstream(source, std::ios::binary) << "original";
    std::filesystem::create_directory(empty);
    std::filesystem::create_directory(nonempty);
    std::ofstream(nonempty / "child.txt", std::ios::binary) << "child";
    for (const auto& sources :
         {std::vector<std::filesystem::path>{}, std::vector<std::filesystem::path>{source, missing}}) {
        std::string message;
        try {
            static_cast<void>(flitzzip::build_manifest(sources));
        } catch (const flitzzip::ArchiveError& error) {
            message = error.what();
        }
        REQUIRE_TRUE(!message.empty());
        if (sources.empty())
            REQUIRE_EQ(message, "at least one source path is required");
        else
            REQUIRE_TRUE(message.find(flitzzip::path_diagnostic_utf8(missing)) != std::string::npos);
    }
    for (const auto& directory : {empty, nonempty}) {
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::pin_source_file(directory));
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    REQUIRE_TRUE(!std::filesystem::exists(missing));
    require_source_write_access(source, true);
    std::ifstream input(source, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "original");
}

// Purpose: Verify a stale or malformed manifest cannot acquire a writer's trusted source session.
// Inputs: A real file identity with each identity field, availability or entry kind changed independently.
// Outputs: Rejects every forged entry, releases partial locks and still accepts the exact original identity.
TEST_CASE(file_manifest_source_lock_validates_complete_identity_and_releases_failures) {
    const auto root = test_temp_dir("manifest-identity-fields");
    const auto source = root / "source.txt";
    std::ofstream(source, std::ios::binary) << "unchanged source";
    const auto manifest = flitzzip::build_manifest({source});
    const auto& original = manifest.entries.front();
    for (const int field : {0, 1, 2, 3, 4, 5}) {
        auto changed = original;
        switch (field) {
        case 0:
            changed.identity.volume_serial ^= 1U;
            break;
        case 1:
            changed.identity.file_id.back() ^= 1U;
            break;
        case 2:
            ++changed.identity.size;
            break;
        case 3:
            changed.identity.last_write_time ^= 1;
            break;
        case 4:
            changed.identity.available = false;
            break;
        case 5:
            changed.directory = true;
            break;
        }
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::lock_manifest_source(changed));
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        require_source_write_access(source, true);
        {
            const auto lock = flitzzip::lock_manifest_source(original);
            require_source_write_access(source, false);
        }
        require_source_write_access(source, true);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep write/rename protection alive until the final shared source-lock owner is released.
// Inputs: A captured source and two successive copies of its lock across lexical scopes.
// Outputs: Blocks write and parent rename while either owner survives, then releases both protections.
TEST_CASE(file_manifest_source_lock_shared_ownership_pins_until_last_release) {
    const auto root = test_temp_dir("manifest-shared-lock");
    const auto parent = root / "parent";
    std::filesystem::create_directory(parent);
    const auto source = parent / "source.txt";
    std::ofstream(source, std::ios::binary) << "shared source";
    const auto entry = flitzzip::build_manifest({source}).entries.front();
    flitzzip::ManifestSourceLock survivor;
    {
        const auto original = flitzzip::lock_manifest_source(entry);
        survivor = original;
        require_source_write_access(source, false);
    }
    require_source_write_access(source, false);
    const auto moved_parent = root / "moved";
    REQUIRE_TRUE(MoveFileExW(parent.c_str(), moved_parent.c_str(), 0) == 0);
    survivor = {};
    require_source_write_access(source, true);
    REQUIRE_TRUE(MoveFileExW(parent.c_str(), moved_parent.c_str(), 0) != 0);
    std::ifstream input(moved_parent / "source.txt", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "shared source");
}

// Purpose: Transfer pinned sessions without dropping the new file's protection or retaining the old file's lock.
// Inputs: Two files, move construction followed by move assignment over an active session.
// Outputs: Preserves exact path/size/bytes, releases replaced ownership and unlocks everything at scope exit.
TEST_CASE(file_manifest_pinned_session_moves_preserve_exclusive_lifetime) {
    const auto root = test_temp_dir("manifest-moved-session");
    const auto first = root / "first.txt";
    const auto second = root / "second.txt";
    std::ofstream(first, std::ios::binary) << "first";
    std::ofstream(second, std::ios::binary) << "second file";
    {
        auto original = flitzzip::pin_source_file(first);
        auto moved = std::move(original);
        REQUIRE_EQ(moved.size(), 5U);
        require_source_write_access(first, false);
        auto replacement = flitzzip::pin_source_file(second);
        moved = std::move(replacement);
        require_source_write_access(first, true);
        require_source_write_access(second, false);
        REQUIRE_EQ(moved.size(), 11U);
        std::ifstream input(moved.path(), std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "second file");
    }
    require_source_write_access(first, true);
    require_source_write_access(second, true);
    std::filesystem::remove_all(root);
}

// Purpose: Detect changes to the same file object rather than depending only on pathname replacement.
// Inputs: A captured source rewritten at equal length with an explicit timestamp change, or extended in place.
// Outputs: Rejects each stale manifest, accepts a fresh one and exposes the actual new contents.
TEST_CASE(file_manifest_rejects_in_place_source_mutation) {
    for (const bool extend : {false, true}) {
        const auto root = test_temp_dir(extend ? "manifest-grow" : "manifest-same-size");
        const auto source = root / "source.txt";
        std::ofstream(source, std::ios::binary) << "original";
        const auto before = flitzzip::build_manifest({source}).entries.front();
        const std::string replacement = extend ? "replacement is longer" : "replaced";
        std::ofstream(source, std::ios::binary | std::ios::trunc) << replacement;
        const auto handle = CreateFileW(source.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
        ULARGE_INTEGER timestamp{};
        timestamp.QuadPart = static_cast<ULONGLONG>(before.identity.last_write_time) + 20'000'000ULL;
        const FILETIME changed_time{timestamp.LowPart, timestamp.HighPart};
        const auto changed = SetFileTime(handle, nullptr, nullptr, &changed_time);
        REQUIRE_TRUE(CloseHandle(handle) != 0);
        REQUIRE_TRUE(changed != 0);
        const auto after = flitzzip::build_manifest({source}).entries.front();
        REQUIRE_EQ(after.identity.file_id, before.identity.file_id);
        REQUIRE_EQ(after.identity.volume_serial, before.identity.volume_serial);
        REQUIRE_EQ(after.size, replacement.size());
        REQUIRE_TRUE(after.identity.last_write_time != before.identity.last_write_time);
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::lock_manifest_source(before));
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        {
            const auto fresh = flitzzip::lock_manifest_source(after);
            std::ifstream input(source, std::ios::binary);
            REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), replacement);
        }
        std::filesystem::remove_all(root);
    }
}

// Purpose: Prove an acquired source handle cannot leak when its owning container fails to grow.
// Inputs: Real write-excluding handles, the production RAII owner and a controlled first/growth allocation failure.
// Outputs: Closes the unretained handle, preserves existing owners, and releases their locks on destruction.
TEST_CASE(file_manifest_handle_allocation_failure_preserves_ownership) {
    using Owner = flitzzip::windows_detail::HandleOwner;
    for (const bool populated : {false, true}) {
        const auto root = test_temp_dir(populated ? "source-owner-grow" : "source-owner-first");
        const auto source = root / "source.txt";
        const auto retained = root / "retained.txt";
        std::ofstream(source, std::ios::binary) << "new source";
        std::ofstream(retained, std::ios::binary) << "retained source";
        bool fail = false;
        {
            std::vector<Owner, SourceHandleFailingAllocator<Owner>> handles{SourceHandleFailingAllocator<Owner>(&fail)};
            if (populated) {
                Owner first(CreateFileW(retained.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
                REQUIRE_TRUE(first.get() != INVALID_HANDLE_VALUE);
                handles.push_back(std::move(first));
                handles.resize(handles.capacity());
                require_source_write_access(retained, false);
            }
            const auto previous_size = handles.size();
            const auto raw = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
            REQUIRE_TRUE(raw != INVALID_HANDLE_VALUE);
            bool rejected = false;
            try {
                Owner acquired(raw);
                fail = true;
                handles.push_back(std::move(acquired));
            } catch (const std::bad_alloc&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_TRUE(!fail);
            REQUIRE_EQ(handles.size(), previous_size);
            DWORD flags = 0;
            REQUIRE_TRUE(GetHandleInformation(raw, &flags) == 0);
            REQUIRE_EQ(GetLastError(), static_cast<DWORD>(ERROR_INVALID_HANDLE));
            require_source_write_access(source, true);
            require_source_write_access(retained, !populated);
        }
        require_source_write_access(retained, true);
        std::filesystem::remove_all(root);
    }
}
