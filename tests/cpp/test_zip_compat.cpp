#include "test_util.hpp"
#include "zip/zip_adapter.hpp"
#include "core/result.hpp"

#include <fstream>
#include <windows.h>

#include "miniz.h"

// Purpose: Verify standard ZIP compatibility roundtrip for regular files.
// Inputs: A temporary source file compressed through the miniz adapter.
// Outputs: Throws if extraction fails or restored contents differ.
TEST_CASE(zip_compat_roundtrip) {
    const auto root = test_temp_dir("zip-compat");
    const auto input = root / "input";
    std::filesystem::create_directories(input);
    std::ofstream(input / "hello.txt") << "hello zip";
    const auto archive = root / "archive.zip";
    const auto stats = superzip::compress_zip({input}, archive);
    REQUIRE_TRUE(stats.output_bytes > 0);
    const auto output = root / "out";
    (void)superzip::extract_zip(archive, output, true);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "hello.txt"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify malicious ZIP traversal entries are rejected before writing.
// Inputs: A handcrafted ZIP containing `../escape.txt`.
// Outputs: Throws if extraction does not reject the unsafe entry.
TEST_CASE(zip_extract_rejects_traversal_entry) {
    const auto root = test_temp_dir("zip-traversal");
    const auto archive = root / "bad.zip";
    mz_zip_archive zip{};
    REQUIRE_TRUE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0) != 0);
    const char payload[] = "owned";
    REQUIRE_TRUE(mz_zip_writer_add_mem(&zip, "../escape.txt", payload, sizeof(payload) - 1, MZ_BEST_SPEED) != 0);
    REQUIRE_TRUE(mz_zip_writer_finalize_archive(&zip) != 0);
    mz_zip_writer_end(&zip);

    bool rejected = false;
    try {
        (void)superzip::extract_zip(archive, root / "out", true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    std::filesystem::remove_all(root);
}
