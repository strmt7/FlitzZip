#include "core/result.hpp"
#include "core/path_text.hpp"
#include "zip/zip_adapter.hpp"
#include "miniz.h"
#include "test_util.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

// Purpose: Create bounded stored ZIP fixtures independently of the production filesystem writer.
// Inputs: `path` is test-owned output and `entries` supplies raw names and bytes; `flags` selects ZIP name encoding.
// Outputs: Writes a complete ZIP or fails the test while releasing miniz state on all paths.
void write_verification_zip(const std::filesystem::path& path,
                            const std::vector<std::pair<std::string, std::string>>& entries, mz_uint flags = 0) {
    const auto utf8 = path.generic_u8string();
    const std::string filename(utf8.begin(), utf8.end());
    mz_zip_archive zip{};
    REQUIRE_TRUE(mz_zip_writer_init_file(&zip, filename.c_str(), 0));
    try {
        for (const auto& [name, bytes] : entries) {
            REQUIRE_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(), flags));
        }
        REQUIRE_TRUE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE_TRUE(mz_zip_writer_end(&zip));
    } catch (...) {
        mz_zip_writer_end(&zip);
        throw;
    }
}

// Purpose: Read a small file for byte-exact test assertions.
// Inputs: `path` is an existing test fixture.
// Outputs: Returns its contents or fails the test on open/read failure.
std::string read_zip_verification_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.is_open());
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    REQUIRE_TRUE(!input.bad());
    return bytes;
}

// Purpose: Flip one byte in a test-owned ZIP header or stored payload.
// Inputs: `path` is the archive and `offset` is a known fixture byte position.
// Outputs: Changes one byte or fails the test on I/O failure.
void corrupt_verification_zip(const std::filesystem::path& path, std::streamoff offset) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE_TRUE(file.is_open());
    file.seekg(offset);
    char value = 0;
    file.read(&value, 1);
    REQUIRE_EQ(file.gcount(), 1);
    file.seekp(offset);
    file.put(static_cast<char>(static_cast<unsigned char>(value) ^ 1U));
    file.flush();
    REQUIRE_TRUE(file.good());
}

// Purpose: Assert that read-only verification rejects a fixture without creating any extraction files.
// Inputs: `archive` is a malformed or unsafe ZIP in an otherwise single-file test directory.
// Outputs: Requires an archive/security error and no filesystem additions.
void require_zip_verification_rejected(const std::filesystem::path& archive) {
    bool rejected = false;
    try {
        static_cast<void>(superzip::verify_zip(archive));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(archive.parent_path()),
                             std::filesystem::directory_iterator{}),
               1);
}

}  // namespace

// Purpose: Validate ZIP creation before replacing output and exercise both optional verification states.
// Inputs: A multi-window file, new/replacement archive paths, and verification enabled or disabled.
// Outputs: Requires read-back progress only when requested, unchanged output during verification, and correct bytes.
TEST_CASE(zip_write_verification_precedes_publication) {
    for (const bool existing : {false, true}) {
        for (const bool verify : {false, true}) {
            const auto root = test_temp_dir("zip-write-verification-order");
            const auto source = root / "source.txt";
            const auto archive = root / "output.zip";
            const std::string payload(192U * 1024U + 1U, 'Z');
            std::ofstream(source, std::ios::binary) << payload;
            if (existing) {
                std::ofstream(archive, std::ios::binary) << "existing ZIP bytes";
            }
            std::vector<superzip::ProgressSnapshot> snapshots;
            const auto created = superzip::compress_zip(
                {source}, archive, 5,
                [&](const auto& snapshot) {
                    if (snapshot.operation == superzip::OperationKind::Verify) {
                        REQUIRE_EQ(std::filesystem::exists(archive), existing);
                        if (existing) {
                            REQUIRE_EQ(read_zip_verification_file(archive), "existing ZIP bytes");
                        }
                        snapshots.push_back(snapshot);
                    }
                },
                verify);
            REQUIRE_EQ(!snapshots.empty(), verify);
            REQUIRE_EQ(created.input_bytes, payload.size());
            REQUIRE_EQ(created.output_bytes, std::filesystem::file_size(archive));
            if (verify) {
                REQUIRE_EQ(snapshots.front().processed_bytes, 0U);
                REQUIRE_EQ(snapshots.back().processed_bytes, payload.size());
                REQUIRE_EQ(snapshots.back().completed_entries, 1U);
            }
            const auto verified = superzip::verify_zip(archive);
            REQUIRE_EQ(verified.output_bytes, payload.size());
            REQUIRE_TRUE(!verified.gpu_used);
            REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}),
                       2);
            static_cast<void>(superzip::extract_zip(archive, root / "extracted", false));
            REQUIRE_EQ(read_zip_verification_file(root / "extracted" / source.filename()), payload);
            std::filesystem::remove_all(root);
        }
    }
}

// Purpose: Propagate cancellation from inside miniz's decoded-byte callback without publishing output.
// Inputs: A multi-window ZIP payload, initial/mid-file/final exceptions, and new/replacement destinations.
// Outputs: Requires the exact callback exception and removal of private output while preserving the original target.
TEST_CASE(zip_write_verification_cancellation_preserves_destination) {
    for (const bool existing : {false, true}) {
        for (const int phase : {0, 1, 2}) {
            const auto root = test_temp_dir("zip-write-verification-cancel");
            const auto source = root / "source.txt";
            const auto archive = root / "output.zip";
            std::ofstream(source, std::ios::binary) << std::string(192U * 1024U + 1U, 'C');
            if (existing) {
                std::ofstream(archive, std::ios::binary) << "existing ZIP bytes";
            }
            bool cancelled = false;
            try {
                static_cast<void>(superzip::compress_zip(
                    {source}, archive, 5,
                    [&](const auto& snapshot) {
                        if (snapshot.operation != superzip::OperationKind::Verify) {
                            return;
                        }
                        const bool at_start = snapshot.processed_bytes == 0;
                        const bool in_file =
                            snapshot.processed_bytes > 0 && snapshot.processed_bytes < snapshot.total_bytes;
                        const bool at_end = snapshot.completed_entries == snapshot.total_entries;
                        if ((phase == 0 && at_start) || (phase == 1 && in_file) || (phase == 2 && at_end)) {
                            throw superzip::ArchiveError("ZIP verification cancelled");
                        }
                    },
                    true));
            } catch (const superzip::ArchiveError& error) {
                REQUIRE_EQ(std::string(error.what()), "ZIP verification cancelled");
                cancelled = true;
            }
            REQUIRE_TRUE(cancelled);
            REQUIRE_EQ(std::filesystem::exists(archive), existing);
            if (existing) {
                REQUIRE_EQ(read_zip_verification_file(archive), "existing ZIP bytes");
            }
            REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}),
                       existing ? 2 : 1);
            std::filesystem::remove_all(root);
        }
    }
}

// Purpose: Validate local headers even when the member has no decoded payload.
// Inputs: Otherwise valid empty-file and directory ZIPs with a corrupted local-header signature.
// Outputs: Requires rejection without output; central-directory-only validation must not report success.
TEST_CASE(zip_verification_rejects_corrupt_empty_entry_headers) {
    for (const auto* name : {"empty.txt", "empty/"}) {
        const auto root = test_temp_dir("zip-verification-empty-header");
        const auto archive = root / "empty.zip";
        write_verification_zip(archive, {{name, ""}});
        corrupt_verification_zip(archive, 0);
        require_zip_verification_rejected(archive);
        std::filesystem::remove_all(root);
    }
}

// Purpose: Use complete central-directory names through validation and extraction beyond miniz's stat buffer.
// Inputs: A stored member with a safe nested path exceeding 511 bytes.
// Outputs: Requires successful header verification and byte-exact extraction under the complete path.
TEST_CASE(zip_verification_preserves_long_member_names) {
    const auto root = test_temp_dir("zip-verification-long-name");
    const auto archive = root / "long.zip";
    std::string name;
    for (unsigned index = 0; index < 7U; ++index) {
        name += std::string(80, static_cast<char>('a' + index)) + '/';
    }
    name += "payload.txt";
    REQUIRE_TRUE(name.size() > 511U);
    write_verification_zip(archive, {{name, "long member payload"}});
    const auto verified = superzip::verify_zip(archive);
    REQUIRE_EQ(verified.entries, 1U);
    static_cast<void>(superzip::extract_zip(archive, root / "extracted", false));
    const auto target =
        std::filesystem::path(superzip::windows_api_path(root / "extracted" / std::filesystem::path(name)));
    REQUIRE_EQ(read_zip_verification_file(target), "long member payload");
    std::filesystem::remove_all(std::filesystem::path(superzip::windows_api_path(root)));
}

// Purpose: Detect stored payload corruption without writing a decompressed copy.
// Inputs: One stored ZIP member with a flipped first payload byte and intact headers.
// Outputs: Requires the production CRC validator to reject the archive without filesystem additions.
TEST_CASE(zip_verification_rejects_corrupt_payload) {
    const auto root = test_temp_dir("zip-verification-crc");
    const auto archive = root / "corrupt.zip";
    const std::string name = "payload.txt";
    write_verification_zip(archive, {{name, "uncorrupted payload"}});
    corrupt_verification_zip(archive, static_cast<std::streamoff>(30U + name.size()));
    require_zip_verification_rejected(archive);
    std::filesystem::remove_all(root);
}

// Purpose: Share path admission between read-only verification and extraction.
// Inputs: ZIPs containing traversal, reserved names, path collisions, or invalid declared UTF-8.
// Outputs: Requires each unsafe entry set to fail without creating extraction files.
TEST_CASE(zip_verification_rejects_unsafe_paths) {
    for (const auto& entries :
         std::vector<std::vector<std::pair<std::string, std::string>>>{{{"../escape.txt", "bytes"}},
                                                                       {{"CON.txt", "bytes"}},
                                                                       {{"same.txt", "first"}, {"same.txt", "second"}},
                                                                       {{"invalid-\xFF.txt", "bytes"}}}) {
        const auto root = test_temp_dir("zip-verification-unsafe-paths");
        const auto archive = root / "unsafe.zip";
        write_verification_zip(archive, entries);
        require_zip_verification_rejected(archive);
        std::filesystem::remove_all(root);
    }
}

// Purpose: Verify valid empty/directory entries and observable progress for an empty ZIP container.
// Inputs: Empty, empty-file, and directory-only ZIP fixtures.
// Outputs: Requires complete counters and no decompressed output on disk.
TEST_CASE(zip_verification_empty_archives_report_progress) {
    for (const auto& entries :
         std::vector<std::vector<std::pair<std::string, std::string>>>{{}, {{"empty.txt", ""}}, {{"empty/", ""}}}) {
        const auto root = test_temp_dir("zip-verification-empty-progress");
        const auto archive = root / "empty.zip";
        write_verification_zip(archive, entries);
        std::vector<superzip::ProgressSnapshot> snapshots;
        const auto stats = superzip::verify_zip(archive, [&](const auto& snapshot) { snapshots.push_back(snapshot); });
        REQUIRE_TRUE(!snapshots.empty());
        REQUIRE_EQ(snapshots.back().completed_entries, entries.size());
        REQUIRE_EQ(stats.entries, entries.size());
        REQUIRE_EQ(stats.output_bytes, 0U);
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
        std::filesystem::remove_all(root);
    }
}
