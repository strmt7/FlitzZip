#include "core/file_manifest.hpp"
#include "core/path_text.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

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
        static_cast<void>(superzip::build_manifest({std::filesystem::path(name)}));
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
    const auto source_io = std::filesystem::path(superzip::windows_api_path(source));
    std::filesystem::create_directories(source_io.parent_path());
    std::ofstream(source_io, std::ios::binary) << "long source bytes";
    const auto manifest = superzip::build_manifest({source});
    const auto extended = superzip::build_manifest({source_io});
    REQUIRE_EQ(manifest.entries.size(), 1U);
    REQUIRE_EQ(extended.entries.size(), 1U);
    const auto& entry = manifest.entries.front();
    REQUIRE_EQ(entry.archive_path, superzip::path_diagnostic_utf8(source.filename()));
    REQUIRE_EQ(entry.identity.file_id, extended.entries.front().identity.file_id);
    {
        const auto pinned = superzip::pin_source_file(source);
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
        static_cast<void>(superzip::lock_manifest_source(entry));
    } catch (const superzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    std::filesystem::remove_all(std::filesystem::path(superzip::windows_api_path(root)));
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
        static_cast<void>(superzip::pin_source_file(source));
    } catch (const superzip::SecurityError& error) {
        message = error.what();
    } catch (...) {
        CloseHandle(handle);
        throw;
    }
    CloseHandle(handle);
    REQUIRE_TRUE(message.starts_with("cannot lock source file: "));
    REQUIRE_TRUE(message.find(superzip::path_diagnostic_utf8(source)) != std::string::npos);
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
    if (!superzip_test::try_create_test_directory_junction(junction, outside)) {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        return;
    }

    bool rejected = false;
    try {
        (void)superzip::build_manifest({source});
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }

    superzip_test::remove_test_directory_junction(junction);
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
    const auto manifest = superzip::build_manifest({source});
    REQUIRE_EQ(manifest.entries.size(), static_cast<std::size_t>(1));

    std::filesystem::rename(source, root / "original.txt");
    std::ofstream(source, std::ios::binary) << "replacement with different identity and size";
    bool rejected = false;
    try {
        static_cast<void>(superzip::lock_manifest_source(manifest.entries.front()));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}
