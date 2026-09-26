#include "core/archive.hpp"
#include "core/result.hpp"
#include "test_suzip_helpers.hpp"
#include "test_util.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Purpose: Read a small fixture or existing destination for exact publication assertions.
// Inputs: `path` is an existing, test-owned regular file.
// Outputs: Returns its complete bytes or fails the test on an open/read error.
std::string read_verification_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.is_open());
    const std::string bytes(std::istreambuf_iterator<char>(input), {});
    REQUIRE_TRUE(!input.bad());
    return bytes;
}

// Purpose: Use small deterministic CPU windows to expose verification progress within one file.
// Inputs: None.
// Outputs: Returns compression options with read-back verification enabled and bounded concurrency.
superzip::CompressOptions verification_compress_options() {
    return {
        .gpu_required = false,
        .force_cpu = true,
        .chunk_size = superzip::kMinArchiveBlockBytes,
        .block_size = superzip::kMinArchiveBlockBytes,
        .worker_count = 2,
        .max_inflight_chunks = 0,
        .verify_after_write = true,
    };
}

// Purpose: Check that a failed or uncommitted create has not changed its destination or left staging behind.
// Inputs: `root` contains the source, `archive` is the destination, and `existing` selects replacement or new output.
// Outputs: Fails unless only the original source and optional original destination remain.
void require_unpublished_verification(const std::filesystem::path& root, const std::filesystem::path& archive,
                                      bool existing) {
    REQUIRE_EQ(std::filesystem::exists(archive), existing);
    if (existing) {
        REQUIRE_EQ(read_verification_fixture(archive), "existing archive bytes");
    }
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}),
               existing ? 2 : 1);
}

}  // namespace

// Purpose: Reject corrupted staged native bytes without publishing a new file or replacing an existing archive.
// Inputs: A deterministic raw-block source and one payload-bit fault injected before the first verification window.
// Outputs: Requires an actual CRC mismatch from the production decoder and complete preservation/cleanup.
TEST_CASE(suzip_write_verification_crc_failure_preserves_destination) {
    for (const bool existing : {false, true}) {
        const auto root = test_temp_dir("suzip-write-verification-crc");
        const auto source = root / "source.bin";
        const auto archive = root / "output.suzip";
        std::string payload(3U * superzip::kMinArchiveBlockBytes + 1U, '\0');
        std::uint32_t state = 0x71F2A903U;
        for (auto& byte : payload) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            byte = static_cast<char>(state & 255U);
        }
        std::ofstream(source, std::ios::binary) << payload;
        if (existing) {
            std::ofstream(archive, std::ios::binary) << "existing archive bytes";
        }
        bool corrupted = false;
        bool rejected = false;
        try {
            static_cast<void>(
                superzip::compress_suzip({source}, archive, verification_compress_options(), [&](const auto& snapshot) {
                    if (corrupted || snapshot.operation != superzip::OperationKind::Verify) {
                        return;
                    }
                    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
                        if (!item.is_regular_file() || item.path() == source || item.path() == archive) {
                            continue;
                        }
                        REQUIRE_TRUE(!corrupted);
                        const auto index = superzip_test::read_test_archive_index(item.path());
                        REQUIRE_EQ(index.entries.size(), 1U);
                        REQUIRE_EQ(index.entries.front().blocks.front().kind, superzip::BlockKind::Raw);
                        superzip_test::xor_archive_byte(item.path(), index.entries.front().payload_offset, 1U);
                        corrupted = true;
                    }
                    REQUIRE_TRUE(corrupted);
                }));
        } catch (const superzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "CRC mismatch while verifying: source.bin");
            rejected = true;
        }
        REQUIRE_TRUE(corrupted);
        REQUIRE_TRUE(rejected);
        require_unpublished_verification(root, archive, existing);
        std::filesystem::remove_all(root);
    }
}

// Purpose: Withhold native output until every read-back progress callback has returned successfully.
// Inputs: A multi-window source, both new/replacement destinations, and verification enabled or disabled.
// Outputs: Requires correct publication, no verification when disabled, and unchanged output throughout verification.
TEST_CASE(suzip_write_verification_precedes_publication) {
    for (const bool existing : {false, true}) {
        for (const bool verify : {false, true}) {
            const auto root = test_temp_dir("suzip-write-verification-order");
            const auto source = root / "source.txt";
            const auto archive = root / "output.suzip";
            const std::string payload(3U * superzip::kMinArchiveBlockBytes + 1U, 'V');
            std::ofstream(source, std::ios::binary) << payload;
            if (existing) {
                std::ofstream(archive, std::ios::binary) << "existing archive bytes";
            }
            auto options = verification_compress_options();
            options.verify_after_write = verify;
            std::vector<superzip::ProgressSnapshot> snapshots;
            const auto stats = superzip::compress_suzip({source}, archive, options, [&](const auto& snapshot) {
                if (snapshot.operation != superzip::OperationKind::Verify) {
                    return;
                }
                REQUIRE_EQ(std::filesystem::exists(archive), existing);
                if (existing) {
                    REQUIRE_EQ(read_verification_fixture(archive), "existing archive bytes");
                }
                snapshots.push_back(snapshot);
            });
            REQUIRE_EQ(!snapshots.empty(), verify);
            if (verify) {
                REQUIRE_EQ(snapshots.front().processed_bytes, 0U);
                REQUIRE_EQ(snapshots.back().processed_bytes, payload.size());
                REQUIRE_EQ(snapshots.back().completed_entries, 1U);
            }
            REQUIRE_EQ(stats.input_bytes, payload.size());
            REQUIRE_EQ(stats.output_bytes, std::filesystem::file_size(archive));
            REQUIRE_TRUE(!stats.gpu_used);
            superzip::ExtractOptions extraction;
            extraction.gpu_required = false;
            extraction.force_cpu = true;
            static_cast<void>(superzip::extract_suzip(archive, root / "extracted", extraction));
            REQUIRE_EQ(read_verification_fixture(root / "extracted" / source.filename()), payload);
            std::filesystem::remove_all(root);
        }
    }
}

// Purpose: Preserve the destination when callers cancel at the start, middle, or completion of verification.
// Inputs: A multi-window native source and new/replacement output paths, with an exception at each checkpoint.
// Outputs: Requires the original callback exception, unchanged or absent final output, and complete staging cleanup.
TEST_CASE(suzip_write_verification_cancellation_preserves_destination) {
    for (const bool existing : {false, true}) {
        for (const int phase : {0, 1, 2}) {
            const auto root = test_temp_dir("suzip-write-verification-cancel");
            const auto source = root / "source.txt";
            const auto archive = root / "output.suzip";
            std::ofstream(source, std::ios::binary) << std::string(3U * superzip::kMinArchiveBlockBytes + 1U, 'C');
            if (existing) {
                std::ofstream(archive, std::ios::binary) << "existing archive bytes";
            }
            bool cancelled = false;
            try {
                static_cast<void>(superzip::compress_suzip(
                    {source}, archive, verification_compress_options(), [&](const auto& snapshot) {
                        if (snapshot.operation != superzip::OperationKind::Verify) {
                            return;
                        }
                        const bool at_start = snapshot.processed_bytes == 0;
                        const bool in_file =
                            snapshot.processed_bytes > 0 && snapshot.processed_bytes < snapshot.total_bytes;
                        const bool at_end = snapshot.completed_entries == snapshot.total_entries;
                        if ((phase == 0 && at_start) || (phase == 1 && in_file) || (phase == 2 && at_end)) {
                            throw superzip::ArchiveError("test verification cancelled");
                        }
                    }));
            } catch (const superzip::ArchiveError& error) {
                REQUIRE_EQ(std::string(error.what()), "test verification cancelled");
                cancelled = true;
            }
            REQUIRE_TRUE(cancelled);
            require_unpublished_verification(root, archive, existing);
            std::filesystem::remove_all(root);
        }
    }
}

// Purpose: Expose bounded decode progress so a large entry does not defer cancellation until its final byte.
// Inputs: A four-window CPU archive verified without extracting files.
// Outputs: Requires monotonic counters, complete terminal progress, and an observable intermediate window.
TEST_CASE(suzip_verification_reports_each_decode_window) {
    const auto root = test_temp_dir("suzip-verification-windows");
    const auto source = root / "source.txt";
    const auto archive = root / "output.suzip";
    std::ofstream(source, std::ios::binary) << std::string(3U * superzip::kMinArchiveBlockBytes + 1U, 'W');
    auto compression = verification_compress_options();
    compression.verify_after_write = false;
    static_cast<void>(superzip::compress_suzip({source}, archive, compression));
    superzip::ExtractOptions options;
    options.gpu_required = false;
    options.force_cpu = true;
    options.chunk_size = superzip::kMinArchiveBlockBytes;
    options.block_size = superzip::kMinArchiveBlockBytes;
    options.worker_count = 2;
    options.max_inflight_chunks = 0;
    std::vector<superzip::ProgressSnapshot> snapshots;
    const auto stats = superzip::verify_suzip(archive, options, [&](const auto& snapshot) {
        REQUIRE_EQ(snapshot.operation, superzip::OperationKind::Verify);
        snapshots.push_back(snapshot);
    });
    REQUIRE_TRUE(!snapshots.empty());
    REQUIRE_EQ(snapshots.front().processed_bytes, 0U);
    bool intermediate = false;
    std::uint64_t previous = 0;
    for (const auto& snapshot : snapshots) {
        REQUIRE_TRUE(snapshot.processed_bytes >= previous);
        REQUIRE_TRUE(snapshot.processed_bytes <= snapshot.total_bytes);
        REQUIRE_TRUE(snapshot.processed_bytes - previous <= superzip::kMinArchiveBlockBytes);
        previous = snapshot.processed_bytes;
        intermediate = intermediate || (previous > 0 && previous < snapshot.total_bytes);
    }
    REQUIRE_TRUE(intermediate);
    REQUIRE_EQ(snapshots.back().processed_bytes, stats.output_bytes);
    REQUIRE_EQ(snapshots.back().completed_entries, stats.entries);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 2);
    std::filesystem::remove_all(root);
}

// Purpose: Keep empty native archives and directory-only archives cancellable and observable.
// Inputs: Handcrafted valid indexes with no entries or one directory, and optional callback cancellation.
// Outputs: Requires an initial snapshot, complete counters, and propagated cancellation without extraction output.
TEST_CASE(suzip_verification_empty_archives_report_progress) {
    for (const bool directory : {false, true}) {
        const auto root = test_temp_dir("suzip-verification-empty");
        const auto archive = root / "empty.suzip";
        superzip::ArchiveIndex index;
        if (directory) {
            index.entries.push_back({.path = "empty", .directory = true});
        }
        {
            std::ofstream output(archive, std::ios::binary);
            superzip::write_archive_index(output, index);
            const auto index_size = static_cast<std::uint64_t>(output.tellp());
            superzip_test::write_test_footer(output, 0, index_size);
            REQUIRE_TRUE(output.good());
        }
        superzip::ExtractOptions options;
        options.gpu_required = false;
        options.force_cpu = true;
        std::vector<superzip::ProgressSnapshot> snapshots;
        static_cast<void>(
            superzip::verify_suzip(archive, options, [&](const auto& snapshot) { snapshots.push_back(snapshot); }));
        REQUIRE_TRUE(!snapshots.empty());
        REQUIRE_EQ(snapshots.front().processed_bytes, 0U);
        REQUIRE_EQ(snapshots.back().completed_entries, index.entries.size());
        REQUIRE_EQ(snapshots.back().processed_bytes, snapshots.back().total_bytes);
        bool cancelled = false;
        try {
            static_cast<void>(superzip::verify_suzip(
                archive, options, [](const auto&) { throw superzip::ArchiveError("empty verification cancelled"); }));
        } catch (const superzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "empty verification cancelled");
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
        std::filesystem::remove_all(root);
    }
}
