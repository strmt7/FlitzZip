#include "test_compat_fixture.hpp"
#include "test_util.hpp"
#include "test_stream_failure.hpp"
#include "test_source_session.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "core/path_text.hpp"
#include "lzip/lzip_adapter.hpp"
#include "lzip/lzip_stream.hpp"
#include "tar/tar_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <future>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char kSingleFileLzipPayload[] = "Example lzip fixture payload.\nSecond line.\n";

// Independent empty member: unmodified public-domain LZMA SDK 26.03 raw encoder,
// framed with the LZIP v1 header, empty CRC32, zero output size and exact member size.
constexpr std::array<unsigned char, 36> kEmptyLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x0CU, 0x00U, 0x83U, 0xFFU, 0xFBU, 0xFFU, 0xFFU,
    0xC0U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
}};

// Independent fixture: Python 3.13 liblzma raw LZMA1 with a size/CRC-verified LZIP v1 container.
constexpr std::array<unsigned char, 78> kSingleFileLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x14U, 0x00U, 0x22U, 0x9EU, 0x08U, 0x26U, 0xFFU, 0x96U, 0x34U, 0x91U, 0xE7U,
    0x9EU, 0x69U, 0x36U, 0x40U, 0x96U, 0xB7U, 0x1AU, 0xCEU, 0xEBU, 0x67U, 0xBCU, 0x99U, 0x6DU, 0x83U, 0x53U, 0x0CU,
    0x3AU, 0x97U, 0x22U, 0x08U, 0x78U, 0xB1U, 0xB0U, 0x85U, 0x3EU, 0xEEU, 0x1EU, 0x51U, 0xA6U, 0x25U, 0x57U, 0x6DU,
    0x13U, 0x60U, 0x7AU, 0x88U, 0x1FU, 0xFFU, 0xC7U, 0x5EU, 0xC0U, 0x00U, 0xB1U, 0xC4U, 0x4AU, 0x61U, 0x2BU, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x4EU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
}};

constexpr std::array<unsigned char, 172> kTarLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x14U, 0x00U, 0x32U, 0x1AU, 0x4AU, 0xA4U, 0x14U, 0xD4U, 0x01U, 0xC9U, 0x6AU,
    0xAEU, 0x77U, 0x6EU, 0x91U, 0xBBU, 0x40U, 0x50U, 0xF6U, 0xF9U, 0x33U, 0xEFU, 0xE6U, 0xA6U, 0x45U, 0x06U, 0x00U,
    0xA4U, 0xFBU, 0x0DU, 0xF1U, 0xCBU, 0xB5U, 0xD8U, 0xFDU, 0x3EU, 0x76U, 0x36U, 0xBDU, 0x6FU, 0xF8U, 0x21U, 0x34U,
    0x57U, 0x24U, 0x56U, 0x19U, 0x6EU, 0x95U, 0xE7U, 0x20U, 0xDBU, 0xD4U, 0x9BU, 0xFCU, 0x81U, 0x35U, 0x16U, 0xFEU,
    0xE9U, 0xDDU, 0x2EU, 0x8EU, 0xF1U, 0xC3U, 0x68U, 0xECU, 0xCBU, 0x9AU, 0x9EU, 0x99U, 0x8EU, 0xFEU, 0x68U, 0x1CU,
    0xEAU, 0x74U, 0x49U, 0xD0U, 0xE1U, 0xACU, 0x09U, 0x6FU, 0xB9U, 0x13U, 0x1AU, 0xDEU, 0x66U, 0xF7U, 0x2DU, 0x2BU,
    0x1BU, 0xDAU, 0x9CU, 0xDDU, 0x3FU, 0x3BU, 0x04U, 0xC9U, 0x6EU, 0xBFU, 0x3BU, 0x20U, 0xB5U, 0xD6U, 0xD7U, 0x62U,
    0x29U, 0x34U, 0x22U, 0x1AU, 0x05U, 0x09U, 0xC8U, 0x91U, 0x93U, 0xB8U, 0x75U, 0xE5U, 0x56U, 0x55U, 0xD4U, 0xF6U,
    0xD6U, 0xBEU, 0x7DU, 0x05U, 0x36U, 0x33U, 0xFEU, 0xB6U, 0xCCU, 0x65U, 0x2BU, 0xF9U, 0x43U, 0xCAU, 0xA2U, 0xC2U,
    0x05U, 0x90U, 0xFFU, 0xFFU, 0xDDU, 0xF3U, 0xF9U, 0x1BU, 0x80U, 0x60U, 0xD5U, 0xD3U, 0x00U, 0x0EU, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0xACU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
}};

constexpr std::array<unsigned char, 104> kUnsafeTarLzipFixture{{
    0x4CU, 0x5AU, 0x49U, 0x50U, 0x01U, 0x14U, 0x00U, 0x17U, 0x60U, 0xB8U, 0xCAU, 0x73U, 0x30U, 0x94U, 0xF6U,
    0x84U, 0x5EU, 0x11U, 0x4EU, 0xE8U, 0x52U, 0xFDU, 0xE4U, 0xA5U, 0x5CU, 0x7CU, 0x19U, 0x45U, 0x22U, 0x67U,
    0x88U, 0xA1U, 0xAEU, 0xE9U, 0x75U, 0x6FU, 0x9AU, 0x2FU, 0x98U, 0xADU, 0xC1U, 0x5FU, 0x5EU, 0xADU, 0x8AU,
    0xDBU, 0x1FU, 0x37U, 0xF5U, 0x8DU, 0x15U, 0x67U, 0x1FU, 0xCCU, 0xE1U, 0x07U, 0x02U, 0xB3U, 0x52U, 0x32U,
    0x19U, 0xB8U, 0xBCU, 0xB6U, 0x1CU, 0x81U, 0x26U, 0xD5U, 0xF9U, 0xE8U, 0x60U, 0x94U, 0x11U, 0x6CU, 0x36U,
    0x98U, 0x94U, 0x39U, 0xDFU, 0xFFU, 0xFBU, 0xD0U, 0x46U, 0x00U, 0x1AU, 0x36U, 0x71U, 0x7DU, 0x00U, 0x08U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x68U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
}};

// Purpose: Write exact fixture bytes to disk.
// Inputs: `path` is the destination and `bytes` contains a complete lzip stream.
// Outputs: Creates or replaces the file.
template <std::size_t Size>
void write_fixture(const std::filesystem::path& path, const std::array<unsigned char, Size>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write exact binary bytes to disk.
// Inputs: `path` is the destination and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Read a full text file for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for no-output assertions.
// Inputs: `root` is the directory tree to inspect.
// Outputs: Returns the number of regular files; missing roots count as zero.
std::uint64_t count_regular_files(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        return 0;
    }
    std::uint64_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

// Purpose: Convert the single lzip fixture to a mutable byte vector.
// Inputs: None.
// Outputs: Returns a copy suitable for corruption tests.
std::vector<unsigned char> single_lzip_bytes() {
    return std::vector<unsigned char>(kSingleFileLzipFixture.begin(), kSingleFileLzipFixture.end());
}

}  // namespace

// Purpose: Preserve real lzip decoding with interleaved streams and sequential cross-thread ownership.
// Inputs: Two independent stream objects over one verified fixture; the first is destroyed out of open order.
// Outputs: Requires exact payload and trailer verification after the remaining stream is handed to another thread.
TEST_CASE(lzip_interleaved_streams_allow_sequential_thread_handoff) {
    const auto root = test_temp_dir("lzip-stream-owner");
    const auto archive = root / "payload.lz";
    write_fixture(archive, kSingleFileLzipFixture);
    {
        auto first = std::make_unique<flitzzip::LzipInputStream>(archive);
        auto second = std::make_unique<flitzzip::LzipInputStream>(archive);
        REQUIRE_EQ(first->get(), static_cast<int>(kSingleFileLzipPayload[0]));
        REQUIRE_EQ(second->get(), static_cast<int>(kSingleFileLzipPayload[0]));
        first.reset();
        auto decoded = std::async(std::launch::async, [&] {
            const std::string payload{std::istreambuf_iterator<char>(*second), std::istreambuf_iterator<char>()};
            second->finish();
            return payload;
        });
        REQUIRE_EQ(decoded.get(), std::string(kSingleFileLzipPayload + 1));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Report the actual missing lzip source instead of masking it with an ANSI conversion failure.
// Inputs: A nonexistent archive path containing supplementary Unicode characters.
// Outputs: Requires an ArchiveError containing the exact UTF-8 diagnostic path.
TEST_CASE(lzip_missing_unicode_source_reports_original_error) {
    const auto root = test_temp_dir("lzip-missing-unicode");
    const auto archive = root / std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.lz");
    bool rejected = false;
    try {
        flitzzip::LzipInputStream input(archive);
    } catch (const flitzzip::ArchiveError& error) {
        rejected =
            std::string_view(error.what()).find(flitzzip::path_diagnostic_utf8(archive)) != std::string_view::npos;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}

// Purpose: Reject a failed lzip trailer even when later bytes could satisfy a retried validation.
// Inputs: A CRC-corrupted member followed by a compensating trailer with the larger member size.
// Outputs: Requires no output, overwrite preservation, compressed-TAR rejection, and sticky decoder errors.
TEST_CASE(lzip_resumable_error_never_publishes) {
    const auto root = test_temp_dir("lzip-resumable-error");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    bytes[bytes.size() - 20U] ^= 1U;
    bytes.insert(bytes.end(), kSingleFileLzipFixture.end() - 20, kSingleFileLzipFixture.end());
    bytes[bytes.size() - 8U] = static_cast<unsigned char>(bytes.size());
    write_binary_file(archive, bytes);
    for (const bool overwrite : {false, true}) {
        const auto output = root / (overwrite ? "existing" : "new");
        if (overwrite) {
            std::filesystem::create_directories(output);
            std::ofstream(output / "payload.txt") << "preserve me";
        }
        bool rejected = false;
        try {
            (void)flitzzip::extract_lzip_file(archive, output, overwrite);
        } catch (const flitzzip::Error&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        if (overwrite) {
            REQUIRE_EQ(read_text_file(output / "payload.txt"), "preserve me");
        } else {
            REQUIRE_EQ(count_regular_files(output), 0U);
        }
    }
    bool tar_rejected = false;
    try {
        (void)flitzzip::extract_tar_lzip(archive, root / "tar", false);
    } catch (const flitzzip::Error&) {
        tar_rejected = true;
    }
    REQUIRE_TRUE(tar_rejected);
    REQUIRE_EQ(count_regular_files(root / "tar"), 0U);
    require_sticky_decoder_failure<flitzzip::LzipInputStream>(archive);
}

// Purpose: Verify native `.lz` extraction over a deterministic lzip fixture.
// Inputs: A small lzip stream generated from one payload.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(lzip_extracts_single_file_fixture) {
    const auto root = test_temp_dir("lzip-single");
    const auto archive = root / "payload.txt.lz";
    write_fixture(archive, kSingleFileLzipFixture);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Lzip);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_lzip_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), kSingleFileLzipPayload);
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Keep independently encoded lzip input immutable through successful and interrupted extraction.
// Inputs: A small verified lzip fixture and real OS sharing probes; no source bytes are changed.
// Outputs: Refused active/concurrent writers, exact decoded bytes and source-lock release on every tested exit.
TEST_CASE(single_stream_source_session_lzip) {
    const auto root = test_temp_dir("lzip-source-session");
    const auto archive = root / "payload.lz";
    write_fixture(archive, kSingleFileLzipFixture);
    flitzzip_test::require_archive_source_session(archive, root, "payload", kSingleFileLzipPayload,
                                                  &flitzzip::extract_lzip_file);
}

// Purpose: Preserve native Unicode filenames while extracting independent lzip fixtures.
// Inputs: A bounded upstream-compatible fixture with an uppercase suffix and a Unicode host filename.
// Outputs: Requires exact restored bytes and overwrite refusal without encoding errors.
TEST_CASE(lzip_unicode_archive_filename_extracts) {
    const auto root = test_temp_dir("lzip-unicode-name");
    const auto filename = std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.txt");
    auto archive = root / filename;
    archive += ".LZ";
    write_fixture(archive, kSingleFileLzipFixture);
    const auto output = root / "out";
    (void)flitzzip::extract_lzip_file(archive, output, false);
    REQUIRE_EQ(read_text_file(output / filename), kSingleFileLzipPayload);
    bool refused = false;
    try {
        (void)flitzzip::extract_lzip_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(read_text_file(output / filename), kSingleFileLzipPayload);
}

// Purpose: Verify concatenated lzip members decode as one continuous single-file payload.
// Inputs: Two valid lzip members written back-to-back.
// Outputs: Throws if the decoder fails to validate both members or drops member data.
TEST_CASE(lzip_extracts_concatenated_members) {
    const auto root = test_temp_dir("lzip-concat");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    bytes.insert(bytes.end(), kSingleFileLzipFixture.begin(), kSingleFileLzipFixture.end());
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_lzip_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), std::string(kSingleFileLzipPayload) + kSingleFileLzipPayload);
}

// Purpose: Preserve the legal single empty lzip stream while enforcing the multi-member empty-file rule.
// Inputs: Independent empty and nonempty members in first, middle, last and all-empty arrangements.
// Outputs: Invalid concatenations reject persistently without changing existing files; valid streams remain reusable.
TEST_CASE(lzip_empty_member_position_is_validated_before_publication) {
    const auto root = test_temp_dir("lzip-empty-member-position");
    const auto archive = root / "payload.txt.lz";
    const auto output = root / "out";
    write_fixture(archive, kEmptyLzipFixture);
    const auto empty_stats = flitzzip::extract_lzip_file(archive, output, false);
    REQUIRE_EQ(empty_stats.entries, 1U);
    REQUIRE_EQ(empty_stats.output_bytes, 0U);
    REQUIRE_TRUE(std::filesystem::is_regular_file(output / "payload.txt"));
    REQUIRE_EQ(std::filesystem::file_size(output / "payload.txt"), 0U);

    const std::vector<std::vector<bool>> arrangements{{true, false}, {false, true}, {false, true, false}, {true, true}};
    for (const auto& arrangement : arrangements) {
        std::vector<unsigned char> bytes;
        for (const bool empty : arrangement) {
            if (empty) {
                bytes.insert(bytes.end(), kEmptyLzipFixture.begin(), kEmptyLzipFixture.end());
            } else {
                bytes.insert(bytes.end(), kSingleFileLzipFixture.begin(), kSingleFileLzipFixture.end());
            }
        }
        write_binary_file(archive, bytes);
        {
            std::ofstream target(output / "payload.txt", std::ios::binary | std::ios::trunc);
            target.exceptions(std::ios::badbit | std::ios::failbit);
            target << "preserve existing bytes";
        }
        bool rejected = false;
        try {
            (void)flitzzip::extract_lzip_file(archive, output, true);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(read_text_file(output / "payload.txt"), "preserve existing bytes");
        REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator()),
                   1);
        require_sticky_decoder_failure<flitzzip::LzipInputStream>(archive);

        write_fixture(archive, kSingleFileLzipFixture);
        (void)flitzzip::extract_lzip_file(archive, output, true);
        REQUIRE_EQ(read_text_file(output / "payload.txt"), kSingleFileLzipPayload);
    }
}

// Purpose: Validate the entire compressed container before publishing any TAR member.
// Inputs: A valid independent TAR.LZ member followed by a forbidden empty lzip member.
// Outputs: Rejects the trailing member, preserves an existing TAR target and leaves no partial extraction.
TEST_CASE(tar_lzip_rejects_empty_final_member_before_output) {
    const auto root = test_temp_dir("tar-lzip-empty-final-member");
    const auto archive = root / "sample.tar.lz";
    std::vector<unsigned char> bytes(kTarLzipFixture.begin(), kTarLzipFixture.end());
    bytes.insert(bytes.end(), kEmptyLzipFixture.begin(), kEmptyLzipFixture.end());
    write_binary_file(archive, bytes);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream target(output / "root.txt", std::ios::binary);
        target.exceptions(std::ios::badbit | std::ios::failbit);
        target << "preserve TAR target";
    }
    bool rejected = false;
    try {
        (void)flitzzip::extract_tar_lzip(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "root.txt"), "preserve TAR target");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator()), 1);

    write_fixture(archive, kTarLzipFixture);
    (void)flitzzip::extract_tar_lzip(archive, output, true);
    REQUIRE_EQ(read_text_file(output / "root.txt"), "root tar lzip payload");
    REQUIRE_EQ(read_text_file(output / "dir" / "hello.txt"), "hello from tar.lz\n");
}

// Purpose: Verify `.lz` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid lzip stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(lzip_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("lzip-overwrite");
    const auto archive = root / "payload.txt.lz";
    write_fixture(archive, kSingleFileLzipFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream(output / "payload.txt") << "old";
    }

    bool rejected = false;
    try {
        (void)flitzzip::extract_lzip_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "old");
}

// Purpose: Verify lzip CRC mismatches do not publish partial output.
// Inputs: A valid lzip stream with one trailer CRC byte corrupted.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzip_rejects_crc_mismatch_without_output) {
    const auto root = test_temp_dir("lzip-crc");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    REQUIRE_TRUE(bytes.size() > 20U);
    bytes[bytes.size() - 20U] ^= 0xFFU;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzip_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify lzip member-size mismatches fail closed.
// Inputs: A valid lzip stream with the trailer member-size field corrupted.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzip_rejects_member_size_mismatch_without_output) {
    const auto root = test_temp_dir("lzip-member-size");
    const auto archive = root / "payload.txt.lz";
    auto bytes = single_lzip_bytes();
    REQUIRE_TRUE(bytes.size() > 8U);
    bytes[bytes.size() - 1U] ^= 0x01U;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzip_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify hostile lzip dictionary-size codes are rejected before decoder allocation.
// Inputs: A lzip header with DS below the specification's minimum base logarithm.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzip_rejects_invalid_dictionary_code_without_output) {
    const auto root = test_temp_dir("lzip-dictionary");
    const auto archive = root / "bad.lz";
    write_binary_file(archive, {'L', 'Z', 'I', 'P', 1U, 0U, 0U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzip_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Check malformed members through real extraction and its complete publication cleanup.
// Inputs: Bounded independent fixture bytes, a test-owned root and an optional required diagnostic fragment.
// Outputs: Rejects new and overwrite extraction, retaining old bytes and no staging objects.
void require_lzip_fixture_rejected(const std::vector<unsigned char>& bytes, const std::filesystem::path& root,
                                   std::string_view diagnostic = {}) {
    const auto archive = root / "input.txt.lz";
    write_binary_file(archive, bytes);
    for (const bool overwrite : {false, true}) {
        const auto destination = root / (overwrite ? "existing" : "new");
        const auto target = destination / "input.txt";
        if (overwrite) {
            std::filesystem::create_directories(destination);
            std::ofstream(target, std::ios::binary) << "retain original";
        }
        std::string error;
        try {
            (void)flitzzip::extract_lzip_file(archive, destination, overwrite);
        } catch (const flitzzip::Error& failure) {
            error = failure.what();
        }
        REQUIRE_TRUE(!error.empty());
        REQUIRE_TRUE(error.find(diagnostic) != std::string::npos);
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(destination)) {
            REQUIRE_TRUE(overwrite);
            REQUIRE_EQ(entry.path(), target);
            REQUIRE_TRUE(entry.is_regular_file());
            ++entries;
        }
        REQUIRE_EQ(entries, overwrite ? 1U : 0U);
        if (overwrite)
            REQUIRE_EQ(read_text_file(target), "retain original");
    }
}

// Purpose: Reject every incomplete member boundary, including late failures after valid decoded bytes.
// Inputs: Every strict prefix of an independent member, alone and following one complete nonempty member.
// Outputs: Never publishes partial data or replaces an existing output, even after decoding a complete first member.
TEST_CASE(lzip_every_member_truncation_preserves_existing_output) {
    const auto root = test_temp_dir("lzip-every-truncation");
    for (std::size_t length = 0; length < kSingleFileLzipFixture.size(); ++length) {
        const std::vector<unsigned char> prefix(kSingleFileLzipFixture.begin(),
                                                kSingleFileLzipFixture.begin() + static_cast<std::ptrdiff_t>(length));
        require_lzip_fixture_rejected(prefix, root);
        if (length != 0) {
            auto concatenated = single_lzip_bytes();
            concatenated.insert(concatenated.end(), prefix.begin(), prefix.end());
            require_lzip_fixture_rejected(concatenated, root);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Check all dictionary codes outside the published format range without allocating large dictionaries.
// Inputs: Every DS byte whose base is outside 12..29 or whose effective dictionary is below 4096 bytes.
// Outputs: Rejects with a dictionary diagnostic before publication for both overwrite policies.
TEST_CASE(lzip_invalid_dictionary_domain_is_rejected_without_publication) {
    const auto root = test_temp_dir("lzip-dictionary-domain");
    for (unsigned int code = 0; code <= 255U; ++code) {
        const auto base = code & 31U;
        if (base >= 12U && base <= 29U && !(base == 12U && (code >> 5U) != 0U))
            continue;
        auto bytes = single_lzip_bytes();
        bytes[5] = static_cast<unsigned char>(code);
        require_lzip_fixture_rejected(bytes, root, "dictionary");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve valid fractional dictionary encodings while enforcing the lower size boundary.
// Inputs: All valid DS encodings up to 128 KiB and a fixture whose entire decoded history fits in 4096 bytes.
// Outputs: Exact independent payload and byte telemetry for each admitted encoding, with bounded decoder memory.
TEST_CASE(lzip_fractional_dictionary_codes_preserve_payload) {
    const auto root = test_temp_dir("lzip-fractional-dictionaries");
    const auto archive = root / "input.txt.lz";
    for (unsigned int base = 12U; base <= 17U; ++base) {
        for (unsigned int numerator = 0; numerator <= 7U; ++numerator) {
            if (base == 12U && numerator != 0U)
                continue;
            auto bytes = single_lzip_bytes();
            bytes[5] = static_cast<unsigned char>(base | (numerator << 5U));
            write_binary_file(archive, bytes);
            const auto destination = root / "output";
            const auto stats = flitzzip::extract_lzip_file(archive, destination, true);
            REQUIRE_EQ(read_text_file(destination / "input.txt"), std::string(kSingleFileLzipPayload));
            REQUIRE_EQ(stats.input_bytes, bytes.size());
            REQUIRE_EQ(stats.output_bytes, sizeof(kSingleFileLzipPayload) - 1U);
            REQUIRE_EQ(stats.entries, 1U);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject invalid framing and trailer counters independently of CRC-only corruption.
// Inputs: Mutated magic/version, invalid LZMA start, wrong output size, and over-limit member size.
// Outputs: Rejects both first and subsequent malformed members without publishing any decoded prefix.
TEST_CASE(lzip_invalid_framing_and_size_fields_never_publish) {
    const auto root = test_temp_dir("lzip-framing-and-sizes");
    for (unsigned int mutation = 0; mutation < 10U; ++mutation) {
        auto bytes = single_lzip_bytes();
        if (mutation < 4U) {
            bytes[mutation] ^= 1U;
        } else if (mutation < 7U) {
            bytes[4] = std::array<unsigned char, 3>{0U, 2U, 255U}[mutation - 4U];
        } else if (mutation == 7U) {
            bytes[6] = 255U;
        } else if (mutation == 8U) {
            std::fill(bytes.end() - 16, bytes.end() - 8, 255U);
        } else {
            std::fill(bytes.end() - 8, bytes.end(), 255U);
        }
        require_lzip_fixture_rejected(bytes, root);
        auto concatenated = single_lzip_bytes();
        concatenated.insert(concatenated.end(), bytes.begin(), bytes.end());
        require_lzip_fixture_rejected(concatenated, root);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify in-process `.tar.lz` compatibility extraction for files and directories.
// Inputs: A deterministic lzip-compressed TAR fixture.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(tar_lzip_extracts_files_and_directories) {
    const auto root = test_temp_dir("tar-lzip-extract");
    const auto archive = root / "sample.tar.lz";
    write_fixture(archive, kTarLzipFixture);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::TarLzip);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_tar_lzip(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(3));
    REQUIRE_EQ(read_text_file(output / "dir" / "hello.txt"), "hello from tar.lz\n");
    REQUIRE_EQ(read_text_file(output / "root.txt"), "root tar lzip payload");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Verify `.tar.lz` extraction rejects traversal metadata during the validation pass.
// Inputs: A deterministic TAR.LZ fixture containing `../escape.txt`.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(tar_lzip_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("tar-lzip-traversal");
    const auto archive = root / "bad.tlz";
    write_fixture(archive, kUnsafeTarLzipFixture);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::TarLzip);

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_tar_lzip(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}
