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
