#include "test_compat_fixture.hpp"
#include "test_util.hpp"
#include "test_decoded_input_transfer.hpp"
#include "test_flzip_helpers.hpp"
#include "test_stream_failure.hpp"
#include "test_source_session.hpp"
#include "test_stream_progress.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "xz/xz_adapter.hpp"
#include "xz/xz_stream.hpp"
#include "tar/tar_adapter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// Independent Python liblzma frames for a three-window binary payload and empty input.
constexpr std::array<unsigned char, 396> kWindowTransferXzFixture{{
    0xFDU, 0x37U, 0x7AU, 0x58U, 0x5AU, 0x00U, 0x00U, 0x04U, 0xE6U, 0xD6U, 0xB4U, 0x46U, 0x02U, 0x00U, 0x21U, 0x01U,
    0x16U, 0x00U, 0x00U, 0x00U, 0x74U, 0x2FU, 0xE5U, 0xA3U, 0xE3U, 0x00U, 0x10U, 0x01U, 0x49U, 0x5DU, 0x00U, 0x00U,
    0x00U, 0x52U, 0x50U, 0x0AU, 0x84U, 0xF9U, 0x9BU, 0xB2U, 0x80U, 0x21U, 0xA9U, 0x69U, 0xD6U, 0x27U, 0xE0U, 0x3EU,
    0x06U, 0x5AU, 0x5FU, 0x04U, 0x8DU, 0x53U, 0xD4U, 0x04U, 0xBAU, 0x39U, 0x57U, 0x05U, 0x09U, 0xC1U, 0x55U, 0x24U,
    0xDEU, 0x9DU, 0xB8U, 0x71U, 0x59U, 0x31U, 0x60U, 0xA1U, 0x9FU, 0xF9U, 0x6FU, 0x49U, 0x73U, 0xF2U, 0xC8U, 0xEAU,
    0x8CU, 0xBAU, 0x1AU, 0x8BU, 0x29U, 0x69U, 0x21U, 0x80U, 0xFEU, 0x33U, 0x83U, 0x66U, 0xAFU, 0x46U, 0x6DU, 0xECU,
    0x9EU, 0x89U, 0x8AU, 0x0BU, 0x83U, 0xF0U, 0x3CU, 0x0EU, 0x89U, 0x8EU, 0x3FU, 0xEDU, 0x5FU, 0xE7U, 0x9EU, 0x90U,
    0xD9U, 0x1CU, 0xFFU, 0x32U, 0xF4U, 0xB2U, 0xE0U, 0x39U, 0x51U, 0xB2U, 0xD2U, 0x14U, 0x15U, 0xB4U, 0xC5U, 0x71U,
    0xBAU, 0xDBU, 0x06U, 0xE3U, 0x79U, 0x9AU, 0x9FU, 0xBBU, 0x38U, 0xC1U, 0xB0U, 0x00U, 0xACU, 0x93U, 0x0BU, 0xAAU,
    0x06U, 0x19U, 0x03U, 0x12U, 0x08U, 0x15U, 0x5BU, 0x9BU, 0xC8U, 0x48U, 0xF0U, 0x32U, 0x2EU, 0xFEU, 0x2DU, 0xA0U,
    0x87U, 0xC8U, 0xF0U, 0xA4U, 0xE0U, 0xD2U, 0x51U, 0xEBU, 0x8DU, 0x67U, 0x56U, 0x92U, 0xB2U, 0x4DU, 0x84U, 0xC5U,
    0xF1U, 0x86U, 0x31U, 0xDFU, 0x6AU, 0x62U, 0x5BU, 0xC2U, 0x79U, 0x2DU, 0xD9U, 0xF7U, 0x3CU, 0x73U, 0xBAU, 0x74U,
    0x74U, 0x07U, 0xD8U, 0x3CU, 0xA9U, 0x56U, 0x22U, 0x24U, 0xA1U, 0x66U, 0xF8U, 0x5AU, 0x84U, 0x5FU, 0x30U, 0x67U,
    0xD2U, 0xF6U, 0x4BU, 0x49U, 0x2EU, 0x7FU, 0x20U, 0xEBU, 0xDBU, 0xF8U, 0x10U, 0x0EU, 0x94U, 0x78U, 0x77U, 0xC7U,
    0x3FU, 0x6BU, 0xEFU, 0xB4U, 0xCDU, 0x95U, 0xE2U, 0x6FU, 0xF6U, 0x44U, 0x6EU, 0x06U, 0xCFU, 0x0BU, 0x82U, 0x1AU,
    0xCBU, 0xDBU, 0x7AU, 0xF0U, 0x57U, 0x8DU, 0x98U, 0xFFU, 0x90U, 0xC0U, 0x3EU, 0xE6U, 0xC1U, 0x12U, 0x41U, 0x75U,
    0xEEU, 0x03U, 0x28U, 0x96U, 0xEBU, 0x13U, 0xFBU, 0xA7U, 0x28U, 0xCCU, 0xAFU, 0x32U, 0xBBU, 0xA4U, 0x0EU, 0x25U,
    0xF2U, 0x58U, 0xB0U, 0xDEU, 0xD8U, 0x56U, 0x1CU, 0x66U, 0xF0U, 0xE2U, 0x1BU, 0x39U, 0x76U, 0xF9U, 0x97U, 0xFFU,
    0x8FU, 0xA3U, 0xC8U, 0x2FU, 0xF4U, 0xADU, 0xF2U, 0xDBU, 0x38U, 0x31U, 0x30U, 0x7AU, 0xC0U, 0x77U, 0x22U, 0x24U,
    0x85U, 0xEAU, 0x02U, 0x04U, 0x02U, 0xA1U, 0x3CU, 0x44U, 0xE5U, 0x21U, 0x33U, 0xEBU, 0x78U, 0xBCU, 0xFEU, 0x62U,
    0x95U, 0x56U, 0x6AU, 0x78U, 0x2DU, 0x83U, 0xFFU, 0x4BU, 0x26U, 0x48U, 0x52U, 0x07U, 0x31U, 0xD7U, 0x3FU, 0x75U,
    0xBDU, 0x95U, 0xD3U, 0xA2U, 0xE9U, 0x02U, 0x94U, 0x58U, 0x9BU, 0x36U, 0x25U, 0x74U, 0x2DU, 0x60U, 0x1AU, 0xBDU,
    0x2EU, 0x5FU, 0xA9U, 0xB7U, 0x8EU, 0x53U, 0xE3U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFDU, 0x36U, 0x1FU, 0xF4U,
    0x59U, 0x1FU, 0x94U, 0x0CU, 0x00U, 0x01U, 0xE5U, 0x02U, 0x91U, 0x80U, 0x0CU, 0x00U, 0xC1U, 0xDFU, 0x60U, 0xB7U,
    0xB1U, 0xC4U, 0x67U, 0xFBU, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x59U, 0x5AU,
}};

constexpr std::array<unsigned char, 32> kEmptyTransferXzFixture{{
    0xFDU, 0x37U, 0x7AU, 0x58U, 0x5AU, 0x00U, 0x00U, 0x04U, 0xE6U, 0xD6U, 0xB4U, 0x46U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x1CU, 0xDFU, 0x44U, 0x21U, 0x1FU, 0xB6U, 0xF3U, 0x7DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x59U, 0x5AU,
}};

// Independent fixture: Python 3.13 liblzma output, verified with 7-Zip 26.03.
constexpr std::array<unsigned char, 100> kSingleFileXzFixture{{
    0xFDU, 0x37U, 0x7AU, 0x58U, 0x5AU, 0x00U, 0x00U, 0x04U, 0xE6U, 0xD6U, 0xB4U, 0x46U, 0x02U, 0x00U, 0x21U,
    0x01U, 0x16U, 0x00U, 0x00U, 0x00U, 0x74U, 0x2FU, 0xE5U, 0xA3U, 0x01U, 0x00U, 0x28U, 0x45U, 0x78U, 0x61U,
    0x6DU, 0x70U, 0x6CU, 0x65U, 0x20U, 0x58U, 0x5AU, 0x20U, 0x66U, 0x69U, 0x78U, 0x74U, 0x75U, 0x72U, 0x65U,
    0x20U, 0x70U, 0x61U, 0x79U, 0x6CU, 0x6FU, 0x61U, 0x64U, 0x2EU, 0x0AU, 0x53U, 0x65U, 0x63U, 0x6FU, 0x6EU,
    0x64U, 0x20U, 0x6CU, 0x69U, 0x6EU, 0x65U, 0x2EU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x46U, 0x0DU, 0xBAU,
    0x04U, 0x20U, 0x0DU, 0xD9U, 0x43U, 0x00U, 0x01U, 0x41U, 0x29U, 0x03U, 0x53U, 0x56U, 0x8BU, 0x1FU, 0xB6U,
    0xF3U, 0x7DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x59U, 0x5AU,
}};

// Purpose: Write exact fixture bytes to disk.
// Inputs: `path` is the destination and `bytes` contains a complete `.xz` stream.
// Outputs: Creates or replaces the file.
template <std::size_t Size>
void write_fixture(const std::filesystem::path& path, const std::array<unsigned char, Size>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write exact unsigned bytes to disk.
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

}  // namespace

// An independent three-window XZ frame exercises cancellation before publication.
TEST_CASE(xz_extract_cancellation_between_output_windows) {
    const auto root = test_temp_dir("xz-window-cancellation");
    const auto archive = root / "payload.xz";
    write_fixture(archive, kWindowTransferXzFixture);
    flitzzip_test::require_stream_window_cancellation(archive, root / "out", "payload",
                                                      flitzzip_test::decoded_transfer_fixture_payload(),
                                                      &flitzzip::extract_xz_file);
}

// Purpose: Verify native `.xz` extraction over a CRC64 fixture.
// Inputs: A deterministic `.xz` file generated from a small single-file payload.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(xz_extracts_single_file_fixture) {
    const auto root = test_temp_dir("xz-single");
    const auto archive = root / "payload.txt.xz";
    write_fixture(archive, kSingleFileXzFixture);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Xz);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_xz_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "Example XZ fixture payload.\nSecond line.\n");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Keep independently encoded XZ input immutable through successful and interrupted extraction.
// Inputs: A small verified CRC64 fixture and real Windows sharing probes; no source bytes are modified.
// Outputs: Refused active/concurrent writers, exact decoded bytes and source-lock release on every tested exit.
TEST_CASE(single_stream_source_session_xz) {
    const auto root = test_temp_dir("xz-source-session");
    const auto archive = root / "payload.xz";
    write_fixture(archive, kSingleFileXzFixture);
    flitzzip_test::require_archive_source_session(
        archive, root, "payload", "Example XZ fixture payload.\nSecond line.\n", &flitzzip::extract_xz_file);
}

// Purpose: Preserve native Unicode filenames while extracting independent xz fixtures.
// Inputs: A bounded upstream-compatible fixture with an uppercase suffix and a Unicode host filename.
// Outputs: Requires exact restored bytes and overwrite refusal without encoding errors.
TEST_CASE(xz_unicode_archive_filename_extracts) {
    const auto root = test_temp_dir("xz-unicode-name");
    const auto filename = std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.txt");
    auto archive = root / filename;
    archive += ".XZ";
    write_fixture(archive, kSingleFileXzFixture);
    const auto output = root / "out";
    (void)flitzzip::extract_xz_file(archive, output, false);
    REQUIRE_EQ(read_text_file(output / filename), "Example XZ fixture payload.\nSecond line.\n");
    bool refused = false;
    try {
        (void)flitzzip::extract_xz_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(read_text_file(output / filename), "Example XZ fixture payload.\nSecond line.\n");
}

// Purpose: Verify `.xz` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.xz` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(xz_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("xz-overwrite");
    const auto archive = root / "payload.txt.xz";
    write_fixture(archive, kSingleFileXzFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream(output / "payload.txt") << "old";
    }

    bool rejected = false;
    try {
        (void)flitzzip::extract_xz_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "old");
}

// Purpose: Verify malformed `.xz` streams do not publish output.
// Inputs: A file with the `.xz` extension but invalid XZ magic/content.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(xz_rejects_bad_magic_without_output) {
    const auto root = test_temp_dir("xz-bad-magic");
    const auto archive = root / "bad.xz";
    write_binary_file(archive, {0xFDU, 0x37U, 0x7AU, 0x00U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_xz_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Reject a recoverable XZ header failure before any final output is published.
// Inputs: A Check-None stream generated by Python liblzma, then a corrupted header CRC.
// Outputs: Requires valid control extraction, overwrite preservation, TAR rejection, and permanent failure.
TEST_CASE(xz_resumable_error_never_publishes) {
    const auto root = test_temp_dir("xz-resumable-error");
    const auto archive = root / "payload.txt.xz";
    std::vector<unsigned char> bytes{
        0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00, 0x00, 0x00, 0xFF, 0x12, 0xD9, 0x41, 0x02, 0x00, 0x21, 0x01,
        0x16, 0x00, 0x00, 0x00, 0x74, 0x2F, 0xE5, 0xA3, 0x01, 0x00, 0x0F, 0x76, 0x65, 0x72, 0x69, 0x66,
        0x69, 0x65, 0x64, 0x20, 0x70, 0x61, 0x79, 0x6C, 0x6F, 0x61, 0x64, 0x00, 0x00, 0x01, 0x20, 0x10,
        0xED, 0x81, 0xB5, 0xA8, 0x06, 0x72, 0x9E, 0x7A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x5A,
    };
    write_binary_file(archive, bytes);
    (void)flitzzip::extract_xz_file(archive, root / "control", false);
    REQUIRE_EQ(read_text_file(root / "control" / "payload.txt"), "verified payload");
    bytes[8] ^= 1U;
    write_binary_file(archive, bytes);
    for (const bool overwrite : {false, true}) {
        const auto output = root / (overwrite ? "existing" : "new");
        if (overwrite) {
            std::filesystem::create_directories(output);
            std::ofstream(output / "payload.txt") << "preserve me";
        }
        bool rejected = false;
        try {
            (void)flitzzip::extract_xz_file(archive, output, overwrite);
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
        (void)flitzzip::extract_tar_xz(archive, root / "tar", false);
    } catch (const flitzzip::Error&) {
        tar_rejected = true;
    }
    REQUIRE_TRUE(tar_rejected);
    REQUIRE_EQ(count_regular_files(root / "tar"), 0U);
    require_sticky_decoder_failure<flitzzip::XzInputStream>(archive);
}

// Purpose: Reject a payload checksum failure even if the decoder produced bytes during the failing call.
// Inputs: A valid CRC64 XZ fixture with a corrupt data checksum.
// Outputs: Requires sticky read/finalization errors and preservation of an existing destination.
TEST_CASE(xz_payload_checksum_error_never_publishes) {
    const auto root = test_temp_dir("xz-payload-checksum-error");
    const auto archive = root / "payload.txt.xz";
    auto bytes = kSingleFileXzFixture;
    bytes[72] ^= 1U;
    write_fixture(archive, bytes);
    require_sticky_decoder_failure<flitzzip::XzInputStream>(archive);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "payload.txt") << "preserve me";
    bool rejected = false;
    try {
        (void)flitzzip::extract_xz_file(archive, output, true);
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "preserve me");
}

// Direct transfer must preserve real decoder windows, concatenation, empty input,
// sink recovery and publication against independent frozen liblzma bytes.
TEST_CASE(xz_decoded_window_transfer_matches_independent_frames) {
    flitzzip_test::ScopedTestDirectory cleanup{test_temp_dir("xz-decoded-transfer")};
    const auto& root = cleanup.path;
    const auto payload = flitzzip_test::decoded_transfer_fixture_payload();
    const auto archive = root / "payload.xz";
    write_fixture(archive, kWindowTransferXzFixture);
    flitzzip_test::require_decoded_fixture_transfer<flitzzip::XzInputStream>(archive, payload);

    std::vector<unsigned char> joined(kWindowTransferXzFixture.begin(), kWindowTransferXzFixture.end());
    joined.insert(joined.end(), kEmptyTransferXzFixture.begin(), kEmptyTransferXzFixture.end());
    joined.insert(joined.end(), kSingleFileXzFixture.begin(), kSingleFileXzFixture.end());
    joined.insert(joined.end(), kWindowTransferXzFixture.begin(), kWindowTransferXzFixture.end());
    const auto expected = payload + "Example XZ fixture payload.\nSecond line.\n" + payload;
    const auto concatenated = root / "joined.xz";
    write_binary_file(concatenated, joined);
    flitzzip_test::require_decoded_fixture_transfer<flitzzip::XzInputStream>(concatenated, expected);
    const auto destination = root / "restored";
    const auto stats = flitzzip::extract_xz_file(concatenated, destination, false);
    REQUIRE_EQ(read_text_file(destination / "joined"), expected);
    REQUIRE_EQ(stats.output_bytes, expected.size());
    REQUIRE_EQ(stats.input_bytes, std::filesystem::file_size(concatenated));
    REQUIRE_TRUE(!stats.gpu_used);

    const auto empty = root / "empty.xz";
    write_fixture(empty, kEmptyTransferXzFixture);
    flitzzip_test::require_decoded_fixture_transfer<flitzzip::XzInputStream>(empty, "");
    REQUIRE_EQ(flitzzip::extract_xz_file(empty, destination, false).output_bytes, 0U);
    REQUIRE_TRUE(std::filesystem::exists(destination / "empty"));
    REQUIRE_EQ(std::filesystem::file_size(destination / "empty"), 0U);
}
