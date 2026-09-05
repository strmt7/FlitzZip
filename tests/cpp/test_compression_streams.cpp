#include "test_util.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"
#include "gzip/gzip_adapter.hpp"
#include "zstd/zstd_stream.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <limits>

namespace {

// Purpose: Read complete bounded fixture bytes and reject missing or unreadable fixture files.
// Inputs: Path to a test-owned small file.
// Outputs: Returns file contents or throws on open/read failure.
std::string read_stream_fixture(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE_TRUE(file.is_open());
    file.exceptions(std::ios::badbit);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Purpose: Verify invalid effort is rejected before any destination is opened or truncated.
// Inputs: An output-stream type and a fixed test-owned codec label.
// Outputs: Requires all invalid levels to preserve existing bytes and leave missing destinations absent.
template <typename OutputStream> void require_non_destructive_stream_admission(const std::string& codec) {
    const auto root = test_temp_dir("stream-admission-" + codec);
    const std::string sentinel = "existing file must survive invalid compression options";
    for (const int level : {0, 10, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        for (const bool exists : {true, false}) {
            const auto path = root / (std::to_string(level) + (exists ? "-existing" : "-missing"));
            if (exists) {
                std::ofstream file(path, std::ios::binary);
                file.exceptions(std::ios::badbit | std::ios::failbit);
                file.write(sentinel.data(), static_cast<std::streamsize>(sentinel.size()));
                file.close();
            }
            bool rejected = false;
            try {
                OutputStream output(path, level);
            } catch (const superzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            if (exists) {
                REQUIRE_EQ(read_stream_fixture(path), sentinel);
            } else {
                REQUIRE_TRUE(!std::filesystem::exists(path));
            }
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Create a mixed binary fixture with embedded NUL bytes, repeated ranges, and stream-buffer tails.
// Inputs: None; size stays below 257 KiB.
// Outputs: Returns immutable test input independent of filesystem or host load.
std::string make_stream_partition_fixture() {
    std::string input(256U * 1024U + 17U, '\0');
    std::uint32_t state = 0x17362549U;
    for (std::size_t index = 0; index < input.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        input[index] =
            index >= 16384U && index % 4096U < 2048U ? input[index % 16384U] : static_cast<char>(state >> 24U);
    }
    return input;
}

// Purpose: Write one valid codec stream, verify byte counts and close semantics, and reject writes after close.
// Inputs: Output type, test-owned path, immutable input, level 1-9, and whole versus fragmented write selection.
// Outputs: Requires accurate counters, safe caller-buffer reuse, idempotent close, and unchanged post-close bytes.
template <typename OutputStream>
void write_partitioned_stream(const std::filesystem::path& path, const std::string& input, int level, bool fragmented) {
    OutputStream output(path, level);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    constexpr std::array<std::size_t, 6> partitions{1U, 3U, 8191U, 65535U, 65536U, 65537U};
    std::vector<char> scratch(partitions.back());
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < input.size()) {
        const auto count =
            std::min(input.size() - offset, fragmented ? partitions[partition++ % partitions.size()] : input.size());
        if (fragmented) {
            std::copy_n(input.data() + offset, count, scratch.data());
            output.write(scratch.data(), static_cast<std::streamsize>(count));
            std::fill(scratch.begin(), scratch.end(), static_cast<char>(0xA5));
        } else {
            output.write(input.data() + offset, static_cast<std::streamsize>(count));
        }
        offset += count;
    }
    output.close();
    output.close();
    REQUIRE_EQ(output.input_bytes(), input.size());
    REQUIRE_EQ(output.output_bytes(), std::filesystem::file_size(path));
    const auto before = read_stream_fixture(path);
    bool rejected = false;
    try {
        output.put('x');
    } catch (const std::exception&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(output.input_bytes(), input.size());
    REQUIRE_EQ(read_stream_fixture(path), before);
}

// Purpose: Prove shared CPU-stream behavior across all supported efforts and caller write partitioning.
// Inputs: Matching output/input codec types and a fixed label for disposable files.
// Outputs: Requires byte-identical encodings, exact full decoding, and valid empty streams for levels 1-9.
template <typename OutputStream, typename InputStream>
void require_consistent_stream_partitioning(const std::string& codec) {
    const auto root = test_temp_dir("stream-partition-" + codec);
    const auto input = make_stream_partition_fixture();
    for (int level = 1; level <= 9; ++level) {
        const auto whole = root / (std::to_string(level) + "-whole");
        const auto fragmented = root / (std::to_string(level) + "-fragmented");
        write_partitioned_stream<OutputStream>(whole, input, level, false);
        write_partitioned_stream<OutputStream>(fragmented, input, level, true);
        REQUIRE_EQ(read_stream_fixture(whole), read_stream_fixture(fragmented));
        for (const auto& path : {whole, fragmented}) {
            InputStream decoded(path);
            const std::string restored{std::istreambuf_iterator<char>(decoded), std::istreambuf_iterator<char>()};
            REQUIRE_EQ(restored, input);
        }
        const auto empty = root / (std::to_string(level) + "-empty");
        write_partitioned_stream<OutputStream>(empty, {}, level, true);
        InputStream decoded(empty);
        REQUIRE_EQ(decoded.peek(), std::char_traits<char>::eof());
    }
    std::filesystem::remove_all(root);
}

}  // namespace

// Purpose: Protect Gzip destinations when compression options are rejected.
// Inputs: Existing and missing disposable destinations with invalid levels.
// Outputs: Requires no destination changes.
TEST_CASE(compression_stream_gzip_invalid_effort_preserves_output) {
    require_non_destructive_stream_admission<superzip::GzipOutputStream>("gzip");
}

// Purpose: Protect Bzip2 destinations when compression options are rejected.
// Inputs: Existing and missing disposable destinations with invalid levels.
// Outputs: Requires no destination changes.
TEST_CASE(compression_stream_bzip2_invalid_effort_preserves_output) {
    require_non_destructive_stream_admission<superzip::Bzip2OutputStream>("bzip2");
}

// Purpose: Protect Zstandard destinations when compression options are rejected.
// Inputs: Existing and missing disposable destinations with invalid levels.
// Outputs: Requires no destination changes.
TEST_CASE(compression_stream_zstd_invalid_effort_preserves_output) {
    require_non_destructive_stream_admission<superzip::ZstdOutputStream>("zstd");
}

// Purpose: Verify Gzip stream partitioning, effort, framing, and close behavior together.
// Inputs: All nine levels, mixed binary writes, and empty streams.
// Outputs: Requires consistent encoding and byte-exact decoding.
TEST_CASE(compression_stream_gzip_partition_and_effort_parity) {
    require_consistent_stream_partitioning<superzip::GzipOutputStream, superzip::GzipInputStream>("gzip");
}

// Purpose: Verify Bzip2 stream partitioning, effort, framing, and close behavior together.
// Inputs: All nine levels, mixed binary writes, and empty streams.
// Outputs: Requires consistent encoding and byte-exact decoding.
TEST_CASE(compression_stream_bzip2_partition_and_effort_parity) {
    require_consistent_stream_partitioning<superzip::Bzip2OutputStream, superzip::Bzip2InputStream>("bzip2");
}

// Purpose: Verify Zstandard stream partitioning, effort, framing, and close behavior together.
// Inputs: All nine levels, mixed binary writes, and empty streams.
// Outputs: Requires consistent encoding and byte-exact decoding.
TEST_CASE(compression_stream_zstd_partition_and_effort_parity) {
    require_consistent_stream_partitioning<superzip::ZstdOutputStream, superzip::ZstdInputStream>("zstd");
}

// Purpose: Establish byte-for-byte equivalence between standalone Gzip and the stream used by container wrappers.
// Inputs: Empty and mixed binary files at every supported effort; outputs are bounded temporary fixtures.
// Outputs: Requires complete encoded-byte identity and matching standalone telemetry before and after consolidation.
TEST_CASE(compression_stream_gzip_adapter_wire_parity) {
    const auto root = test_temp_dir("gzip-adapter-stream-parity");
    for (const auto& input : {std::string{}, make_stream_partition_fixture()}) {
        const auto source = root / "source.bin";
        {
            std::ofstream file(source, std::ios::binary);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(input.data(), static_cast<std::streamsize>(input.size()));
            file.close();
        }
        for (int level = 1; level <= 9; ++level) {
            const auto archive = root / "standalone.gz";
            const auto stream = root / "stream.gz";
            const auto stats = superzip::compress_gzip_file(source, archive, level);
            write_partitioned_stream<superzip::GzipOutputStream>(stream, input, level, true);
            REQUIRE_EQ(read_stream_fixture(archive), read_stream_fixture(stream));
            REQUIRE_EQ(stats.input_bytes, input.size());
            REQUIRE_EQ(stats.output_bytes, std::filesystem::file_size(archive));
            REQUIRE_TRUE(!stats.gpu_used);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve atomic publication and temporary cleanup when a Gzip progress callback interrupts compression.
// Inputs: A bounded source, existing destination sentinel, and callback that throws after the first processed bytes.
// Outputs: Requires the exception to propagate, the destination to stay unchanged, and temporary state to be removed.
TEST_CASE(compression_stream_gzip_callback_failure_preserves_output) {
    const auto root = test_temp_dir("gzip-callback-failure");
    const auto source = root / "source.bin";
    const auto archive = root / "destination.gz";
    const auto input = make_stream_partition_fixture();
    const std::string sentinel = "existing destination";
    for (const auto& [path, bytes] : {std::pair{source, input}, std::pair{archive, sentinel}}) {
        std::ofstream file(path, std::ios::binary);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
    }
    bool interrupted = false;
    try {
        (void)superzip::compress_gzip_file(source, archive, 5, [](const superzip::ProgressSnapshot& progress) {
            if (progress.processed_bytes > 0U) {
                throw std::runtime_error("test progress interruption");
            }
        });
    } catch (const std::runtime_error& error) {
        interrupted = std::string(error.what()) == "test progress interruption";
    }
    REQUIRE_TRUE(interrupted);
    REQUIRE_EQ(read_stream_fixture(archive), sentinel);
    REQUIRE_EQ(read_stream_fixture(source), input);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator()), 2);
    std::filesystem::remove_all(root);
}
