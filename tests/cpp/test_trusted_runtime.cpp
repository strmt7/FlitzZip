#include "test_util.hpp"

#include "core/integrity.hpp"
#include "core/result.hpp"
#include "core/runtime_loader.hpp"
#include "core/trusted_runtime.hpp"

#include <array>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
namespace {

// Purpose: Keep deliberate image-loader failures headless without changing another thread or process policy.
// Inputs: The test thread's current error mode; the owner must remain on that same thread.
// Outputs: Restores the exact preceding mode when native loader testing finishes, including exception unwinding.
class LoaderErrorMode final {
  public:
    // Purpose: Suppress owned loader dialogs; inputs: current thread; outputs: retained preceding error mode.
    LoaderErrorMode() {
        const auto flags = GetThreadErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX;
        REQUIRE_TRUE(SetThreadErrorMode(flags, &previous_) != 0);
    }
    LoaderErrorMode(const LoaderErrorMode&) = delete;
    LoaderErrorMode& operator=(const LoaderErrorMode&) = delete;

    // Purpose: Restore thread policy; inputs: retained error mode; outputs: original mode without throwing.
    ~LoaderErrorMode() {
        static_cast<void>(SetThreadErrorMode(previous_, nullptr));
    }

  private:
    DWORD previous_ = 0;
};

// Purpose: Own a tiny invalid app-local module without replacing any installed runtime or foreign fixture.
// Inputs: A test-specific suffix; the running test executable's directory must permit exclusive fixture creation.
// Outputs: Creates exactly one new file and removes only that file after module and source owners have released it.
class RuntimeLoadFixture final {
  public:
    // Purpose: Reserve invalid DLL bytes; inputs: unique case suffix; outputs: an owned CREATE_NEW file or refusal.
    explicit RuntimeLoadFixture(std::wstring_view suffix) {
        std::array<wchar_t, 32768> executable{};
        const auto count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        REQUIRE_TRUE(count > 0U && count < executable.size());
        const auto parent = std::filesystem::path(std::wstring_view(executable.data(), count)).parent_path();
        path_ = parent / (L"flitzzip-runtime-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::wstring(suffix) + L".dll");
        const auto handle =
            CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
        constexpr std::array<unsigned char, 8> invalid_image{0x46, 0x5a, 0x00, 0xff, 0x12, 0x34, 0x56, 0x78};
        DWORD written = 0;
        const bool complete =
            WriteFile(handle, invalid_image.data(), static_cast<DWORD>(invalid_image.size()), &written, nullptr) != 0 &&
            written == invalid_image.size();
        const bool closed = CloseHandle(handle) != 0;
        if (!complete || !closed) {
            static_cast<void>(DeleteFileW(path_.c_str()));
            throw std::runtime_error("runtime fixture write failed");
        }
    }
    RuntimeLoadFixture(const RuntimeLoadFixture&) = delete;
    RuntimeLoadFixture& operator=(const RuntimeLoadFixture&) = delete;

    // Purpose: Remove only this fixture; inputs: released module/source owners; outputs: best-effort file cleanup.
    ~RuntimeLoadFixture() {
        static_cast<void>(DeleteFileW(path_.c_str()));
    }

    // Purpose: Borrow fixture identity; inputs: none; outputs: the exact app-local path without transferring ownership.
    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    // Purpose: Check independent OS lock state without modifying bytes; inputs: none; outputs: sharing error or zero.
    DWORD exclusive_write_error() const {
        const auto handle = CreateFileW(path_.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return GetLastError();
        REQUIRE_TRUE(CloseHandle(handle) != 0);
        return ERROR_SUCCESS;
    }

  private:
    std::filesystem::path path_;
};

enum class LoaderFault {
    None,
    ExecutableMissing,
    ExecutableTruncated,
    LoadDenied,
    ModuleMissing,
    ModuleTruncated,
    DifferentObject,
    MissingObject
};

// Purpose: Exercise loader failures while retaining real hashing, source pins and successful OS module references.
// Inputs: One selected fault and precomputed replacement paths; no hooks affect production or another test thread.
// Outputs: Records exact loads/releases and native search flags; nonfailed operations use the production adapter.
class ControlledRuntimeLoader final : public flitzzip::runtime_detail::RuntimeLoader {
  public:
    LoaderFault fault = LoaderFault::None;
    std::wstring different_path;
    std::wstring missing_path;
    mutable unsigned loads = 0;
    mutable unsigned releases = 0;
    mutable std::uint32_t search_flags = 0;
    mutable void* loaded_module = nullptr;
    mutable bool release_valid = true;

    // Purpose: Vary only the documented path-query outcome; inputs: real module and buffer; outputs: Win32 semantics.
    std::uint32_t module_file_name(void* module, wchar_t* buffer, std::uint32_t capacity) const noexcept override {
        if ((!module && fault == LoaderFault::ExecutableMissing) || (module && fault == LoaderFault::ModuleMissing)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return 0;
        }
        if ((!module && fault == LoaderFault::ExecutableTruncated) ||
            (module && fault == LoaderFault::ModuleTruncated)) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return capacity;
        }
        if (module && (fault == LoaderFault::DifferentObject || fault == LoaderFault::MissingObject)) {
            const auto& path = fault == LoaderFault::DifferentObject ? different_path : missing_path;
            if (path.size() >= capacity) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return capacity;
            }
            std::copy(path.begin(), path.end(), buffer);
            buffer[path.size()] = L'\0';
            return static_cast<std::uint32_t>(path.size());
        }
        return native_.module_file_name(module, buffer, capacity);
    }

    // Purpose: Deny one native load or retain its real reference; inputs: pinned path/flags; outputs: load record.
    void* load_library(const wchar_t* path, std::uint32_t flags) const noexcept override {
        ++loads;
        search_flags = flags;
        if (fault == LoaderFault::LoadDenied) {
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        loaded_module = native_.load_library(path, flags);
        return loaded_module;
    }

    // Purpose: Observe failed-validation cleanup; inputs: just-loaded reference; outputs: actual release and record.
    void free_library(void* module) const noexcept override {
        ++releases;
        release_valid = release_valid && module == loaded_module;
        native_.free_library(module);
    }

  private:
    flitzzip::runtime_detail::NativeRuntimeLoader native_;
};

}  // namespace

// Purpose: Validate every documented loader failure and cleanup boundary without resource exhaustion or API hooks.
// Inputs: Per-instance path/load failures over one private real DLL, including identical bytes at another identity.
// Outputs: Preserves native diagnostics, releases rejected modules/pins, and successfully reuses the same path.
TEST_CASE(trusted_runtime_controlled_loader_failures_preserve_ownership_and_recovery) {
    const LoaderErrorMode error_mode;
    RuntimeLoadFixture fixture(L"loader-boundary");
    const auto original = fixture.path().parent_path() / L"libzstd.dll";
    REQUIRE_TRUE(
        std::filesystem::copy_file(original, fixture.path(), std::filesystem::copy_options::overwrite_existing));
    const auto digest = flitzzip::hash_file(fixture.path(), flitzzip::IntegrityMode::Sha256).hex_digest;
    REQUIRE_EQ(flitzzip::hash_file(original, flitzzip::IntegrityMode::Sha256).hex_digest, digest);
    REQUIRE_TRUE(!std::filesystem::equivalent(original, fixture.path()));
    struct Case {
        LoaderFault fault;
        std::string_view message;
        bool security;
        unsigned loads;
        unsigned releases;
    };
    constexpr std::array<Case, 7> cases{{
        {LoaderFault::ExecutableMissing, "cannot locate FlitzZip executable directory: Windows error 5", false, 0, 0},
        {LoaderFault::ExecutableTruncated, "FlitzZip executable path exceeds", false, 0, 0},
        {LoaderFault::LoadDenied, "cannot load verified bundled runtime ", false, 1, 0},
        {LoaderFault::ModuleMissing, "cannot resolve loaded runtime path: Windows error 5", false, 1, 1},
        {LoaderFault::ModuleTruncated, "loaded runtime path exceeds", false, 1, 1},
        {LoaderFault::DifferentObject, "Windows loaded a different object", true, 1, 1},
        {LoaderFault::MissingObject, "Windows loaded a different object", true, 1, 1},
    }};
    for (const auto& entry : cases) {
        ControlledRuntimeLoader loader;
        loader.fault = entry.fault;
        loader.different_path = original.native();
        loader.missing_path = fixture.path().native() + L".missing";
        REQUIRE_TRUE(!std::filesystem::exists(loader.missing_path));
        bool rejected = false;
        try {
            static_cast<void>(
                flitzzip::runtime_detail::load_trusted_runtime(fixture.path().filename().native(), digest, loader));
        } catch (const flitzzip::SecurityError& error) {
            rejected = entry.security && std::string_view(error.what()).starts_with(entry.message);
        } catch (const flitzzip::ArchiveError& error) {
            const std::string_view message(error.what());
            rejected = !entry.security && message.starts_with(entry.message);
            if (entry.fault == LoaderFault::LoadDenied)
                rejected = rejected && message.ends_with("Windows error 5");
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(loader.loads, entry.loads);
        REQUIRE_EQ(loader.releases, entry.releases);
        REQUIRE_TRUE(loader.release_valid);
        if (entry.loads)
            REQUIRE_EQ(loader.search_flags, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        REQUIRE_EQ(GetModuleHandleW(fixture.path().c_str()), nullptr);
        REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SUCCESS);
        loader.fault = LoaderFault::None;
        {
            const auto recovered =
                flitzzip::runtime_detail::load_trusted_runtime(fixture.path().filename().native(), digest, loader);
            REQUIRE_TRUE(GetProcAddress(static_cast<HMODULE>(recovered.native_handle()), "ZSTD_versionNumber") !=
                         nullptr);
            REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SHARING_VIOLATION);
        }
        REQUIRE_EQ(GetModuleHandleW(fixture.path().c_str()), nullptr);
        REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SUCCESS);
    }
    REQUIRE_EQ(flitzzip::hash_file(fixture.path(), flitzzip::IntegrityMode::Sha256).hex_digest, digest);
}

// Purpose: Keep a failed native load from retaining a source lock, then prove valid reuse of the same pathname.
// Inputs: Correctly hashed invalid Unicode-named image bytes followed by a private copy of the shipped Zstandard DLL.
// Outputs: Rejects at native loading, preserves the invalid file, releases its locks and loads the replacement.
TEST_CASE(trusted_runtime_native_load_failure_releases_source_and_recovers) {
    const LoaderErrorMode error_mode;
    RuntimeLoadFixture fixture(L"invalid-image-\u65e5\u672c-\U0001F4C1");
    const auto before = flitzzip::hash_file(fixture.path(), flitzzip::IntegrityMode::Sha256);
    auto wrong_digest = before.hex_digest;
    wrong_digest.front() = wrong_digest.front() == '0' ? '1' : '0';
    bool mismatched = false;
    try {
        static_cast<void>(flitzzip::load_trusted_app_local_runtime(fixture.path().filename().native(), wrong_digest));
    } catch (const flitzzip::SecurityError& error) {
        REQUIRE_EQ(std::string(error.what()), "bundled runtime SHA-256 verification failed: " +
                                                  flitzzip::path_diagnostic_utf8(fixture.path().filename()));
        mismatched = true;
    }
    REQUIRE_TRUE(mismatched);
    REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SUCCESS);
    bool rejected = false;
    try {
        static_cast<void>(
            flitzzip::load_trusted_app_local_runtime(fixture.path().filename().native(), before.hex_digest));
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string_view(error.what())
                       .starts_with("cannot load verified bundled runtime " +
                                    flitzzip::path_diagnostic_utf8(fixture.path().filename()) + ": ");
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(flitzzip::hash_file(fixture.path(), flitzzip::IntegrityMode::Sha256).hex_digest, before.hex_digest);
    REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SUCCESS);
    REQUIRE_TRUE(std::filesystem::copy_file(fixture.path().parent_path() / L"libzstd.dll", fixture.path(),
                                            std::filesystem::copy_options::overwrite_existing));
    const auto digest = flitzzip::hash_file(fixture.path(), flitzzip::IntegrityMode::Sha256).hex_digest;
    {
        const auto module = flitzzip::load_trusted_app_local_runtime(fixture.path().filename().native(), digest);
        REQUIRE_TRUE(module.native_handle() != nullptr);
        REQUIRE_TRUE(GetProcAddress(static_cast<HMODULE>(module.native_handle()), "ZSTD_versionNumber") != nullptr);
        REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SHARING_VIOLATION);
    }
    REQUIRE_EQ(GetModuleHandleW(fixture.path().c_str()), nullptr);
    REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SUCCESS);
}

// Purpose: Keep verified bytes and the native module alive after moving ownership beyond the original scope.
// Inputs: One app-local Zstandard copy and an optional owner receiving the production move constructor.
// Outputs: Leaves the moved-from owner empty, retains a usable pinned module, then unloads and releases its locks.
TEST_CASE(trusted_runtime_move_keeps_module_and_file_lock_together) {
    RuntimeLoadFixture fixture(L"moved-owner");
    REQUIRE_TRUE(std::filesystem::copy_file(fixture.path().parent_path() / L"libzstd.dll", fixture.path(),
                                            std::filesystem::copy_options::overwrite_existing));
    auto digest = flitzzip::hash_file(fixture.path(), flitzzip::IntegrityMode::Sha256).hex_digest;
    for (auto& character : digest)
        if (character >= 'a' && character <= 'f')
            character = static_cast<char>(character - 'a' + 'A');
    std::optional<flitzzip::TrustedRuntimeModule> retained;
    void* expected_handle = nullptr;
    {
        auto original = flitzzip::load_trusted_app_local_runtime(fixture.path().filename().native(), digest);
        expected_handle = original.native_handle();
        retained.emplace(std::move(original));
        REQUIRE_EQ(original.native_handle(), nullptr);
        REQUIRE_EQ(retained->native_handle(), expected_handle);
    }
    REQUIRE_TRUE(std::filesystem::equivalent(retained->path(), fixture.path()));
    const auto version = reinterpret_cast<unsigned (*)()>(
        GetProcAddress(static_cast<HMODULE>(retained->native_handle()), "ZSTD_versionNumber"));
    REQUIRE_TRUE(version != nullptr);
    REQUIRE_TRUE(version() > 0U);
    REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SHARING_VIOLATION);
    retained.reset();
    REQUIRE_EQ(GetModuleHandleW(fixture.path().c_str()), nullptr);
    REQUIRE_EQ(fixture.exclusive_write_error(), ERROR_SUCCESS);
}

// Purpose: Reject ambiguous Windows module identities before opening any file.
// Inputs: Empty, rooted, drive-relative, stream and NUL-bearing names with a valid digest shape.
// Outputs: Requires the filename-admission error rather than filesystem or loader diagnostics.
TEST_CASE(trusted_runtime_rejects_ambiguous_module_names_before_io) {
    std::wstring embedded = L"libzstd.dll";
    embedded.push_back(L'\0');
    embedded += L"other.dll";
    const std::array<std::wstring, 9> names{
        L"",
        L".",
        L"..",

        L"folder/libzstd.dll",
        L"C:libzstd.dll",
        L"C:\\libzstd.dll",
        L"\\\\server\\share\\libzstd.dll",
        L"libzstd.dll:stream",
        embedded,
    };
    for (const auto& name : names) {
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::load_trusted_app_local_runtime(name, std::string(64U, '0')));
        } catch (const flitzzip::ArchiveError& error) {
            rejected = std::string_view(error.what()) == "bundled runtime name must be a single filename";
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Reject malformed provenance before attempting to locate or open a native runtime.
// Inputs: Invalid digest lengths, nonhexadecimal bytes and embedded NUL, with an absent module filename.
// Outputs: Requires the metadata error rather than a filesystem or Windows loader failure.
TEST_CASE(trusted_runtime_rejects_invalid_provenance_before_io) {
    const std::array<std::string, 6> digests{
        "",
        std::string(63U, '0'),
        std::string(65U, '0'),
        std::string(64U, 'g'),
        std::string(64U, '\0'),
        std::string(64U, static_cast<char>(0xff)),
    };
    for (const auto& digest : digests) {
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::load_trusted_app_local_runtime(L"flitzzip-absent-runtime-test.dll", digest));
        } catch (const flitzzip::ArchiveError& error) {
            rejected = std::string_view(error.what()) == "bundled runtime has an invalid pinned SHA-256 value";
        }
        REQUIRE_TRUE(rejected);
    }
}
#endif
