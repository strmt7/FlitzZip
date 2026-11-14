#pragma once

#include "test_flzip_helpers.hpp"

#include "core/archive.hpp"
#include "core/progress.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace flitzzip_test {

// Observe the last completed entry separately from final byte progress. All
// fixture files must exist by the extraction completion notification.
template <class Create, class Extract>
void require_archive_entry_completion(const char* label, bool extracting, Create create, Extract extract) {
    ScopedTestDirectory cleanup{test_temp_dir(label)};
    const auto source = cleanup.path / "input";
    const auto archive = cleanup.path / "input.archive";
    const auto output = cleanup.path / "output";
    std::filesystem::create_directory(source);
    const std::string expected = "final member";
    std::ofstream(source / "data.bin", std::ios::binary) << expected;
    std::ofstream(source / "empty.bin", std::ios::binary).close();
    flitzzip::ProgressSnapshot last;
    const flitzzip::ProgressCallback observer = [&](const auto& snapshot) {
        if (snapshot.total_entries == 0U) {
            return;
        }
        REQUIRE_EQ(snapshot.total_entries, 3U);
        REQUIRE_EQ(snapshot.total_bytes, expected.size());
        REQUIRE_TRUE(snapshot.completed_entries >= last.completed_entries && snapshot.completed_entries <= 3U);
        REQUIRE_TRUE(snapshot.processed_bytes >= last.processed_bytes && snapshot.processed_bytes <= expected.size());
        last = snapshot;
        if (extracting && snapshot.completed_entries == 3U) {
            REQUIRE_TRUE(std::filesystem::is_regular_file(output / "input" / "data.bin"));
            REQUIRE_TRUE(std::filesystem::is_regular_file(output / "input" / "empty.bin"));
        }
    };
    const auto created = create(std::vector<std::filesystem::path>{source}, archive,
                                extracting ? flitzzip::ProgressCallback{} : observer);
    REQUIRE_EQ(created.entries, 3U);
    if (!extracting) {
        REQUIRE_EQ(last.operation, flitzzip::OperationKind::Compress);
        REQUIRE_EQ(last.completed_entries, 3U);
        REQUIRE_EQ(last.processed_bytes, expected.size());
    }
    const auto restored = extract(archive, output, false, extracting ? observer : flitzzip::ProgressCallback{});
    REQUIRE_EQ(restored.entries, 3U);
    REQUIRE_EQ(restored.output_bytes, expected.size());
    if (extracting) {
        REQUIRE_EQ(last.operation, flitzzip::OperationKind::Extract);
        REQUIRE_EQ(last.completed_entries, 3U);
        REQUIRE_EQ(last.processed_bytes, expected.size());
    }
    {
        std::ifstream input(output / "input" / "data.bin", std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), expected);
    }
    REQUIRE_EQ(std::filesystem::file_size(output / "input" / "empty.bin"), 0U);
    std::filesystem::remove_all(cleanup.path);
}

}  // namespace flitzzip_test
