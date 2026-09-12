#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
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

// Purpose: Prove private publication moves the original file rather than rewriting its payload.
// Inputs: Nested nonempty and empty Unicode staged files, existing destinations, and a fixed write timestamp.
// Outputs: Requires exact file identity, timestamp, and contents after publication, with private staging removed.
TEST_CASE(directory_publish_moves_payload_without_copying) {
    const auto root = test_temp_dir("publish-identity");
    for (const bool overwrite : {false, true}) {
        const auto destination = root / (overwrite ? "replace" : "new");
        superzip::DirectoryPublishTransaction transaction(destination);
        const auto stage = transaction.staging_directory();
        const auto relative = std::filesystem::path(u8"\u65e5\u672c/caf\u00e9.txt");
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
    superzip::DirectoryPublishTransaction transaction(destination);
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
        superzip::DirectoryPublishTransaction transaction(root / "output");
        std::filesystem::create_hard_link(outside, transaction.staging_directory() / "alias.txt");
        try {
            transaction.publish(false);
        } catch (const superzip::SecurityError&) {
            rejected = true;
        }
        const auto attributes = GetFileAttributesW(outside.c_str());
        REQUIRE_TRUE(SetFileAttributesW(outside.c_str(), FILE_ATTRIBUTE_NORMAL) != 0);
        REQUIRE_TRUE((attributes & FILE_ATTRIBUTE_READONLY) != 0U);
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(std::filesystem::hard_link_count(outside), 1U);
    REQUIRE_TRUE(!std::filesystem::exists(root / "output/alias.txt"));
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
        superzip::DirectoryPublishTransaction transaction(destination);
        std::filesystem::create_hard_link(outside, transaction.staging_directory() / "alias.txt");
        try {
            transaction.publish(false);
        } catch (const superzip::SecurityError&) {
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
            superzip::extract_with_publication(
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
            superzip::extract_with_publication(
                {.destination = destination, .overwrite = true, .validate_before_publish = true},
                [&](const auto& path, bool overwrite) {
                    REQUIRE_TRUE(!overwrite);
                    ++decodes;
                    stage = path;
                    std::ofstream(path / "existing.txt", std::ios::binary) << "new";
                    std::ofstream(path / "added.txt", std::ios::binary) << "added";
                    if (failure == 0)
                        throw superzip::ArchiveError("late decode failure");
                },
                [&](const auto&) {
                    if (failure == 1)
                        throw superzip::ArchiveError("inspection failure");
                    cancelled = true;
                },
                [&] {
                    if (cancelled)
                        throw superzip::ArchiveError("operation cancelled");
                });
        } catch (const superzip::ArchiveError&) {
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
    REQUIRE_EQ(superzip::path_diagnostic_utf8(name), expected_name);
    REQUIRE_EQ(superzip::path_diagnostic_utf8(std::filesystem::path{}), "");
    for (const bool directory : {false, true}) {
        const auto output = root / (directory ? "directory" : "file");
        std::filesystem::create_directories(output);
        const auto target = output / name;
        std::ofstream(target, std::ios::binary) << "old";
        bool refused = false;
        try {
            if (directory) {
                superzip::DirectoryPublishTransaction transaction(output);
                std::ofstream(transaction.staging_directory() / name, std::ios::binary) << "new";
                transaction.publish(false);
            } else {
                superzip::FilePublishTransaction transaction(target);
                std::ofstream(transaction.staging_path(), std::ios::binary) << "new";
                transaction.commit(false);
            }
        } catch (const superzip::SecurityError& error) {
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
            superzip::DirectoryPublishTransaction transaction(output);
            std::ofstream(transaction.staging_directory() / name, std::ios::binary) << "new";
            transaction.publish(true);
        } else {
            superzip::FilePublishTransaction transaction(target);
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

// Purpose: Reject file/descendant conflicts even when punctuation siblings separate their sorted keys.
// Inputs: All permutations of a file parent, a valid sibling, and a nested child, plus a directory-parent control.
// Outputs: Requires rejection of every file-parent set and acceptance of the corresponding directory-parent set.
TEST_CASE(path_set_rejects_nonadjacent_file_descendants) {
    for (const auto* sibling : {"root/a-sibling", "root/a.sibling", "root/a sibling", "root/a!sibling"}) {
        const std::array<superzip::ArchivePathValidationEntry, 3> entries{{
            {.path = "root/a", .directory = false},
            {.path = sibling, .directory = false},
            {.path = "root/a/b.txt", .directory = false},
        }};
        std::array<std::size_t, 3> order{0U, 1U, 2U};
        do {
            std::vector<superzip::ArchivePathValidationEntry> permuted;
            for (const auto index : order) {
                permuted.push_back(entries[index]);
            }
            bool rejected = false;
            try {
                superzip::validate_archive_path_set(permuted);
            } catch (const superzip::SecurityError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            for (auto& entry : permuted) {
                if (entry.path == "root/a") {
                    entry.directory = true;
                }
            }
            superzip::validate_archive_path_set(permuted);
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
        (void)superzip::safe_join_archive_path(root, "../escape.txt");
    } catch (const superzip::SecurityError&) {
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
        (void)superzip::safe_join_archive_path(root, "CON.txt");
    } catch (const superzip::SecurityError&) {
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
            (void)superzip::safe_join_archive_path(root, path);
        } catch (const superzip::SecurityError&) {
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
    const auto target = superzip::safe_join_archive_path(root, "dir/file.txt");
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
        const auto archive_name = superzip::normalize_entry_name(relative);
        const auto target = superzip::safe_join_archive_path(root, archive_name, superzip::ArchivePathEncoding::Utf8);
        REQUIRE_EQ(target, canonical_root / relative);
        REQUIRE_EQ(superzip::normalize_entry_name(target.lexically_relative(canonical_root)), archive_name);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve UTF-8 names while publishing an internally inventoried extraction tree.
// Inputs: A staged Unicode directory and filename with an exact known payload.
// Outputs: Requires publication at the original native filename with unchanged content.
TEST_CASE(directory_publish_preserves_unicode_names) {
    const auto root = test_temp_dir("unicode-publication");
    const auto destination = root / "output";
    const auto relative = std::filesystem::path(u8"\u65e5\u672c/caf\u00e9-\U0001f4c1.txt");
    superzip::DirectoryPublishTransaction transaction(destination);
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
    if (!superzip_test::try_create_test_directory_junction(junction, outside)) {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        return;
    }

    bool rejected = false;
    try {
        (void)superzip::safe_join_archive_path(root, "linked/payload.txt");
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    superzip_test::remove_test_directory_junction(junction);
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

// Purpose: Verify archive path key normalization collapses harmless separators and current-directory components.
// Inputs: A path with redundant separators and `.` components.
// Outputs: Throws if the normalized key is not the deterministic archive key used for collision checks.
TEST_CASE(path_safety_normalizes_archive_path_key) {
    REQUIRE_EQ(superzip::normalize_archive_path_key("dir//./nested/file.txt"), std::string("dir/nested/file.txt"));
}

// Purpose: Verify a publication reservation prevents its final parent from being renamed during commit.
// Inputs: A nested output target and an attempted parent move while the reservation is active.
// Outputs: The move fails, the exact staged file publishes, and private staging data is removed.
TEST_CASE(file_publish_pins_parent_identity_until_commit) {
    const auto root = test_temp_dir("file-publish-parent-pin");
    const auto target = root / "nested" / "payload.txt";
    const auto reservation = superzip::reserve_file_publish_target(target);
    std::ofstream(reservation.file, std::ios::binary) << "verified payload";

    const auto parent_text = target.parent_path().wstring();
    const auto moved_text = (root / "moved-parent").wstring();
    REQUIRE_TRUE(MoveFileExW(parent_text.c_str(), moved_text.c_str(), MOVEFILE_WRITE_THROUGH) == 0);

    superzip::commit_verified_file(reservation, target, false);
    superzip::cleanup_file_publish_target(reservation);
    std::ifstream input(target, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
               std::string("verified payload"));
    REQUIRE_TRUE(!std::filesystem::exists(reservation.directory));
}

// Purpose: Verify publication state cannot be reused to redirect a verified payload to another path.
// Inputs: A valid reservation and a different final filename in the same parent.
// Outputs: Throws `SecurityError` and leaves both final paths absent.
TEST_CASE(file_publish_rejects_target_not_bound_to_reservation) {
    const auto root = test_temp_dir("file-publish-target-binding");
    const auto target = root / "expected.txt";
    const auto redirected = root / "redirected.txt";
    const auto reservation = superzip::reserve_file_publish_target(target);
    std::ofstream(reservation.file, std::ios::binary) << "verified payload";

    bool rejected = false;
    try {
        superzip::commit_verified_file(reservation, redirected, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(target));
    REQUIRE_TRUE(!std::filesystem::exists(redirected));
    superzip::cleanup_file_publish_target(reservation);
}

// Purpose: Verify publication never traverses a preexisting output-parent junction.
// Inputs: A final target below a junction that points outside the selected tree.
// Outputs: Reservation throws and no outside payload is created.
TEST_CASE(file_publish_rejects_reparse_parent_chain) {
    const auto root = test_temp_dir("file-publish-reparse-root");
    const auto outside = test_temp_dir("file-publish-reparse-outside");
    const auto junction = root / "linked";
    if (!superzip_test::try_create_test_directory_junction(junction, outside)) {
        return;
    }
    bool rejected = false;
    try {
        static_cast<void>(superzip::reserve_file_publish_target(junction / "payload.txt"));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(outside / "payload.txt"));
    superzip_test::remove_test_directory_junction(junction);
}

// Purpose: Verify quarantined extraction bytes remain private until explicit publication.
// Inputs: A staged nested file and an empty final destination.
// Outputs: Throws if the file appears early, fails to publish, or changes content during the merge.
TEST_CASE(directory_publish_quarantines_until_explicit_publish) {
    const auto root = test_temp_dir("directory-publish-success");
    const auto destination = root / "output";
    superzip::DirectoryPublishTransaction transaction(destination);
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
        superzip::DirectoryPublishTransaction transaction(destination);
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
        superzip::DirectoryPublishTransaction transaction(destination);
        std::ofstream(transaction.staging_directory() / "a.txt", std::ios::binary) << "new a";
        std::ofstream(transaction.staging_directory() / "b.txt", std::ios::binary) << "new b";
        transaction.publish(false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(destination / "a.txt"));
    std::ifstream input(destination / "b.txt", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()),
               std::string("existing"));
}
