#include "core/archive.hpp"
#include "core/result.hpp"
#include "test_flzip_helpers.hpp"
#include "test_util.hpp"
#include "test_source_session.hpp"

#include <fstream>
#include <iterator>
#include <optional>
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
flitzzip::CompressOptions verification_compress_options() {
    return {
        .gpu_required = false,
        .force_cpu = true,
        .chunk_size = flitzzip::kMinArchiveBlockBytes,
        .block_size = flitzzip::kMinArchiveBlockBytes,
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

// Purpose: Observe the one private output owned by this synchronous create operation without naming its directory.
// Inputs: Owned root, original source and final destination. Outputs: The sole staged regular file, or test failure.
std::filesystem::path verification_candidate(const std::filesystem::path& root, const std::filesystem::path& source,
                                             const std::filesystem::path& archive) {
    std::optional<std::filesystem::path> candidate;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (item.is_regular_file() && item.path() != source && item.path() != archive) {
            REQUIRE_TRUE(!candidate);
            candidate = item.path();
        }
    }
    REQUIRE_TRUE(candidate.has_value());
    return *candidate;
}

}  // namespace

// Purpose: Reject corrupted staged native bytes without publishing a new file or replacing an existing archive.
// Inputs: A bounded raw-block source, a persisted producer-write fault before immutable read admission and both target
// states. Outputs: Requires an actual CRC mismatch from the production decoder and complete preservation/cleanup.
TEST_CASE(flzip_write_verification_crc_failure_preserves_destination) {
    for (const bool existing : {false, true}) {
        const auto root = test_temp_dir("flzip-write-verification-crc");
        const auto source = root / "source.bin";
        const auto archive = root / "output.flzip";
        // Exceed the current write buffer so the fault targets actual file bytes rather than buffered producer memory.
        // Both target states, failed candidates and successful retries together write less than the 64 MiB smoke cap.
        std::string payload(5U * 1024U * 1024U + 1U, '\0');
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
        bool geometry_checked = false;
        bool rejected = false;
        auto options = verification_compress_options();
        options.chunk_size = 256U * 1024U;
        try {
            static_cast<void>(flitzzip::compress_flzip({source}, archive, options, [&](const auto& snapshot) {
                if (snapshot.operation == flitzzip::OperationKind::Verify) {
                    REQUIRE_TRUE(corrupted);
                    if (!geometry_checked) {
                        const auto index =
                            flitzzip_test::read_test_archive_index(verification_candidate(root, source, archive));
                        REQUIRE_EQ(index.entries.size(), 1U);
                        REQUIRE_EQ(index.entries.front().blocks.front().kind, flitzzip::BlockKind::Raw);
                        REQUIRE_EQ(index.entries.front().payload_offset, 0U);
                        geometry_checked = true;
                    }
                    return;
                }
                if (corrupted || snapshot.operation != flitzzip::OperationKind::Compress ||
                    snapshot.processed_bytes != payload.size()) {
                    return;
                }
                const auto candidate = verification_candidate(root, source, archive);
                REQUIRE_TRUE(std::filesystem::file_size(candidate) > 0U);
                {
                    std::ifstream persisted(candidate, std::ios::binary);
                    REQUIRE_EQ(persisted.get(), static_cast<unsigned char>(payload.front()));
                }
                flitzzip_test::xor_archive_byte(candidate, 0U, 1U);
                corrupted = true;
            }));
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "CRC mismatch while verifying: source.bin");
            rejected = true;
        }
        REQUIRE_TRUE(corrupted);
        REQUIRE_TRUE(geometry_checked);
        REQUIRE_TRUE(rejected);
        require_unpublished_verification(root, archive, existing);
        REQUIRE_EQ(read_verification_fixture(source), payload);
        REQUIRE_EQ(flitzzip::compress_flzip({source}, archive, options).input_bytes, payload.size());
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 2);
        std::filesystem::remove_all(root);
    }
}

// Purpose: Refuse candidate tampering once the producer has entered immutable read-back verification.
// Inputs: A tiny CPU-only native create and real Windows write admission against its private candidate.
// Outputs: Actual sharing denial throughout verification, normal successful publication and unchanged source bytes.
TEST_CASE(flzip_write_verification_refuses_late_mutation) {
    const auto root = test_temp_dir("flzip-readback-source-session");
    const auto source = root / "source.bin";
    const auto archive = root / "output.flzip";
    const std::string payload(4097U, 'V');
    std::ofstream(source, std::ios::binary) << payload;
    unsigned probes = 0;
    const auto result =
        flitzzip::compress_flzip({source}, archive, verification_compress_options(), [&](const auto& snapshot) {
            if (snapshot.operation == flitzzip::OperationKind::Verify) {
                const auto candidate = verification_candidate(root, source, archive);
                DWORD error = ERROR_SUCCESS;
                REQUIRE_TRUE(!flitzzip_test::source_writer_available(candidate, error));
                REQUIRE_EQ(error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
                ++probes;
            }
        });
    REQUIRE_TRUE(probes > 0U);
    REQUIRE_EQ(result.input_bytes, payload.size());
    REQUIRE_EQ(read_verification_fixture(source), payload);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 2);
    std::filesystem::remove_all(root);
}

// Purpose: Withhold native output until every read-back progress callback has returned successfully.
// Inputs: A multi-window source, both new/replacement destinations, and verification enabled or disabled.
// Outputs: Requires correct publication, no verification when disabled, and unchanged output throughout verification.
TEST_CASE(flzip_write_verification_precedes_publication) {
    for (const bool existing : {false, true}) {
        for (const bool verify : {false, true}) {
            const auto root = test_temp_dir("flzip-write-verification-order");
            const auto source = root / "source.txt";
            const auto archive = root / "output.flzip";
            const std::string payload(3U * flitzzip::kMinArchiveBlockBytes + 1U, 'V');
            std::ofstream(source, std::ios::binary) << payload;
            if (existing) {
                std::ofstream(archive, std::ios::binary) << "existing archive bytes";
            }
            auto options = verification_compress_options();
            options.verify_after_write = verify;
            std::vector<flitzzip::ProgressSnapshot> snapshots;
            const auto stats = flitzzip::compress_flzip({source}, archive, options, [&](const auto& snapshot) {
                if (snapshot.operation != flitzzip::OperationKind::Verify) {
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
            flitzzip::ExtractOptions extraction;
            extraction.gpu_required = false;
            extraction.force_cpu = true;
            static_cast<void>(flitzzip::extract_flzip(archive, root / "extracted", extraction));
            REQUIRE_EQ(read_verification_fixture(root / "extracted" / source.filename()), payload);
            std::filesystem::remove_all(root);
        }
    }
}

// Purpose: Preserve the destination when callers cancel at the start, middle, or completion of verification.
// Inputs: A multi-window native source and new/replacement output paths, with an exception at each checkpoint.
// Outputs: Requires the original callback exception, unchanged or absent final output, and complete staging cleanup.
TEST_CASE(flzip_write_verification_cancellation_preserves_destination) {
    for (const bool existing : {false, true}) {
        for (const int phase : {0, 1, 2}) {
            const auto root = test_temp_dir("flzip-write-verification-cancel");
            const auto source = root / "source.txt";
            const auto archive = root / "output.flzip";
            std::ofstream(source, std::ios::binary) << std::string(3U * flitzzip::kMinArchiveBlockBytes + 1U, 'C');
            if (existing) {
                std::ofstream(archive, std::ios::binary) << "existing archive bytes";
            }
            bool cancelled = false;
            try {
                static_cast<void>(flitzzip::compress_flzip(
                    {source}, archive, verification_compress_options(), [&](const auto& snapshot) {
                        if (snapshot.operation != flitzzip::OperationKind::Verify) {
                            return;
                        }
                        const bool at_start = snapshot.processed_bytes == 0;
                        const bool in_file =
                            snapshot.processed_bytes > 0 && snapshot.processed_bytes < snapshot.total_bytes;
                        const bool at_end = snapshot.completed_entries == snapshot.total_entries;
                        if ((phase == 0 && at_start) || (phase == 1 && in_file) || (phase == 2 && at_end)) {
                            throw flitzzip::ArchiveError("test verification cancelled");
                        }
                    }));
            } catch (const flitzzip::ArchiveError& error) {
                REQUIRE_EQ(std::string(error.what()), "test verification cancelled");
                cancelled = true;
            }
            REQUIRE_TRUE(cancelled);
            require_unpublished_verification(root, archive, existing);
            std::filesystem::remove_all(root);
        }
    }
}

// Purpose: Let callers cancel native extraction between decode windows without exposing partial output.
// Inputs: A multi-window CPU archive and absent/existing extraction targets with overwrite enabled.
// Outputs: Requires an intermediate progress snapshot, unchanged destination, no staging residue, and exact recovery.
TEST_CASE(flzip_extraction_progress_cancels_before_publication_and_recovers) {
    const auto root = test_temp_dir("flzip-extraction-window-cancel");
    const auto source = root / "source.txt";
    const auto archive = root / "output.flzip";
    const std::string payload(3U * flitzzip::kMinArchiveBlockBytes + 1U, 'E');
    std::ofstream(source, std::ios::binary) << payload;
    auto compression = verification_compress_options();
    compression.verify_after_write = false;
    static_cast<void>(flitzzip::compress_flzip({source}, archive, compression));
    const auto archive_bytes = read_verification_fixture(archive);

    flitzzip::ExtractOptions extraction;
    extraction.gpu_required = false;
    extraction.force_cpu = true;
    extraction.overwrite = true;
    extraction.chunk_size = flitzzip::kMinArchiveBlockBytes;
    extraction.block_size = flitzzip::kMinArchiveBlockBytes;
    extraction.worker_count = 2;
    extraction.max_inflight_chunks = 1;
    for (const bool existing : {false, true}) {
        const auto destination = root / (existing ? "existing" : "new");
        const auto target = destination / source.filename();
        std::filesystem::create_directory(destination);
        if (existing) {
            std::ofstream(target, std::ios::binary) << "original output";
        }
        bool cancelled = false;
        try {
            static_cast<void>(flitzzip::extract_flzip(
                archive, destination, extraction, [&](const flitzzip::ProgressSnapshot& snapshot) {
                    if (snapshot.processed_bytes == 0 || snapshot.processed_bytes == snapshot.total_bytes) {
                        return;
                    }
                    REQUIRE_EQ(snapshot.current_entry, "source.txt");
                    REQUIRE_EQ(std::filesystem::exists(target), existing);
                    if (existing) {
                        REQUIRE_EQ(read_verification_fixture(target), "original output");
                    }
                    throw flitzzip::ArchiveError("test extraction cancelled within file");
                }));
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "test extraction cancelled within file");
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(std::filesystem::exists(target), existing);
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
            existing ? 1 : 0);
        if (existing) {
            REQUIRE_EQ(read_verification_fixture(target), "original output");
        }
        REQUIRE_EQ(read_verification_fixture(archive), archive_bytes);
        REQUIRE_EQ(flitzzip::extract_flzip(archive, destination, extraction).output_bytes, payload.size());
        REQUIRE_EQ(read_verification_fixture(target), payload);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Expose bounded decode progress so a large entry does not defer cancellation until its final byte.
// Inputs: A four-window CPU archive verified without extracting files.
// Outputs: Requires monotonic counters, complete terminal progress, and an observable intermediate window.
TEST_CASE(flzip_verification_reports_each_decode_window) {
    const auto root = test_temp_dir("flzip-verification-windows");
    const auto source = root / "source.txt";
    const auto archive = root / "output.flzip";
    std::ofstream(source, std::ios::binary) << std::string(3U * flitzzip::kMinArchiveBlockBytes + 1U, 'W');
    auto compression = verification_compress_options();
    compression.verify_after_write = false;
    static_cast<void>(flitzzip::compress_flzip({source}, archive, compression));
    flitzzip::ExtractOptions options;
    options.gpu_required = false;
    options.force_cpu = true;
    options.chunk_size = flitzzip::kMinArchiveBlockBytes;
    options.block_size = flitzzip::kMinArchiveBlockBytes;
    options.worker_count = 2;
    options.max_inflight_chunks = 0;
    std::vector<flitzzip::ProgressSnapshot> snapshots;
    const auto stats = flitzzip::verify_flzip(archive, options, [&](const auto& snapshot) {
        REQUIRE_EQ(snapshot.operation, flitzzip::OperationKind::Verify);
        snapshots.push_back(snapshot);
    });
    REQUIRE_TRUE(!snapshots.empty());
    REQUIRE_EQ(snapshots.front().processed_bytes, 0U);
    bool intermediate = false;
    std::uint64_t previous = 0;
    for (const auto& snapshot : snapshots) {
        REQUIRE_TRUE(snapshot.processed_bytes >= previous);
        REQUIRE_TRUE(snapshot.processed_bytes <= snapshot.total_bytes);
        REQUIRE_TRUE(snapshot.processed_bytes - previous <= flitzzip::kMinArchiveBlockBytes);
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
TEST_CASE(flzip_verification_empty_archives_report_progress) {
    for (const bool directory : {false, true}) {
        const auto root = test_temp_dir("flzip-verification-empty");
        const auto archive = root / "empty.flzip";
        flitzzip::ArchiveIndex index;
        if (directory) {
            index.entries.push_back({.path = "empty", .directory = true});
        }
        {
            std::ofstream output(archive, std::ios::binary);
            flitzzip::write_archive_index(output, index);
            const auto index_size = static_cast<std::uint64_t>(output.tellp());
            flitzzip_test::write_test_footer(output, 0, index_size);
            REQUIRE_TRUE(output.good());
        }
        flitzzip::ExtractOptions options;
        options.gpu_required = false;
        options.force_cpu = true;
        std::vector<flitzzip::ProgressSnapshot> snapshots;
        static_cast<void>(
            flitzzip::verify_flzip(archive, options, [&](const auto& snapshot) { snapshots.push_back(snapshot); }));
        REQUIRE_TRUE(!snapshots.empty());
        REQUIRE_EQ(snapshots.front().processed_bytes, 0U);
        REQUIRE_EQ(snapshots.back().completed_entries, index.entries.size());
        REQUIRE_EQ(snapshots.back().processed_bytes, snapshots.back().total_bytes);
        bool cancelled = false;
        try {
            static_cast<void>(flitzzip::verify_flzip(
                archive, options, [](const auto&) { throw flitzzip::ArchiveError("empty verification cancelled"); }));
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "empty verification cancelled");
            cancelled = true;
        }
        REQUIRE_TRUE(cancelled);
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}), 1);
        std::filesystem::remove_all(root);
    }
}
