#include "core/file_manifest.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>

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
