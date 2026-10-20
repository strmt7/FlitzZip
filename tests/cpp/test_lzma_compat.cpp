#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "lzma/lzma_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Independent fixture: Python 3.13 liblzma output, verified with 7-Zip 26.03.
constexpr std::array<unsigned char, 65> kSingleFileLzmaFixture{{
    0x5DU, 0x00U, 0x00U, 0x80U, 0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0x00U, 0x22U, 0x9EU, 0x08U, 0x26U, 0xFFU, 0x96U, 0x34U, 0x91U, 0xE7U, 0x71U, 0xDDU, 0x2EU,
    0xADU, 0x49U, 0x72U, 0xC3U, 0xDEU, 0xC5U, 0x9DU, 0x9CU, 0x34U, 0x17U, 0xBDU, 0x12U, 0xFFU,
    0xA7U, 0xD0U, 0x2FU, 0x0EU, 0x27U, 0xBBU, 0x66U, 0xB0U, 0x91U, 0xAAU, 0x58U, 0xCEU, 0x40U,
    0x4EU, 0x29U, 0xF8U, 0x26U, 0x46U, 0x88U, 0x0EU, 0x3DU, 0x1FU, 0xFEU, 0x0BU, 0x40U, 0x00U,
}};

// Independent known-size streams from the unmodified public-domain LZMA SDK 26.03 encoder.
// The upstream SDK command-line decoder verified all four streams, including both optional-marker forms.

constexpr std::array<unsigned char, 74> kKnownMarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x34U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x36U,
    0x19U, 0x49U, 0x29U, 0x7AU, 0x9AU, 0x86U, 0x9CU, 0xF8U, 0xD4U, 0xF6U, 0xBDU, 0xA4U, 0x95U, 0x43U, 0xFEU,
    0x6BU, 0x88U, 0x6AU, 0xCFU, 0x73U, 0xEBU, 0xCEU, 0x55U, 0x45U, 0x6DU, 0xCBU, 0xA7U, 0xD8U, 0x38U, 0x2DU,
    0x78U, 0x25U, 0xA4U, 0x63U, 0x9EU, 0x03U, 0x75U, 0xCFU, 0xC0U, 0x2BU, 0x8AU, 0x5DU, 0xF7U, 0xC6U, 0xE5U,
    0xC3U, 0x15U, 0x61U, 0x08U, 0x6DU, 0x2BU, 0x17U, 0x2CU, 0x7FU, 0xFFU, 0xDDU, 0xC8U, 0x40U, 0x00U,
}};

constexpr std::array<unsigned char, 69> kKnownUnmarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x34U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x36U, 0x19U, 0x49U, 0x29U, 0x7AU, 0x9AU, 0x86U, 0x9CU, 0xF8U, 0xD4U, 0xF6U, 0xBDU, 0xA4U, 0x95U,
    0x43U, 0xFEU, 0x6BU, 0x88U, 0x6AU, 0xCFU, 0x73U, 0xEBU, 0xCEU, 0x55U, 0x45U, 0x6DU, 0xCBU, 0xA7U,
    0xD8U, 0x38U, 0x2DU, 0x78U, 0x25U, 0xA4U, 0x63U, 0x9EU, 0x03U, 0x75U, 0xCFU, 0xC0U, 0x2BU, 0x8AU,
    0x5DU, 0xF7U, 0xC6U, 0xE5U, 0xC3U, 0x15U, 0x61U, 0x08U, 0x6CU, 0xC6U, 0x47U, 0x00U, 0x00U,
}};

constexpr std::array<unsigned char, 23> kEmptyMarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x83U, 0xFFU, 0xFBU, 0xFFU, 0xFFU, 0xC0U, 0x00U, 0x00U, 0x00U,
}};

constexpr std::array<unsigned char, 18> kEmptyUnmarkedLzma{{
    0x5DU,
    0x00U,
    0x10U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
}};

// Independent SDK 26.03 streams span the 64 KiB output buffer and exact two-buffer completion.
constexpr std::array<unsigned char, 113> kBoundary65537MarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U,
    0x0CU, 0x42U, 0x92U, 0x6AU, 0x67U, 0xBCU, 0x0EU, 0xD1U, 0x33U, 0x2CU, 0xE4U, 0xCCU, 0x42U, 0x29U, 0xC0U,
    0x9BU, 0x53U, 0x23U, 0xF1U, 0xBCU, 0x08U, 0x21U, 0x89U, 0x02U, 0x99U, 0xB5U, 0x18U, 0x23U, 0x05U, 0xD3U,
    0x5CU, 0xC3U, 0xBAU, 0x85U, 0x52U, 0x4DU, 0x2FU, 0xBFU, 0xF8U, 0x21U, 0xE5U, 0xF6U, 0x6BU, 0xFEU, 0xA2U,
    0x04U, 0x13U, 0x64U, 0x7EU, 0xCBU, 0x1CU, 0x47U, 0x82U, 0x67U, 0x75U, 0x01U, 0x8FU, 0x5FU, 0x22U, 0xE8U,
    0x13U, 0xD9U, 0x91U, 0xDEU, 0x3CU, 0x8CU, 0x02U, 0xD1U, 0x2DU, 0x62U, 0x0AU, 0xB4U, 0xF1U, 0xC0U, 0x79U,
    0x8FU, 0xECU, 0x2BU, 0x69U, 0x02U, 0x66U, 0xC2U, 0xCEU, 0x47U, 0x73U, 0x31U, 0x5AU, 0xEBU, 0xE7U, 0x85U,
    0xACU, 0x86U, 0x16U, 0xFFU, 0xFEU, 0x4DU, 0x48U, 0x00U,
}};

constexpr std::array<unsigned char, 108> kBoundary65537UnmarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x0CU,
    0x42U, 0x92U, 0x6AU, 0x67U, 0xBCU, 0x0EU, 0xD1U, 0x33U, 0x2CU, 0xE4U, 0xCCU, 0x42U, 0x29U, 0xC0U, 0x9BU, 0x53U,
    0x23U, 0xF1U, 0xBCU, 0x08U, 0x21U, 0x89U, 0x02U, 0x99U, 0xB5U, 0x18U, 0x23U, 0x05U, 0xD3U, 0x5CU, 0xC3U, 0xBAU,
    0x85U, 0x52U, 0x4DU, 0x2FU, 0xBFU, 0xF8U, 0x21U, 0xE5U, 0xF6U, 0x6BU, 0xFEU, 0xA2U, 0x04U, 0x13U, 0x64U, 0x7EU,
    0xCBU, 0x1CU, 0x47U, 0x82U, 0x67U, 0x75U, 0x01U, 0x8FU, 0x5FU, 0x22U, 0xE8U, 0x13U, 0xD9U, 0x91U, 0xDEU, 0x3CU,
    0x8CU, 0x02U, 0xD1U, 0x2DU, 0x62U, 0x0AU, 0xB4U, 0xF1U, 0xC0U, 0x79U, 0x8FU, 0xECU, 0x2BU, 0x69U, 0x02U, 0x66U,
    0xC2U, 0xCEU, 0x47U, 0x73U, 0x31U, 0x5AU, 0xEBU, 0xE7U, 0x7BU, 0x15U, 0xB7U, 0xC7U,
}};

constexpr std::array<unsigned char, 126> kBoundary131072MarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x0CU,
    0x42U, 0x92U, 0x6AU, 0x67U, 0xBCU, 0x0EU, 0xD1U, 0x33U, 0x2CU, 0xE4U, 0xCCU, 0x42U, 0x29U, 0xC0U, 0x9BU, 0x53U,
    0x23U, 0xF1U, 0xBCU, 0x08U, 0x21U, 0x89U, 0x02U, 0x99U, 0xB5U, 0x18U, 0x23U, 0x05U, 0xD3U, 0x5CU, 0xC3U, 0xBAU,
    0x85U, 0x52U, 0x4DU, 0x2FU, 0xBFU, 0xF8U, 0x21U, 0xE5U, 0xF6U, 0x6BU, 0xFEU, 0xA2U, 0x04U, 0x13U, 0x64U, 0x7EU,
    0xCBU, 0x1CU, 0x47U, 0x82U, 0x67U, 0x75U, 0x01U, 0x8FU, 0x5FU, 0x22U, 0xE8U, 0x13U, 0xD9U, 0x91U, 0xDEU, 0x3CU,
    0x8CU, 0x02U, 0xD1U, 0x2DU, 0x62U, 0x0AU, 0xB4U, 0xF1U, 0xC0U, 0x79U, 0x8FU, 0xECU, 0x2BU, 0x69U, 0x02U, 0x66U,
    0xC2U, 0xCEU, 0x47U, 0x73U, 0x31U, 0x5AU, 0xEBU, 0xE7U, 0xF3U, 0x8EU, 0x4AU, 0x28U, 0x72U, 0xA1U, 0x7CU, 0xF7U,
    0xF8U, 0x6AU, 0x1AU, 0xE8U, 0xB5U, 0x30U, 0x8AU, 0xCFU, 0xFFU, 0xFFU, 0xF8U, 0xCEU, 0x40U, 0x00U,
}};

constexpr std::array<unsigned char, 120> kBoundary131072UnmarkedLzma{{
    0x5DU, 0x00U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U,
    0x0CU, 0x42U, 0x92U, 0x6AU, 0x67U, 0xBCU, 0x0EU, 0xD1U, 0x33U, 0x2CU, 0xE4U, 0xCCU, 0x42U, 0x29U, 0xC0U,
    0x9BU, 0x53U, 0x23U, 0xF1U, 0xBCU, 0x08U, 0x21U, 0x89U, 0x02U, 0x99U, 0xB5U, 0x18U, 0x23U, 0x05U, 0xD3U,
    0x5CU, 0xC3U, 0xBAU, 0x85U, 0x52U, 0x4DU, 0x2FU, 0xBFU, 0xF8U, 0x21U, 0xE5U, 0xF6U, 0x6BU, 0xFEU, 0xA2U,
    0x04U, 0x13U, 0x64U, 0x7EU, 0xCBU, 0x1CU, 0x47U, 0x82U, 0x67U, 0x75U, 0x01U, 0x8FU, 0x5FU, 0x22U, 0xE8U,
    0x13U, 0xD9U, 0x91U, 0xDEU, 0x3CU, 0x8CU, 0x02U, 0xD1U, 0x2DU, 0x62U, 0x0AU, 0xB4U, 0xF1U, 0xC0U, 0x79U,
    0x8FU, 0xECU, 0x2BU, 0x69U, 0x02U, 0x66U, 0xC2U, 0xCEU, 0x47U, 0x73U, 0x31U, 0x5AU, 0xEBU, 0xE7U, 0xF3U,
    0x8EU, 0x4AU, 0x28U, 0x72U, 0xA1U, 0x7CU, 0xF7U, 0xF8U, 0x6AU, 0x1AU, 0xE8U, 0xB5U, 0x26U, 0xD1U, 0x00U,
}};

// Purpose: Write exact fixture bytes to disk.
// Inputs: `path` is the destination and `bytes` contains a complete `.lzma` stream.
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

// Purpose: Verify known-size LZMA termination using independently encoded streams rather than prefix length alone.
// Inputs: Empty and nonempty upstream SDK fixtures, with and without their optional end marker.
// Outputs: Requires exact bytes and telemetry, then rejects trailing data while preserving the existing destination.
TEST_CASE(lzma_known_size_termination_and_trailing_data) {
    const auto root = test_temp_dir("lzma-known-termination");
    const std::vector<std::vector<unsigned char>> fixtures{
        {kKnownMarkedLzma.begin(), kKnownMarkedLzma.end()},
        {kKnownUnmarkedLzma.begin(), kKnownUnmarkedLzma.end()},
        {kEmptyMarkedLzma.begin(), kEmptyMarkedLzma.end()},
        {kEmptyUnmarkedLzma.begin(), kEmptyUnmarkedLzma.end()},
    };
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        const auto archive = root / (std::to_string(index) + ".lzma");
        const auto output = root / ("out-" + std::to_string(index));
        const auto target = output / std::to_string(index);
        const std::string expected = index < 2 ? "legacy LZMA must not silently truncate this payload\n" : "";
        write_binary_file(archive, fixtures[index]);
        const auto stats = flitzzip::extract_lzma_file(archive, output, false);
        REQUIRE_EQ(stats.entries, 1U);
        REQUIRE_EQ(stats.output_bytes, expected.size());
        REQUIRE_EQ(read_text_file(target), expected);
        REQUIRE_EQ(count_regular_files(output), 1U);
        for (const unsigned char trailing : {0U, 0xFFU}) {
            auto malformed = fixtures[index];
            malformed.push_back(trailing);
            write_binary_file(archive, malformed);
            std::ofstream(target, std::ios::binary | std::ios::trunc) << "original";
            bool rejected = false;
            try {
                (void)flitzzip::extract_lzma_file(archive, output, true);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(read_text_file(target), "original");
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}), 1);
        }
    }
}

// Purpose: Reject a known output size that conceals additional compressed payload or requires missing output.
// Inputs: Independent known-size fixtures with each smaller declaration and a larger declaration.
// Outputs: Requires failure before publication, unchanged caller bytes and successful subsequent valid reuse.
TEST_CASE(lzma_declared_size_cannot_truncate_or_extend_payload) {
    const auto root = test_temp_dir("lzma-size-mismatch");
    const auto archive = root / "payload.lzma";
    const auto output = root / "out";
    std::filesystem::create_directory(output);
    const std::string expected = "legacy LZMA must not silently truncate this payload\n";
    const std::vector<std::vector<unsigned char>> fixtures{
        {kKnownMarkedLzma.begin(), kKnownMarkedLzma.end()},
        {kKnownUnmarkedLzma.begin(), kKnownUnmarkedLzma.end()},
    };
    for (const auto& fixture : fixtures) {
        for (std::uint64_t declared = 0; declared <= expected.size() + 1; ++declared) {
            if (declared == expected.size()) {
                continue;
            }
            auto malformed = fixture;
            for (unsigned byte = 0; byte < 8; ++byte) {
                malformed[5U + byte] = static_cast<unsigned char>(declared >> (byte * 8U));
            }
            write_binary_file(archive, malformed);
            std::ofstream(output / "payload", std::ios::binary | std::ios::trunc) << "original";
            bool rejected = false;
            try {
                (void)flitzzip::extract_lzma_file(archive, output, true);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(read_text_file(output / "payload"), "original");
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}), 1);
        }
        write_binary_file(archive, fixture);
        REQUIRE_EQ(flitzzip::extract_lzma_file(archive, output, true).output_bytes, expected.size());
        REQUIRE_EQ(read_text_file(output / "payload"), expected);
    }
}

// Purpose: Require complete range-coder termination even when an LZMA header declares zero output bytes.
// Inputs: Every strict prefix of four independently encoded known-size streams.
// Outputs: Rejects truncation without replacing an existing file or leaving a staging directory.
TEST_CASE(lzma_known_size_rejects_every_truncation) {
    const auto root = test_temp_dir("lzma-known-truncation");
    const auto archive = root / "payload.lzma";
    const auto output = root / "out";
    std::filesystem::create_directory(output);
    std::ofstream(output / "payload", std::ios::binary) << "original";
    const std::vector<std::vector<unsigned char>> fixtures{
        {kKnownMarkedLzma.begin(), kKnownMarkedLzma.end()},
        {kKnownUnmarkedLzma.begin(), kKnownUnmarkedLzma.end()},
        {kEmptyMarkedLzma.begin(), kEmptyMarkedLzma.end()},
        {kEmptyUnmarkedLzma.begin(), kEmptyUnmarkedLzma.end()},
    };
    for (const auto& fixture : fixtures) {
        for (std::size_t size = 0; size < fixture.size(); ++size) {
            write_binary_file(archive, {fixture.begin(), fixture.begin() + static_cast<std::ptrdiff_t>(size)});
            bool rejected = false;
            try {
                (void)flitzzip::extract_lzma_file(archive, output, true);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(read_text_file(output / "payload"), "original");
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}), 1);
        }
    }
}

// Purpose: Verify known-size completion after multiple decoder calls and at an exact buffer boundary.
// Inputs: Independent SDK streams with 65,537 and 131,072 output bytes, each with and without an end marker.
// Outputs: Exact payloads survive buffer transitions; adjacent false size declarations never replace a valid file.
TEST_CASE(lzma_known_size_completion_spans_output_buffers) {
    const auto root = test_temp_dir("lzma-buffer-boundary");
    const auto archive = root / "payload.lzma";
    const auto output = root / "out";
    struct Fixture {
        std::vector<unsigned char> bytes;
        std::uint64_t size;
    };
    const std::array<Fixture, 4> fixtures{{
        {{kBoundary65537MarkedLzma.begin(), kBoundary65537MarkedLzma.end()}, 65537U},
        {{kBoundary65537UnmarkedLzma.begin(), kBoundary65537UnmarkedLzma.end()}, 65537U},
        {{kBoundary131072MarkedLzma.begin(), kBoundary131072MarkedLzma.end()}, 131072U},
        {{kBoundary131072UnmarkedLzma.begin(), kBoundary131072UnmarkedLzma.end()}, 131072U},
    }};
    constexpr std::string_view pattern = "0123456789abcdef\n";
    for (const auto& fixture : fixtures) {
        std::string expected;
        expected.reserve(static_cast<std::size_t>(fixture.size));
        while (expected.size() < fixture.size) {
            expected.append(pattern.substr(0, std::min<std::size_t>(pattern.size(), fixture.size - expected.size())));
        }
        write_binary_file(archive, fixture.bytes);
        REQUIRE_EQ(flitzzip::extract_lzma_file(archive, output, true).output_bytes, fixture.size);
        REQUIRE_EQ(read_text_file(output / "payload"), expected);
        for (const auto declared : {fixture.size - 1U, fixture.size + 1U}) {
            auto malformed = fixture.bytes;
            for (unsigned byte = 0; byte < 8; ++byte) {
                malformed[5U + byte] = static_cast<unsigned char>(declared >> (byte * 8U));
            }
            write_binary_file(archive, malformed);
            bool rejected = false;
            try {
                (void)flitzzip::extract_lzma_file(archive, output, true);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(read_text_file(output / "payload"), expected);
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator()), 1);
        }
    }
}

// Purpose: Verify native `.lzma` extraction over a deterministic LZMA-Alone fixture.
// Inputs: A small LZMA-Alone stream generated from a single-file payload.
// Outputs: Throws if format detection, extraction, or restored bytes regress.
TEST_CASE(lzma_extracts_single_file_fixture) {
    const auto root = test_temp_dir("lzma-single");
    const auto archive = root / "payload.txt.lzma";
    write_fixture(archive, kSingleFileLzmaFixture);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Lzma);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_lzma_file(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "Example LZMA fixture payload.\nSecond line.\n");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Preserve native Unicode filenames while extracting independent lzma fixtures.
// Inputs: A bounded upstream-compatible fixture with an uppercase suffix and a Unicode host filename.
// Outputs: Requires exact restored bytes and overwrite refusal without encoding errors.
TEST_CASE(lzma_unicode_archive_filename_extracts) {
    const auto root = test_temp_dir("lzma-unicode-name");
    const auto filename = std::filesystem::path(u8"\u6E2C\u8A66\U0001F4E6.txt");
    auto archive = root / filename;
    archive += ".LZMA";
    write_fixture(archive, kSingleFileLzmaFixture);
    const auto output = root / "out";
    (void)flitzzip::extract_lzma_file(archive, output, false);
    REQUIRE_EQ(read_text_file(output / filename), "Example LZMA fixture payload.\nSecond line.\n");
    bool refused = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_EQ(read_text_file(output / filename), "Example LZMA fixture payload.\nSecond line.\n");
}

// Purpose: Verify `.lzma` extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid `.lzma` stream and a preexisting destination file.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(lzma_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("lzma-overwrite");
    const auto archive = root / "payload.txt.lzma";
    write_fixture(archive, kSingleFileLzmaFixture);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    {
        std::ofstream(output / "payload.txt") << "old";
    }

    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "payload.txt"), "old");
}

// Purpose: Verify malformed `.lzma` streams do not publish output.
// Inputs: A file with a valid-looking suffix but incomplete LZMA-Alone header.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_truncated_header_without_output) {
    const auto root = test_temp_dir("lzma-truncated-header");
    const auto archive = root / "bad.lzma";
    write_binary_file(archive, {0x5DU, 0x00U, 0x00U, 0x80U});

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify corrupt LZMA payloads do not publish partial output.
// Inputs: A complete header followed by invalid compressed payload bytes.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_corrupt_payload_without_output) {
    const auto root = test_temp_dir("lzma-corrupt-payload");
    const auto archive = root / "payload.bin.lzma";
    write_binary_file(archive, {
                                   0x5DU,
                                   0x00U,
                                   0x00U,
                                   0x80U,
                                   0x00U,
                                   0x05U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                                   0x00U,
                               });

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify `.lzma` streams with trailing data after the end marker fail closed.
// Inputs: A valid LZMA-Alone stream with one extra byte appended.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_trailing_data_after_end_marker) {
    const auto root = test_temp_dir("lzma-trailing-data");
    const auto archive = root / "payload.txt.lzma";
    std::vector<unsigned char> bytes(kSingleFileLzmaFixture.begin(), kSingleFileLzmaFixture.end());
    bytes.push_back(0x00U);
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}

// Purpose: Verify hostile dictionary declarations are rejected before decoder allocation.
// Inputs: A valid LZMA property byte with a dictionary larger than FlitzZip policy.
// Outputs: Throws and leaves the destination without regular files.
TEST_CASE(lzma_rejects_oversized_dictionary_without_output) {
    const auto root = test_temp_dir("lzma-oversized-dict");
    const auto archive = root / "huge.lzma";
    write_binary_file(archive, {
                                   0x5DU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0x7FU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0xFFU,
                                   0x00U,
                               });

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_lzma_file(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
}
