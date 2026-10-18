#include "app/folder_size.hpp"
#include "app/operation_destination.hpp"
#include "test_util.hpp"

#include <aclapi.h>
#include <array>
#include <fstream>
#include <memory>

using flitzzip::app::FolderSizeLimits;
using flitzzip::app::measure_folder_size;

namespace {

struct FreeLocal {
    // Purpose: Release a Windows local allocation; inputs: owned buffer; outputs: freed storage.
    void operator()(void* value) const noexcept {
        if (value != nullptr) {
            (void)LocalFree(value);
        }
    }
};

class DeniedDirectory {
  public:
    // Purpose: Deny enumeration only on an owned fixture, retaining its exact previous ACL.
    // Inputs: An existing test directory; outputs: scoped deny rule or setup failure before use.
    explicit DeniedDirectory(const std::filesystem::path& path) : path_(path.native()) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const auto read = GetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                                nullptr, &previous_acl_, nullptr, &descriptor);
        previous_descriptor_.reset(descriptor);
        REQUIRE_EQ(read, ERROR_SUCCESS);
        alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> everyone{};
        DWORD sid_size = static_cast<DWORD>(everyone.size());
        REQUIRE_TRUE(CreateWellKnownSid(WinWorldSid, nullptr, everyone.data(), &sid_size) != FALSE);
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_LIST_DIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        BuildTrusteeWithSidW(&deny.Trustee, everyone.data());
        PACL changed = nullptr;
        const auto built = SetEntriesInAclW(1, &deny, previous_acl_, &changed);
        std::unique_ptr<void, FreeLocal> changed_owner(changed);
        REQUIRE_EQ(built, ERROR_SUCCESS);
        REQUIRE_EQ(SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         changed, nullptr),
                   ERROR_SUCCESS);
        active_ = true;
    }

    DeniedDirectory(const DeniedDirectory&) = delete;
    DeniedDirectory& operator=(const DeniedDirectory&) = delete;

    // Purpose: Restore the fixture ACL on every test exit; inputs: owned snapshot; outputs: restored access.
    ~DeniedDirectory() {
        if (active_) {
            (void)SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                        previous_acl_, nullptr);
        }
    }

    // Purpose: Check restoration before filesystem cleanup; inputs: retained ACL; outputs: restores or fails test.
    void restore() {
        REQUIRE_EQ(SetNamedSecurityInfoW(path_.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         previous_acl_, nullptr),
                   ERROR_SUCCESS);
        active_ = false;
    }

  private:
    std::wstring path_;
    std::unique_ptr<void, FreeLocal> previous_descriptor_;
    PACL previous_acl_ = nullptr;
    bool active_ = false;
};

// Purpose: Write an independently sized small fixture; inputs: path and payload; outputs: exact closed file.
void write_folder_fixture(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE_TRUE(stream.good());
}

}  // namespace

TEST_CASE(folder_size_reports_complete_files_and_rejects_inaccessible_subtrees) {
    const auto root = flitzzip::windows_regular_path(test_temp_dir("folder-size-permissions"));
    const auto child = root / "child";
    std::filesystem::create_directory(child);
    write_folder_fixture(root / "visible", "1234567");
    write_folder_fixture(child / "hidden", "12345678901");
    const std::atomic_bool running = false;
    REQUIRE_EQ(measure_folder_size(root, running, running), std::optional<std::uintmax_t>(18));
    {
        DeniedDirectory denied(child);
        std::error_code ec;
        const std::filesystem::directory_iterator inaccessible(child, ec);
        REQUIRE_TRUE(ec == std::errc::permission_denied);
        REQUIRE_TRUE(inaccessible == std::filesystem::directory_iterator{});
        REQUIRE_TRUE(!measure_folder_size(root, running, running));
        REQUIRE_TRUE(!measure_folder_size(child, running, running));
        REQUIRE_TRUE(flitzzip::app::extraction_destination_needs_confirmation(child));
        denied.restore();
    }
    REQUIRE_EQ(measure_folder_size(root, running, running), std::optional<std::uintmax_t>(18));
    std::filesystem::remove_all(root);
}

TEST_CASE(folder_size_validates_roots_and_both_cancellation_signals) {
    const auto root = flitzzip::windows_regular_path(test_temp_dir("folder-size-roots"));
    const auto file = root / "plain";
    const std::atomic_bool running = false;
    const std::atomic_bool cancelled = true;
    REQUIRE_EQ(measure_folder_size(root, running, running), std::optional<std::uintmax_t>(0));
    write_folder_fixture(file, "data");
    REQUIRE_TRUE(!measure_folder_size(file, running, running));
    REQUIRE_TRUE(!measure_folder_size(root / "absent", running, running));
    REQUIRE_TRUE(!measure_folder_size("relative-input", running, running));
    REQUIRE_TRUE(!measure_folder_size(flitzzip::windows_api_path(root), running, running));
    REQUIRE_TRUE(!measure_folder_size(root, cancelled, running));
    REQUIRE_TRUE(!measure_folder_size(root, running, cancelled));
    REQUIRE_EQ(measure_folder_size(root, running, running), std::optional<std::uintmax_t>(4));
    std::filesystem::remove_all(root);
}

TEST_CASE(folder_size_never_returns_partial_bytes_when_a_work_budget_expires) {
    const auto root = flitzzip::windows_regular_path(test_temp_dir("folder-size-limits"));
    const auto child = root / "child";
    std::filesystem::create_directory(child);
    write_folder_fixture(root / "top", "123");
    write_folder_fixture(child / "nested", "12345");
    const std::atomic_bool running = false;
    FolderSizeLimits limits;
    limits.entries = 2;
    REQUIRE_TRUE(!measure_folder_size(root, running, running, limits));
    limits.entries = 3;
    limits.depth = 1;
    REQUIRE_TRUE(!measure_folder_size(root, running, running, limits));
    limits.depth = 2;
    REQUIRE_EQ(measure_folder_size(root, running, running, limits), std::optional<std::uintmax_t>(8));
    limits.duration = std::chrono::steady_clock::duration::zero();
    REQUIRE_TRUE(!measure_folder_size(root, running, running, limits));
    std::filesystem::remove_all(root);
}

TEST_CASE(folder_size_excludes_reparse_targets_and_rejects_reparse_roots) {
    const auto root = flitzzip::windows_regular_path(test_temp_dir("folder-size-reparse"));
    const auto external = root / "external";
    const auto measured = root / "measured";
    const auto junction = measured / "junction";
    std::filesystem::create_directories(external);
    std::filesystem::create_directory(measured);
    write_folder_fixture(external / "other", "12345678901");
    write_folder_fixture(measured / "local", "1234567");
    REQUIRE_TRUE(flitzzip_test::try_create_test_directory_junction(junction, external));
    const std::atomic_bool running = false;
    try {
        REQUIRE_TRUE(!measure_folder_size(junction, running, running));
        REQUIRE_EQ(measure_folder_size(measured, running, running), std::optional<std::uintmax_t>(7));
    } catch (...) {
        flitzzip_test::remove_test_directory_junction(junction);
        throw;
    }
    flitzzip_test::remove_test_directory_junction(junction);
    REQUIRE_TRUE(std::filesystem::exists(external / "other"));
    std::filesystem::remove_all(root);
}
