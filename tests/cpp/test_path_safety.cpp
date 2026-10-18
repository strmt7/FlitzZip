#include "core/file_publish.hpp"
#include "core/windows_directory.hpp"
#include <memory>
#include "core/path_safety.hpp"
#include "core/stream_archive_path.hpp"
#include "core/file_size.hpp"
#include "core/path_text.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#include <algorithm>
#include <array>

namespace {

// Purpose: Snapshot Windows file identity without retaining a handle that could prevent publication.
// Inputs: `path` names an ordinary test payload on the local test volume.
// Outputs: Returns its volume and file identifier; fails the test if the snapshot cannot be read.
BY_HANDLE_FILE_INFORMATION publication_file_identity(const std::filesystem::path& path) {
    const auto handle =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    REQUIRE_TRUE(handle != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION identity{};
    const auto queried = GetFileInformationByHandle(handle, &identity);
    CloseHandle(handle);
    REQUIRE_TRUE(queried != 0);
    return identity;
}

}  // namespace

// Purpose: Keep test filesystem I/O independent of the temporary root's length, Unicode text and prefix form.
// Inputs: A nested Unicode parent beyond MAX_PATH in ordinary and extended Windows representations.
// Outputs: Creates, writes, reads and recreates the same owned directory without changing host configuration.
TEST_CASE(test_temporary_directories_support_long_unicode_roots) {
    const auto root = test_temp_dir("temporary-path-contract");
    auto parent = flitzzip::windows_regular_path(root) / L"temporary \u65e5\u672c \U0001f680";
    while (parent.native().size() < 320U)
        parent /= std::wstring(60U, L'p');
    for (const auto& candidate : {parent, std::filesystem::path(flitzzip::windows_api_path(parent))}) {
        const auto directory = test_temp_dir("nested", candidate);
        REQUIRE_TRUE(directory.native().starts_with(L"\\\\?\\"));
        const auto file = directory / L"payload \u03a9.txt";
        {
            std::ofstream output(file, std::ios::binary);
            REQUIRE_TRUE(output.is_open());
            output << "exact payload";
            output.close();
            REQUIRE_TRUE(!output.fail());
        }
        {
            std::ifstream input(file, std::ios::binary);
            REQUIRE_TRUE(input.is_open());
            const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            REQUIRE_EQ(bytes, "exact payload");
        }
        REQUIRE_EQ(test_temp_dir("nested", candidate), directory);
        REQUIRE_TRUE(std::filesystem::is_empty(directory));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Resolve long extraction-root aliases identically without relaxing archive-entry containment.
// Inputs: One real Unicode destination in ordinary, extended, trailing-separator and dot-component forms.
// Outputs: Every alias reaches the same payload; traversal and absolute archive entries remain rejected.
TEST_CASE(path_safety_long_destination_aliases_preserve_containment) {
    const auto root = test_temp_dir("destination-aliases");
    auto parent = flitzzip::windows_regular_path(root) / L"destination \u65e5\u672c";
    while (parent.native().size() < 320U)
        parent /= std::wstring(60U, L'p');
    const auto io_parent = std::filesystem::path(flitzzip::windows_api_path(parent));
    std::filesystem::create_directories(io_parent / "inside");
    {
        std::ofstream payload(io_parent / "inside" / "file.txt", std::ios::binary);
        REQUIRE_TRUE(payload.is_open());
        payload << "unchanged";
        payload.close();
        REQUIRE_TRUE(!payload.fail());
    }
    for (const auto& alias : {parent, parent / ".", std::filesystem::path(parent.native() + L"\\"), io_parent,
                              io_parent / ".", std::filesystem::path(io_parent.native() + L"\\")}) {
        const auto target = flitzzip::safe_join_archive_path(alias, "inside/file.txt");
        REQUIRE_TRUE(std::filesystem::equivalent(std::filesystem::path(flitzzip::windows_api_path(target)),
                                                 io_parent / "inside" / "file.txt"));
        for (const auto* name : {"../outside.txt", "inside/../../outside.txt", "/absolute.txt", "C:/absolute.txt"}) {
            bool rejected = false;
            try {
                (void)flitzzip::safe_join_archive_path(alias, name);
            } catch (const flitzzip::SecurityError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep Win32 path conversion independent of long-path registry settings and locale.
// Inputs: Normal drive, UNC, already-extended, relative-component, and device-namespace paths.
// Outputs: Preserves filesystem meaning with extended syntax and rejects non-filesystem namespaces.
TEST_CASE(windows_api_path_normalizes_filesystem_roots) {
    REQUIRE_EQ(flitzzip::windows_api_path(L"C:/root/one/../file.txt"), L"\\\\?\\C:\\root\\file.txt");
    REQUIRE_EQ(flitzzip::windows_api_path(L"\\\\server\\share\\file.txt"), L"\\\\?\\UNC\\server\\share\\file.txt");
    REQUIRE_EQ(flitzzip::windows_api_path(L"\\\\?\\C:\\root\\file.txt"), L"\\\\?\\C:\\root\\file.txt");
    REQUIRE_EQ(flitzzip::windows_api_path(L"\\\\?\\C:\\root\\nested/file.txt"), L"\\\\?\\C:\\root\\nested\\file.txt");
    REQUIRE_EQ(flitzzip::windows_api_path(L"\\\\?\\UNC\\server\\share\\file.txt"),
               L"\\\\?\\UNC\\server\\share\\file.txt");
    REQUIRE_EQ(flitzzip::windows_regular_path(L"\\\\?\\C:\\root\\file.txt"), L"C:\\root\\file.txt");
    REQUIRE_EQ(flitzzip::windows_regular_path(L"\\\\?\\UNC\\server\\share\\file.txt"), L"\\\\server\\share\\file.txt");
    REQUIRE_EQ(flitzzip::windows_regular_path(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1"),
               L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1");
    for (const auto* path : {L"\\\\.\\PhysicalDrive0", L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1"}) {
        bool rejected = false;
        try {
            static_cast<void>(flitzzip::windows_api_path(path));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Reject path representations that Win32 cannot consume as one complete filesystem pathname.
// Inputs: Embedded-NUL and oversized absolute paths without any filesystem writes.
// Outputs: Requires explicit rejection rather than truncation or reliance on host long-path configuration.
TEST_CASE(windows_api_path_rejects_unrepresentable_paths) {
    auto embedded_nul = std::wstring(L"C:\\root\\file");
    embedded_nul.push_back(L'\0');
    embedded_nul += L"hidden.txt";
    for (const auto& path : {embedded_nul, L"C:\\" + std::wstring(32764U, L'a')}) {
        bool refused = false;
        try {
            static_cast<void>(flitzzip::windows_api_path(std::filesystem::path(path)));
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
    }
}

// Purpose: Reject embedded NUL before publication reserves anything at the truncated prefix pathname.
// Inputs: A missing target followed by embedded NUL and suffix text in the native path value.
// Outputs: Rejects file and directory transactions while leaving the selected parent empty.
TEST_CASE(file_publish_rejects_nul_before_path_resolution) {
    const auto root = test_temp_dir("publish-nul");
    auto name = (root / "target").native();
    name.push_back(L'\0');
    name += L"suffix";
    for (const bool directory : {false, true}) {
        bool refused = false;
        try {
            if (directory) {
                const flitzzip::DirectoryPublishTransaction transaction{std::filesystem::path(name)};
            } else {
                const flitzzip::FilePublishTransaction transaction{std::filesystem::path(name)};
            }
        } catch (const std::invalid_argument&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
        REQUIRE_TRUE(std::filesystem::is_empty(root));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve exact payload identity and reservation cleanup beyond the legacy Windows path limit.
// Inputs: Long Unicode destinations and normal, overwritten, refused, and abandoned private file transactions.
// Outputs: Publishes the original file identity with exact bytes and leaves no temporary publication directories.
TEST_CASE(file_publish_long_paths_preserve_identity_and_cleanup) {
    const auto root = test_temp_dir("publish-long-path");
    auto parent = root;
    while (parent.native().size() < 320U)
        parent /= std::wstring(60U, L'p');
    const auto target = parent / L"\u65e5\u672c-\U0001f680.txt";
    const auto target_io = std::filesystem::path(flitzzip::windows_api_path(target));
    for (const bool overwrite : {false, true}) {
        flitzzip::FilePublishTransaction transaction(target);
        const auto stage = transaction.staging_path();
        REQUIRE_TRUE(stage.native().size() > 320U);
        std::ofstream(stage, std::ios::binary) << (overwrite ? "replacement" : "original");
        const auto before = publication_file_identity(stage);
        transaction.commit(overwrite);
        const auto after = publication_file_identity(target_io);
        REQUIRE_EQ(before.dwVolumeSerialNumber, after.dwVolumeSerialNumber);
        REQUIRE_EQ(before.nFileIndexHigh, after.nFileIndexHigh);
        REQUIRE_EQ(before.nFileIndexLow, after.nFileIndexLow);
        REQUIRE_TRUE(!std::filesystem::exists(stage.parent_path()));
        std::ifstream input(target_io, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), overwrite ? "replacement" : "original");
    }
    for (const bool commit : {false, true}) {
        std::filesystem::path stage;
        bool refused = false;
        {
            flitzzip::FilePublishTransaction transaction(target);
            stage = transaction.staging_path();
            std::ofstream(stage, std::ios::binary) << "unpublished";
            if (commit) {
                try {
                    transaction.commit(false);
                } catch (const flitzzip::SecurityError&) {
                    refused = true;
                }
            }
        }
        REQUIRE_EQ(refused, commit);
        REQUIRE_TRUE(!std::filesystem::exists(stage.parent_path()));
        std::ifstream input(target_io, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "replacement");
    }
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(target_io.parent_path()),
                             std::filesystem::directory_iterator{}),
               1);
    std::filesystem::remove_all(std::filesystem::path(flitzzip::windows_api_path(root)));
}

// Purpose: Prove private publication moves the original file rather than rewriting its payload.
// Inputs: Nested nonempty and empty Unicode staged files, existing destinations, and a fixed write timestamp.
// Outputs: Requires exact file identity, timestamp, and contents after publication, with private staging removed.
TEST_CASE(directory_publish_moves_payload_without_copying) {
    const auto root = test_temp_dir("publish-identity");
    for (const bool overwrite : {false, true}) {
        const auto destination = root / (overwrite ? "replace" : "new");
        flitzzip::DirectoryPublishTransaction transaction(destination);
        const auto stage = transaction.staging_directory();
        const auto relative = std::filesystem::path(u8"\u65e5\u672c/caf\u00e9.txt").lexically_normal();
        std::filesystem::create_directories((stage / relative).parent_path());
        std::ofstream(stage / relative, std::ios::binary) << "exact published bytes";
        std::ofstream(stage / "empty.txt", std::ios::binary).close();
        const auto timestamp = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
        std::filesystem::last_write_time(stage / relative, timestamp);
        const auto before = publication_file_identity(stage / relative);
        const auto empty_before = publication_file_identity(stage / "empty.txt");
        const auto written_time = std::filesystem::last_write_time(stage / relative);
        if (overwrite) {
            std::filesystem::create_directories((destination / relative).parent_path());
            std::ofstream(destination / relative, std::ios::binary) << "old bytes";
        }
        transaction.publish(overwrite);
        const auto after = publication_file_identity(destination / relative);
        const auto empty_after = publication_file_identity(destination / "empty.txt");
        REQUIRE_EQ(before.dwVolumeSerialNumber, after.dwVolumeSerialNumber);
        REQUIRE_EQ(before.nFileIndexHigh, after.nFileIndexHigh);
        REQUIRE_EQ(before.nFileIndexLow, after.nFileIndexLow);
        REQUIRE_EQ(empty_before.nFileIndexHigh, empty_after.nFileIndexHigh);
        REQUIRE_EQ(empty_before.nFileIndexLow, empty_after.nFileIndexLow);
        REQUIRE_EQ(std::filesystem::last_write_time(destination / relative), written_time);
        std::ifstream input(destination / relative, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "exact published bytes");
        REQUIRE_TRUE(!std::filesystem::exists(stage));
    }
}

// Purpose: Preserve publication of read-only staged files when moving instead of copying payloads.
// Inputs: One complete private file marked read-only before directory publication.
// Outputs: Publishes exact bytes and removes private staging without requiring writable source attributes.
TEST_CASE(directory_publish_accepts_read_only_payload) {
    const auto root = test_temp_dir("publish-readonly");
    const auto destination = root / "output";
    flitzzip::DirectoryPublishTransaction transaction(destination);
    const auto stage = transaction.staging_directory();
    const auto source = stage / "readonly.txt";
    std::ofstream(source, std::ios::binary) << "read-only payload";
    REQUIRE_TRUE(SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_READONLY) != 0);
    const auto before = publication_file_identity(source);
    transaction.publish(false);
    const auto target = destination / "readonly.txt";
    const auto after = publication_file_identity(target);
    REQUIRE_EQ(before.dwVolumeSerialNumber, after.dwVolumeSerialNumber);
    REQUIRE_EQ(before.nFileIndexHigh, after.nFileIndexHigh);
    REQUIRE_EQ(before.nFileIndexLow, after.nFileIndexLow);
    std::ifstream input(target, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "read-only payload");
    REQUIRE_TRUE(!std::filesystem::exists(stage));
    input.close();
    REQUIRE_TRUE((GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_READONLY) == 0U);
}

// Purpose: Prevent private read-only normalization from changing a file outside the publication transaction.
// Inputs: A read-only caller-owned file hard-linked into the private staging directory.
// Outputs: Rejects the alias without changing outside attributes or publishing any final file.
TEST_CASE(directory_publish_read_only_alias_keeps_attributes) {
    const auto root = test_temp_dir("publish-readonly-alias");
    const auto outside = root / "outside.txt";
    std::ofstream(outside, std::ios::binary) << "outside payload";
    REQUIRE_TRUE(SetFileAttributesW(outside.c_str(), FILE_ATTRIBUTE_READONLY) != 0);
    bool rejected = false;
    {
        flitzzip::DirectoryPublishTransaction transaction(root / "output");
        std::filesystem::create_hard_link(outside, transaction.staging_directory() / "alias.txt");
        try {
            transaction.publish(false);
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
        const auto attributes = GetFileAttributesW(outside.c_str());
        REQUIRE_TRUE(SetFileAttributesW(outside.c_str(), FILE_ATTRIBUTE_NORMAL) != 0);
        REQUIRE_TRUE((attributes & FILE_ATTRIBUTE_READONLY) != 0U);
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 1U);
    REQUIRE_TRUE(!std::filesystem::exists(root / "output" / "alias.txt"));
}

// Purpose: Keep quarantine ownership exclusive when publication stops copying the payload.
// Inputs: A staged hard link to an existing caller-owned file.
// Outputs: Rejects publication and preserves the outside file without a final destination alias.
TEST_CASE(directory_publish_rejects_shared_file_identity) {
    const auto root = test_temp_dir("publish-linked");
    const auto outside = root / "original.txt";
    std::ofstream(outside, std::ios::binary) << "original bytes";
    const auto destination = root / "output";
    bool rejected = false;
    {
        flitzzip::DirectoryPublishTransaction transaction(destination);
        std::filesystem::create_hard_link(outside, transaction.staging_directory() / "alias.txt");
        try {
            transaction.publish(false);
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(destination / "alias.txt"));
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 1U);
    std::ifstream input(outside, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "original bytes");
}

// Purpose: Exercise one-decode publication policy independently of any archive adapter or GUI state.
// Inputs: Both validation settings, optional inspection, and overwrite enabled on the final destination.
// Outputs: Decodes once, never overwrites inside quarantine, inspects before final visibility, and publishes exact
// bytes.
TEST_CASE(extraction_publication_decodes_once_with_captured_policy) {
    const auto root = test_temp_dir("extract-publication-policy");
    for (const bool validate : {false, true}) {
        for (const bool inspect : {false, true}) {
            const auto destination = root / (std::to_string(validate) + std::to_string(inspect));
            int decodes = 0;
            int inspections = 0;
            std::filesystem::path decoded_path;
            std::function<void(const std::filesystem::path&)> inspector;
            if (inspect) {
                inspector = [&](const auto& path) {
                    ++inspections;
                    REQUIRE_EQ(path, decoded_path);
                    REQUIRE_TRUE(!std::filesystem::exists(destination / "file.txt"));
                    REQUIRE_EQ(std::filesystem::file_size(path / "file.txt"), 7U);
                };
            }
            flitzzip::extract_with_publication(
                {.destination = destination, .overwrite = true, .validate_before_publish = validate},
                [&](const auto& path, bool overwrite) {
                    ++decodes;
                    decoded_path = path;
                    REQUIRE_EQ(overwrite, !validate && !inspect);
                    REQUIRE_EQ(path == destination, !validate && !inspect);
                    std::filesystem::create_directories(path);
                    std::ofstream(path / "file.txt", std::ios::binary) << "payload";
                },
                inspector);
            REQUIRE_EQ(decodes, 1);
            REQUIRE_EQ(inspections, inspect ? 1 : 0);
            REQUIRE_EQ(std::filesystem::file_size(destination / "file.txt"), 7U);
            if (validate || inspect)
                REQUIRE_TRUE(!std::filesystem::exists(decoded_path));
        }
    }
}

// Purpose: Prevent late decode, inspection, or cancellation failures from exposing final extracted files.
// Inputs: A simulated two-file adapter, existing output, and exceptions at each pre-publication boundary.
// Outputs: Preserves existing bytes, withholds new files, invokes decoding once, and cleans private staging.
TEST_CASE(extraction_publication_failure_keeps_final_files_unchanged) {
    const auto root = test_temp_dir("extract-publication-failure");
    for (const int failure : {0, 1, 2}) {
        const auto destination = root / std::to_string(failure);
        std::filesystem::create_directories(destination);
        std::ofstream(destination / "existing.txt", std::ios::binary) << "old";
        std::filesystem::path stage;
        int decodes = 0;
        bool cancelled = false;
        bool rejected = false;
        try {
            flitzzip::extract_with_publication(
                {.destination = destination, .overwrite = true, .validate_before_publish = true},
                [&](const auto& path, bool overwrite) {
                    REQUIRE_TRUE(!overwrite);
                    ++decodes;
                    stage = path;
                    std::ofstream(path / "existing.txt", std::ios::binary) << "new";
                    std::ofstream(path / "added.txt", std::ios::binary) << "added";
                    if (failure == 0)
                        throw flitzzip::ArchiveError("late decode failure");
                },
                [&](const auto&) {
                    if (failure == 1)
                        throw flitzzip::ArchiveError("inspection failure");
                    cancelled = true;
                },
                [&] {
                    if (cancelled)
                        throw flitzzip::ArchiveError("operation cancelled");
                });
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(decodes, 1);
        REQUIRE_TRUE(!std::filesystem::exists(stage));
        REQUIRE_TRUE(!std::filesystem::exists(destination / "added.txt"));
        std::ifstream input(destination / "existing.txt", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "old");
    }
}

// Purpose: Keep Unicode overwrite diagnostics and transaction cleanup equivalent to ASCII paths.
// Inputs: Existing Unicode files and new payloads in per-file and directory publication transactions.
// Outputs: Checks UTF-8 messages, preserved original bytes, explicit replacement, and absence of private leftovers.
TEST_CASE(file_publication_unicode_overwrite_diagnostics) {
    const auto root = test_temp_dir("publish-unicode") / L"caf\u00e9";
    std::filesystem::create_directories(root);
    const std::u8string utf8_name = u8"\u65e5\u672c-\U0001f680.txt";
    const std::string expected_name(utf8_name.begin(), utf8_name.end());
    const std::filesystem::path name(utf8_name);
    REQUIRE_EQ(flitzzip::path_diagnostic_utf8(name), expected_name);
    REQUIRE_EQ(flitzzip::path_diagnostic_utf8(std::filesystem::path{}), "");
    for (const bool directory : {false, true}) {
        const auto output = root / (directory ? "directory" : "file");
        std::filesystem::create_directories(output);
        const auto target = output / name;
        std::ofstream(target, std::ios::binary) << "old";
        bool refused = false;
        try {
            if (directory) {
                flitzzip::DirectoryPublishTransaction transaction(output);
                std::ofstream(transaction.staging_directory() / name, std::ios::binary) << "new";
                transaction.publish(false);
            } else {
                flitzzip::FilePublishTransaction transaction(target);
                std::ofstream(transaction.staging_path(), std::ios::binary) << "new";
                transaction.commit(false);
            }
        } catch (const flitzzip::SecurityError& error) {
            const std::string message(error.what());
            REQUIRE_TRUE(message.find("refusing to overwrite existing file:") != std::string::npos);
            REQUIRE_TRUE(message.find(expected_name) != std::string::npos);
            refused = true;
        }
        REQUIRE_TRUE(refused);
        {
            std::ifstream input(target, std::ios::binary);
            REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "old");
        }
        if (directory) {
            flitzzip::DirectoryPublishTransaction transaction(output);
            std::ofstream(transaction.staging_directory() / name, std::ios::binary) << "new";
            transaction.publish(true);
        } else {
            flitzzip::FilePublishTransaction transaction(target);
            std::ofstream(transaction.staging_path(), std::ios::binary) << "new";
            transaction.commit(true);
        }
        {
            std::ifstream input(target, std::ios::binary);
            REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "new");
        }
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}),
                   1);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject non-ASCII case aliases before Windows can map them to the same output path.
// Inputs: UTF-8 file duplicates and a file/child conflict using upper- and lowercase A-umlaut.
// Outputs: Requires rejection on Windows and preserves distinct names on case-sensitive hosts.
TEST_CASE(path_set_rejects_unicode_case_aliases) {
    const std::string upper = "\xC3\x84";
    const std::string lower = "\xC3\xA4";
    const std::array cases{
        std::array{upper + ".txt", lower + ".txt"},
        std::array{upper, lower + "/child.txt"},
    };
    for (const auto& paths : cases) {
        const std::array<flitzzip::ArchivePathValidationEntry, 2> entries{{
            {.path = paths[0], .directory = false, .encoding = flitzzip::ArchivePathEncoding::Utf8},
            {.path = paths[1], .directory = false, .encoding = flitzzip::ArchivePathEncoding::Utf8},
        }};
        bool rejected = false;
        try {
            flitzzip::validate_archive_path_set(entries);
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
#ifdef _WIN32
        REQUIRE_TRUE(rejected);
#else
        REQUIRE_TRUE(!rejected);
#endif
    }
}

// Purpose: Reject file/descendant conflicts even when punctuation siblings separate their sorted keys.
// Inputs: All permutations of a file parent, a valid sibling, and a nested child, plus a directory-parent control.
// Outputs: Requires rejection of every file-parent set and acceptance of the corresponding directory-parent set.
TEST_CASE(path_set_rejects_nonadjacent_file_descendants) {
    for (const auto* sibling : {"root/a-sibling", "root/a.sibling", "root/a sibling", "root/a!sibling"}) {
        const std::array<flitzzip::ArchivePathValidationEntry, 3> entries{{
            {.path = "root/a", .directory = false},
            {.path = sibling, .directory = false},
            {.path = "root/a/b.txt", .directory = false},
        }};
        std::array<std::size_t, 3> order{0U, 1U, 2U};
        do {
            std::vector<flitzzip::ArchivePathValidationEntry> permuted;
            for (const auto index : order) {
                permuted.push_back(entries[index]);
            }
            bool rejected = false;
            try {
                flitzzip::validate_archive_path_set(permuted);
            } catch (const flitzzip::SecurityError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            for (auto& entry : permuted) {
                if (entry.path == "root/a") {
                    entry.directory = true;
                }
            }
            flitzzip::validate_archive_path_set(permuted);
        } while (std::next_permutation(order.begin(), order.end()));
    }
}

// Purpose: Verify archive path traversal is rejected before extraction.
// Inputs: A relative path containing a `..` segment.
// Outputs: Throws if traversal is accepted.
TEST_CASE(path_safety_rejects_traversal) {
    const auto root = test_temp_dir("path-safety");
    bool rejected = false;
    try {
        (void)flitzzip::safe_join_archive_path(root, "../escape.txt");
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify Windows reserved names are rejected as archive components.
// Inputs: A path containing the reserved `CON` device name.
// Outputs: Throws if the reserved name is accepted.
TEST_CASE(path_safety_rejects_reserved_windows_names) {
    const auto root = test_temp_dir("reserved");
    bool rejected = false;
    try {
        (void)flitzzip::safe_join_archive_path(root, "CON.txt");
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Verify archive path validation rejects Windows absolute, UNC, invalid-character, and trailing-character
// forms. Inputs: A table of untrusted archive entry names covering Windows path edge cases. Outputs: Throws if any
// unsafe path is accepted.
TEST_CASE(path_safety_rejects_windows_unsafe_forms) {
    const auto root = test_temp_dir("unsafe-forms");
    const std::vector<std::string> unsafe_paths = {
        "/absolute.txt",
        "\\absolute.txt",
        "\\\\server\\share\\file.txt",
        "C:drive.txt",
        "dir/file.",
        "dir/file ",
        "dir/a<b.txt",
        "dir/a>b.txt",
        "dir/a:b.txt",
        "dir/a\"b.txt",
        "dir/a|b.txt",
        "dir/a?b.txt",
        "dir/a*b.txt",
        ".",
        "./.",
        "dir/COM9.txt",
        "dir/LPT1",
        "dir/COM\xC2\xB9.txt",
        "dir/LPT\xC2\xB2",
        std::string("dir/control") + static_cast<char>(0x1F) + ".txt",
    };
    for (const auto& path : unsafe_paths) {
        bool rejected = false;
        try {
            (void)flitzzip::safe_join_archive_path(root, path);
        } catch (const flitzzip::SecurityError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify ordinary nested relative paths are accepted and joined safely.
// Inputs: A destination root and nested archive entry path.
// Outputs: Throws if the normalized target is not the expected child path.
TEST_CASE(path_safety_accepts_nested_relative_path) {
    const auto root = test_temp_dir("nested");
    const auto target = flitzzip::safe_join_archive_path(root, "dir/file.txt");
    REQUIRE_EQ(target.filename().string(), "file.txt");
    std::filesystem::remove_all(root);
}

// Purpose: Preserve explicit UTF-8 filenames independently of the Windows ANSI code page.
// Inputs: Nested Unicode components below a temporary root with a redundant current-directory component.
// Outputs: Requires native path equality and an exact UTF-8 archive-name roundtrip.
TEST_CASE(path_safety_utf8_filename_roundtrip) {
    const auto root = test_temp_dir("utf8-path-roundtrip") / ".";
    const auto canonical_root = std::filesystem::weakly_canonical(root);
    for (const auto* name : {u8"caf\u00e9/file.txt", u8"\u65e5\u672c/\U0001f4c1.txt", u8"e\u0301.txt"}) {
        const std::filesystem::path relative(name);
        const auto archive_name = flitzzip::normalize_entry_name(relative);
        const auto target = flitzzip::safe_join_archive_path(root, archive_name, flitzzip::ArchivePathEncoding::Utf8);
        REQUIRE_EQ(target, canonical_root / relative);
        REQUIRE_EQ(flitzzip::normalize_entry_name(target.lexically_relative(canonical_root)), archive_name);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve UTF-8 names while publishing an internally inventoried extraction tree.
// Inputs: A staged Unicode directory and filename with an exact known payload.
// Outputs: Requires publication at the original native filename with unchanged content.
TEST_CASE(directory_publish_preserves_unicode_names) {
    const auto root = test_temp_dir("unicode-publication");
    const auto destination = root / "output";
    const auto relative = std::filesystem::path(u8"\u65e5\u672c/caf\u00e9-\U0001f4c1.txt").lexically_normal();
    flitzzip::DirectoryPublishTransaction transaction(destination);
    std::filesystem::create_directories((transaction.staging_directory() / relative).parent_path());
    std::ofstream(transaction.staging_directory() / relative, std::ios::binary) << "Unicode payload";
    transaction.publish(false);
    std::ifstream input(destination / relative, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
               std::string("Unicode payload"));
}

// Purpose: Verify existing destination junction parents cannot redirect archive entries outside the extraction root.
// Inputs: A destination root containing a real Windows directory junction to another temporary directory.
// Outputs: Throws if the joined archive path is accepted through the reparse parent.
TEST_CASE(path_safety_rejects_existing_reparse_parent_escape) {
    const auto root = test_temp_dir("path-safety-reparse-root");
    const auto outside = test_temp_dir("path-safety-reparse-outside");
    const auto junction = root / "linked";
    if (!flitzzip_test::try_create_test_directory_junction(junction, outside)) {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        return;
    }

    bool rejected = false;
    try {
        (void)flitzzip::safe_join_archive_path(root, "linked/payload.txt");
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    flitzzip_test::remove_test_directory_junction(junction);
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

// Purpose: Verify archive path key normalization collapses harmless separators and current-directory components.
// Inputs: A path with redundant separators and `.` components.
// Outputs: Throws if the normalized key is not the deterministic archive key used for collision checks.
TEST_CASE(path_safety_normalizes_archive_path_key) {
    REQUIRE_EQ(flitzzip::normalize_archive_path_key("dir//./nested/file.txt"), std::string("dir/nested/file.txt"));
}

// Purpose: Verify a publication reservation prevents its final parent from being renamed during commit.
// Inputs: A nested output target and an attempted parent move while the reservation is active.
// Outputs: The move fails, the exact staged file publishes, and private staging data is removed.
TEST_CASE(file_publish_pins_parent_identity_until_commit) {
    const auto root = test_temp_dir("file-publish-parent-pin");
    const auto target = root / "nested" / "payload.txt";
    const auto reservation = flitzzip::reserve_file_publish_target(target);
    std::ofstream(reservation.file, std::ios::binary) << "verified payload";

    const auto parent_text = target.parent_path().wstring();
    const auto moved_text = (root / "moved-parent").wstring();
    REQUIRE_TRUE(MoveFileExW(parent_text.c_str(), moved_text.c_str(), MOVEFILE_WRITE_THROUGH) == 0);

    flitzzip::commit_verified_file(reservation, target, false);
    flitzzip::cleanup_file_publish_target(reservation);
    std::ifstream input(target, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
               std::string("verified payload"));
    REQUIRE_TRUE(!std::filesystem::exists(reservation.directory));
}

// Purpose: Keep staging alive for shared owners and remove it when the last reservation unwinds or returns.
// Inputs: Two tiny private payloads, a copied reservation and normal/exceptional scope exit.
// Outputs: Preserves the live shared payload, then removes only staging without publishing a final target.
TEST_CASE(file_publish_last_reservation_owner_cleans_abandoned_staging) {
    struct ExpectedUnwind {};
    for (const bool unwind : {false, true}) {
        const auto root = test_temp_dir(unwind ? "reservation-unwind" : "reservation-return");
        const auto target = root / "final.txt";
        std::filesystem::path staging;
        bool unwound = false;
        try {
            flitzzip::ReservedFilePublishTarget survivor;
            {
                const auto original = flitzzip::reserve_file_publish_target(target);
                staging = original.directory;
                std::ofstream(original.file, std::ios::binary) << "private";
                survivor = original;
            }
            REQUIRE_TRUE(std::filesystem::is_regular_file(survivor.file));
            REQUIRE_TRUE(!std::filesystem::exists(target));
            if (unwind)
                throw ExpectedUnwind{};
        } catch (const ExpectedUnwind&) {
            unwound = true;
        }
        REQUIRE_EQ(unwound, unwind);
        REQUIRE_TRUE(!std::filesystem::exists(staging));
        REQUIRE_TRUE(!std::filesystem::exists(target));
        REQUIRE_TRUE(std::filesystem::is_directory(root));
    }
}

// Purpose: Verify publication state cannot be reused to redirect a verified payload to another path.
// Inputs: A valid reservation and a different final filename in the same parent.
// Outputs: Throws `SecurityError` and leaves both final paths absent.
TEST_CASE(file_publish_rejects_target_not_bound_to_reservation) {
    const auto root = test_temp_dir("file-publish-target-binding");
    const auto target = root / "expected.txt";
    const auto redirected = root / "redirected.txt";
    const auto reservation = flitzzip::reserve_file_publish_target(target);
    std::ofstream(reservation.file, std::ios::binary) << "verified payload";

    bool rejected = false;
    try {
        flitzzip::commit_verified_file(reservation, redirected, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(target));
    REQUIRE_TRUE(!std::filesystem::exists(redirected));
    flitzzip::cleanup_file_publish_target(reservation);
}

// Purpose: Verify publication never traverses a preexisting output-parent junction.
// Inputs: A final target below a junction that points outside the selected tree.
// Outputs: Reservation throws and no outside payload is created.
TEST_CASE(file_publish_rejects_reparse_parent_chain) {
    const auto root = test_temp_dir("file-publish-reparse-root");
    const auto outside = test_temp_dir("file-publish-reparse-outside");
    const auto junction = root / "linked";
    if (!flitzzip_test::try_create_test_directory_junction(junction, outside)) {
        return;
    }
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::reserve_file_publish_target(junction / "payload.txt"));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(outside / "payload.txt"));
    flitzzip_test::remove_test_directory_junction(junction);
}

// Purpose: Verify quarantined extraction bytes remain private until explicit publication.
// Inputs: A staged nested file and an empty final destination.
// Outputs: Throws if the file appears early, fails to publish, or changes content during the merge.
TEST_CASE(directory_publish_quarantines_until_explicit_publish) {
    const auto root = test_temp_dir("directory-publish-success");
    const auto destination = root / "output";
    flitzzip::DirectoryPublishTransaction transaction(destination);
    std::filesystem::create_directories(transaction.staging_directory() / "nested");
    std::ofstream(transaction.staging_directory() / "nested" / "payload.txt", std::ios::binary) << "clean payload";
    REQUIRE_TRUE(!std::filesystem::exists(destination / "nested" / "payload.txt"));

    transaction.publish(false);
    std::ifstream input(destination / "nested" / "payload.txt", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
               std::string("clean payload"));
}

// Purpose: Verify failed or abandoned security scans remove all quarantined extraction bytes.
// Inputs: An uncommitted directory publication transaction containing one payload.
// Outputs: Throws if destructor cleanup exposes a final file or leaves the private staging tree behind.
TEST_CASE(directory_publish_abandonment_removes_quarantine) {
    const auto root = test_temp_dir("directory-publish-abandon");
    const auto destination = root / "output";
    std::filesystem::path quarantine;
    {
        flitzzip::DirectoryPublishTransaction transaction(destination);
        quarantine = transaction.staging_directory();
        std::ofstream(quarantine / "detected.bin", std::ios::binary) << "detected payload";
    }
    REQUIRE_TRUE(!std::filesystem::exists(quarantine));
    REQUIRE_TRUE(!std::filesystem::exists(destination / "detected.bin"));
}

// Purpose: Verify overwrite refusal happens before any quarantined file is published.
// Inputs: Two staged files and one conflicting final path with overwrite disabled.
// Outputs: Throws if the existing file changes or a nonconflicting staged file leaks through a partial publication.
TEST_CASE(directory_publish_preflights_overwrite_conflicts) {
    const auto root = test_temp_dir("directory-publish-conflict");
    const auto destination = root / "output";
    std::filesystem::create_directories(destination);
    std::ofstream(destination / "b.txt", std::ios::binary) << "existing";

    bool rejected = false;
    try {
        flitzzip::DirectoryPublishTransaction transaction(destination);
        std::ofstream(transaction.staging_directory() / "a.txt", std::ios::binary) << "new a";
        std::ofstream(transaction.staging_directory() / "b.txt", std::ios::binary) << "new b";
        transaction.publish(false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(destination / "a.txt"));
    std::ifstream input(destination / "b.txt", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
               std::string("existing"));
}

// Purpose: Preserve single-stream suffix semantics without interpreting Unicode bytes as ANSI text.
// Inputs: Native Unicode paths, uppercase aliases, hidden names, and unrecognized suffixes.
// Outputs: Requires exact UTF-8 names, preserved lzip TAR aliases, and rejection of unsafe output names.
TEST_CASE(single_stream_names_preserve_suffix_and_path_safety_rules) {
    const auto unicode = std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6");
    const auto expected = flitzzip::path_diagnostic_utf8(unicode);
    for (const auto* suffix : {".gz", ".bz2", ".xz", ".lzma", ".zst", ".zstd", ".z"}) {
        auto archive = unicode;
        archive += std::string(".bin") + suffix;
        REQUIRE_EQ(flitzzip::single_stream_entry_name(archive, {{suffix, ""}}), expected + ".bin");
    }
    REQUIRE_EQ(flitzzip::single_stream_entry_name("payload.TAR.LZ", {{".tlz", ".tar"}, {".lz", ""}}),
               std::string("payload.TAR"));
    REQUIRE_EQ(flitzzip::single_stream_entry_name("payload.TLZ", {{".tlz", ".tar"}, {".lz", ""}}),
               std::string("payload.tar"));
    REQUIRE_EQ(flitzzip::single_stream_entry_name(".gz", {{".gz", ""}}), std::string(".gz"));
    REQUIRE_EQ(flitzzip::single_stream_entry_name("", {{".gz", ""}}), std::string("payload"));
    REQUIRE_EQ(flitzzip::single_stream_entry_name("payload.other", {{".gz", ""}}), std::string("payload"));
    for (const auto* unsafe : {"CON.gz", "NUL.zst", "payload .bz2"}) {
        bool refused = false;
        try {
            (void)flitzzip::single_stream_entry_name(unsafe, {{".gz", ""}, {".zst", ""}, {".bz2", ""}});
        } catch (const flitzzip::SecurityError&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
    }
}

// Purpose: Keep file-size failures in the archive exception domain for non-ANSI native paths.
// Inputs: A missing Unicode filename and then a created file at the same test-owned path.
// Outputs: Requires an exact UTF-8 error diagnostic and accurate size when the file exists.
TEST_CASE(regular_file_size_preserves_unicode_error_diagnostics) {
    const auto root = test_temp_dir("unicode-file-size");
    const auto path = root / std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.bin");
    bool refused = false;
    try {
        (void)flitzzip::regular_file_size(path);
    } catch (const flitzzip::ArchiveError& error) {
        refused = true;
        REQUIRE_EQ(std::string(error.what()), "cannot read file size: " + flitzzip::path_diagnostic_utf8(path));
    }
    REQUIRE_TRUE(refused);
    std::ofstream(path, std::ios::binary) << "size";
    REQUIRE_EQ(flitzzip::regular_file_size(path), std::uint64_t{4});
}

// Purpose: Reproduce an in-place junction conversion despite a live, non-delete-sharing parent handle.
// Inputs: Test-owned directories, a positive full-path redirection control and every relative admission mode.
// Outputs: Proves the attack setup, then rejects both changed parents and leaf reparses without outside creation.
TEST_CASE(relative_directory_rejects_reparse_after_parent_was_pinned) {
    using flitzzip::windows_detail::DirectoryOpenMode;
    using flitzzip::windows_detail::open_directory_relative;
    const auto root = test_temp_dir("relative-directory-reparse");
    const auto parent = root / "parent";
    const auto outside = root / "outside";
    std::filesystem::create_directory(parent);
    std::filesystem::create_directory(outside);
    {
        std::unique_ptr<void, decltype(&CloseHandle)> root_handle(
            CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
            &CloseHandle);
        REQUIRE_TRUE(root_handle.get() != INVALID_HANDLE_VALUE);
        std::unique_ptr<void, decltype(&CloseHandle)> parent_handle(
            open_directory_relative(root_handle.get(), L"parent", DirectoryOpenMode::Existing), &CloseHandle);
        REQUIRE_TRUE(flitzzip_test::try_create_test_directory_junction(
            parent, outside, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
        REQUIRE_TRUE(CreateDirectoryW((parent / "control").c_str(), nullptr) != 0);
        REQUIRE_TRUE(std::filesystem::is_directory(outside / "control"));
        for (const auto mode :
             {DirectoryOpenMode::Existing, DirectoryOpenMode::OpenOrCreate, DirectoryOpenMode::CreateNew}) {
            bool refused = false;
            try {
                const auto name = mode == DirectoryOpenMode::Existing ? L"control" : L"escaped";
                std::unique_ptr<void, decltype(&CloseHandle)> child(
                    open_directory_relative(parent_handle.get(), name, mode), &CloseHandle);
            } catch (const flitzzip::SecurityError&) {
                refused = true;
            }
            REQUIRE_TRUE(refused);
            REQUIRE_TRUE(!std::filesystem::exists(outside / "escaped"));
        }
        bool leaf_refused = false;
        try {
            std::unique_ptr<void, decltype(&CloseHandle)> leaf(
                open_directory_relative(root_handle.get(), L"parent", DirectoryOpenMode::Existing), &CloseHandle);
        } catch (const flitzzip::SecurityError&) {
            leaf_refused = true;
        }
        REQUIRE_TRUE(leaf_refused);
    }
    flitzzip_test::remove_test_directory_junction(parent);
    std::filesystem::remove_all(root);
}

// Purpose: Keep directory admission bounded to one name and preserve exclusive reservation semantics.
// Inputs: An ordinary pinned parent, unsafe components, a Unicode child and an existing-name collision.
// Outputs: Rejects path interpretation, opens Unicode exactly and reports exclusive collisions without mutation.
TEST_CASE(relative_directory_preserves_names_and_exclusive_creation) {
    using flitzzip::windows_detail::DirectoryOpenMode;
    using flitzzip::windows_detail::open_directory_relative;
    const auto root = test_temp_dir("relative-directory-names");
    {
        std::unique_ptr<void, decltype(&CloseHandle)> parent(
            CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
            &CloseHandle);
        REQUIRE_TRUE(parent.get() != INVALID_HANDLE_VALUE);
        const auto unicode = L"\u65e5\u672c-\U0001f680";
        std::unique_ptr<void, decltype(&CloseHandle)> created(
            open_directory_relative(parent.get(), unicode, DirectoryOpenMode::CreateNew), &CloseHandle);
        std::unique_ptr<void, decltype(&CloseHandle)> reopened(
            open_directory_relative(parent.get(), unicode, DirectoryOpenMode::OpenOrCreate), &CloseHandle);
        REQUIRE_TRUE(std::filesystem::is_directory(root / unicode));
        REQUIRE_EQ(open_directory_relative(parent.get(), unicode, DirectoryOpenMode::CreateNew), INVALID_HANDLE_VALUE);
        for (const auto* component : {L"", L".", L"..", L"../escape", L"child\\escape", L"C:escape"}) {
            bool refused = false;
            try {
                std::unique_ptr<void, decltype(&CloseHandle)> unsafe(
                    open_directory_relative(parent.get(), component, DirectoryOpenMode::CreateNew), &CloseHandle);
            } catch (const flitzzip::SecurityError&) {
                refused = true;
            }
            REQUIRE_TRUE(refused);
        }
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prove that private reservations stabilize their parent without blocking overlapping publications.
// Inputs: Two simultaneous Unicode file transactions and an attempted junction conversion of their shared parent.
// Outputs: Refuses redirection, publishes both exact payloads and removes staging without creating outside files.
TEST_CASE(file_publish_overlapping_reservations_keep_parent_stable) {
    const auto root = test_temp_dir("publish-overlapping-reservations");
    const auto parent = root / L"\u65e5\u672c";
    const auto outside = root / "outside";
    std::filesystem::create_directory(outside);
    {
        flitzzip::FilePublishTransaction first(parent / L"caf\u00e9.txt");
        flitzzip::FilePublishTransaction second(parent / L"\U0001f680.txt");
        REQUIRE_TRUE(!flitzzip_test::try_create_test_directory_junction(
            parent, outside, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
        std::ofstream(first.staging_path(), std::ios::binary) << "first bytes";
        std::ofstream(second.staging_path(), std::ios::binary) << "second bytes";
        first.commit(false);
        second.commit(false);
    }
    std::ifstream first(parent / L"caf\u00e9.txt", std::ios::binary);
    std::ifstream second(parent / L"\U0001f680.txt", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(first), {}), "first bytes");
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(second), {}), "second bytes");
    first.close();
    second.close();
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(parent), std::filesystem::directory_iterator{}), 2);
    REQUIRE_TRUE(std::filesystem::is_empty(outside));
    std::filesystem::remove_all(root);
}

// Purpose: Keep a valid maximum-length output filename independent of private staging-name overhead.
// Inputs: A 255-character filename, private temporary payload and ordinary final publication.
// Outputs: Publishes the exact bytes and identity without exceeding a filesystem component's name limit.
TEST_CASE(file_publish_supports_maximum_length_filename) {
    const auto root = test_temp_dir("publish-component-limit");
    const auto target = root / (std::wstring(251U, L'x') + L".bin");
    const auto target_io = std::filesystem::path(flitzzip::windows_api_path(target));
    {
        flitzzip::FilePublishTransaction transaction(target);
        const auto staging = transaction.staging_path();
        REQUIRE_TRUE(staging.parent_path().filename().native().size() <= 255U);
        std::ofstream(staging, std::ios::binary) << "maximum filename payload";
        const auto before = publication_file_identity(staging);
        transaction.commit(false);
        const auto after = publication_file_identity(target_io);
        REQUIRE_EQ(before.dwVolumeSerialNumber, after.dwVolumeSerialNumber);
        REQUIRE_EQ(before.nFileIndexHigh, after.nFileIndexHigh);
        REQUIRE_EQ(before.nFileIndexLow, after.nFileIndexLow);
        std::ifstream input(target_io, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "maximum filename payload");
        input.close();
        REQUIRE_TRUE(!std::filesystem::exists(staging.parent_path()));
    }
    std::filesystem::remove_all(std::filesystem::path(flitzzip::windows_api_path(root)));
}
