#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <windows.h>
#include <algorithm>
#include <array>

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
