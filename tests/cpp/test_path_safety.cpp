#include "core/path_safety.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <vector>
#include <windows.h>

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

// Purpose: Verify archive path validation rejects Windows absolute, UNC, invalid-character, and trailing-character forms.
// Inputs: A table of untrusted archive entry names covering Windows path edge cases.
// Outputs: Throws if any unsafe path is accepted.
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
