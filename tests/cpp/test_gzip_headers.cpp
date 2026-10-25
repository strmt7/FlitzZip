#include "test_util.hpp"
#include "test_source_session.hpp"

#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"
#include "gzip/gzip_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace {

// Purpose: Build a checksum oracle independent of the production miniz CRC implementation.
// Inputs: Exact header bytes. Outputs: RFC 1952 CRC32, reduced to its low 16 bits by the fixture builder.
std::uint32_t header_crc(std::span<const unsigned char> bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

// Purpose: Encode a Gzip member independently of its production writer, including every optional header field.
// Inputs: RFC 1952 flags and a bounded extra-field size. Outputs: A stored DEFLATE block containing "hello".
std::vector<unsigned char> gzip_header_fixture(unsigned char flags, std::size_t extra_bytes = 4U) {
    std::vector<unsigned char> bytes{0x1FU, 0x8BU, 8U, flags, 0U, 0U, 0U, 0U, 0U, 255U};
    if ((flags & 4U) != 0U) {
        bytes.push_back(static_cast<unsigned char>(extra_bytes & 255U));
        bytes.push_back(static_cast<unsigned char>(extra_bytes >> 8U));
        bytes.insert(bytes.end(), extra_bytes, 0U);
    }
    if ((flags & 8U) != 0U) {
        constexpr std::array<unsigned char, 15> name{'.', '.', '/', 'o', 'u', 't', 's', 'i',
                                                     'd', 'e', '.', 't', 'x', 't', 0};
        bytes.insert(bytes.end(), name.begin(), name.end());
    }
    if ((flags & 16U) != 0U) {
        constexpr std::array<unsigned char, 9> comment{'m', 'e', 't', 'a', 'd', 'a', 't', 'a', 0};
        bytes.insert(bytes.end(), comment.begin(), comment.end());
    }
    if ((flags & 2U) != 0U) {
        const auto crc = header_crc(bytes);
        bytes.push_back(static_cast<unsigned char>(crc & 255U));
        bytes.push_back(static_cast<unsigned char>((crc >> 8U) & 255U));
    }
    // Stored DEFLATE: BFINAL, LEN=5, NLEN; fixed CRC32("hello")=0x3610A686 and ISIZE=5.
    constexpr std::array<unsigned char, 18> body{1U,  5U,    0U,    250U,  255U,  'h', 'e', 'l', 'l',
                                                 'o', 0x86U, 0xA6U, 0x10U, 0x36U, 5U,  0U,  0U,  0U};
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

// Purpose: Write a bounded hand-authored fixture without involving a production encoder.
// Inputs: Owned temporary path and exact bytes. Outputs: Complete file or an I/O failure.
void write_gzip_header_fixture(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

// Purpose: Require a malformed wrapper to fail instead of silently accepting damaged metadata.
// Inputs: A bounded invalid member and an owned temporary path. Outputs: ArchiveError must occur by finalization.
void require_gzip_header_rejected(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
    write_gzip_header_fixture(path, bytes);
    bool rejected = false;
    try {
        flitzzip::GzipInputStream input(path);
        input.finish();
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

}  // namespace

// Purpose: Preserve valid metadata combinations and bounded reading across the largest extra field.
// Inputs: All 32 defined flag combinations and a 65535-byte extra field, using independent DEFLATE/checksums.
// Outputs: Requires the same exact payload, accounting and repeatable finalization.
TEST_CASE(gzip_header_optional_fields_and_crc_roundtrip) {
    const auto root = test_temp_dir("gzip-header-fields");
    const auto path = root / "fixture.gz";
    for (unsigned char flags = 0U; flags < 32U; ++flags) {
        const auto bytes = gzip_header_fixture(flags, flags == 31U ? 65535U : 4U);
        write_gzip_header_fixture(path, bytes);
        flitzzip::GzipInputStream input(path);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), "hello");
        input.finish();
        input.finish();
        REQUIRE_EQ(input.input_bytes(), bytes.size());
        REQUIRE_EQ(input.output_bytes(), 5U);
        REQUIRE_EQ(input.compressed_payload_bytes(), 10U);
        REQUIRE_EQ(input.compressed_payload_read_bytes(), 10U);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Detect corruption of every header region covered by FHCRC before accepting the member.
// Inputs: Changed timestamp, extra data, filename, comment and both checksum bytes, with a valid payload CRC.
// Outputs: Every mutation is rejected; a subsequent valid member still decodes correctly.
TEST_CASE(gzip_header_crc_rejects_each_covered_region) {
    const auto root = test_temp_dir("gzip-header-corruption");
    const auto path = root / "fixture.gz";
    const auto valid = gzip_header_fixture(30U);
    for (const auto index : {4U, 12U, 20U, 35U, 40U, 41U}) {
        auto corrupted = valid;
        corrupted[index] ^= 1U;
        require_gzip_header_rejected(path, corrupted);
    }
    write_gzip_header_fixture(path, valid);
    {
        flitzzip::GzipInputStream input(path);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()), "hello");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Refuse headers that overlap their trailer or rely on unterminated metadata.
// Inputs: Invalid magic/method/reserved flags, excessive extra length, missing terminators and a short checksum.
// Outputs: Every malformed stream throws without allocating from untrusted header lengths.
TEST_CASE(gzip_header_rejects_malformed_bounds_and_flags) {
    const auto root = test_temp_dir("gzip-header-bounds");
    const auto path = root / "fixture.gz";
    const auto valid = gzip_header_fixture(0U);
    for (const auto index : {0U, 1U, 2U, 3U}) {
        auto invalid = valid;
        invalid[index] = 0xE0U;
        require_gzip_header_rejected(path, invalid);
    }
    auto extra = gzip_header_fixture(4U);
    extra[10] = 255U;
    extra[11] = 255U;
    require_gzip_header_rejected(path, extra);
    for (const auto flags : {2U, 4U, 8U, 16U}) {
        auto invalid = valid;
        invalid[3] = static_cast<unsigned char>(flags);
        invalid.resize(19U, 0U);
        invalid[10] = 255U;
        require_gzip_header_rejected(path, invalid);
    }
    for (std::size_t size = 0; size < 18U; ++size) {
        require_gzip_header_rejected(path, std::span(valid).first(size));
    }
    std::filesystem::remove_all(root);
}

namespace {

// Purpose: Distinguish the size-trailer grammar refusal from CRC, transport and unrelated exceptions.
// Inputs: One synchronous decoder or public extraction action. Outputs: Exact ArchiveError, never success.
template <typename Operation> void require_gzip_isize_error(Operation&& operation) {
    bool rejected = false;
    try {
        operation();
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_EQ(std::string(error.what()), "Gzip uncompressed-size verification failed");
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Prove a real size mismatch remains terminal without modifying caller bytes after refusal.
// Inputs: Malformed complete member and whether the caller first consumes its legal five-byte prefix.
// Outputs: Exact prefix/canaries, sticky ordinary read and finish refusal, with decoded accounting bounded at five.
void require_gzip_isize_terminal(const std::filesystem::path& path, bool read_first) {
    flitzzip::GzipInputStream input(path);
    std::array<char, 11> destination{};
    destination.fill('Z');
    if (read_first) {
        input.read(destination.data() + 3, 5);
        REQUIRE_EQ(input.gcount(), 5);
        REQUIRE_EQ(std::string(destination.data() + 3, 5), "hello");
        REQUIRE_EQ(destination, (std::array<char, 11>{'Z', 'Z', 'Z', 'h', 'e', 'l', 'l', 'o', 'Z', 'Z', 'Z'}));
        REQUIRE_EQ(input.output_bytes(), 5U);
        REQUIRE_TRUE(input.good());
    }
    const auto accepted = destination;
    if (read_first) {
        require_gzip_isize_error([&] { input.read(destination.data() + 3, 1); });
        REQUIRE_TRUE(input.bad());
    } else {
        require_gzip_isize_error([&] { input.finish(); });
    }
    REQUIRE_EQ(destination, accepted);
    REQUIRE_EQ(input.output_bytes(), 5U);
    for (int attempt = 0; attempt < 2; ++attempt) {
        input.clear();
        require_gzip_isize_error([&] { input.read(destination.data() + 3, 1); });
        REQUIRE_TRUE(input.bad());
        REQUIRE_EQ(destination, accepted);
        input.clear();
        require_gzip_isize_error([&] { input.finish(); });
        REQUIRE_EQ(destination, accepted);
        REQUIRE_EQ(input.output_bytes(), 5U);
    }
    REQUIRE_EQ(destination.front(), 'Z');
    REQUIRE_EQ(destination.back(), 'Z');
}

// Purpose: Change only the independent complete member's ISIZE field while preserving its DEFLATE and CRC bytes.
// Inputs: Original literal "hello" frame and one wrong modulo-32-bit length.
// Outputs: A complete equally sized wire fixture; no production writer or decoder constructs the oracle.
std::vector<unsigned char> gzip_wrong_isize_fixture(const std::vector<unsigned char>& valid, std::uint32_t size) {
    REQUIRE_EQ(valid.size(), 28U);
    auto damaged = valid;
    for (unsigned byte = 0; byte < 4U; ++byte)
        damaged[damaged.size() - 4U + byte] = static_cast<unsigned char>((size >> (byte * 8U)) & 255U);
    REQUIRE_TRUE(std::equal(valid.begin(), valid.end() - 4, damaged.begin()));
    return damaged;
}

// Purpose: Confirm normal complete decoding after the failed reader has released its native source owners.
// Inputs: Same owned path and independent complete valid frame. Outputs: Exact caller bytes, canaries and counters.
void require_gzip_isize_valid_reuse(const std::filesystem::path& path, const std::vector<unsigned char>& valid) {
    write_gzip_header_fixture(path, valid);
    {
        flitzzip::GzipInputStream input(path);
        std::array<char, 11> destination{};
        destination.fill('Z');
        input.read(destination.data() + 3, 5);
        REQUIRE_EQ(input.gcount(), 5);
        REQUIRE_EQ(std::string(destination.data() + 3, 5), "hello");
        REQUIRE_EQ(destination, (std::array<char, 11>{'Z', 'Z', 'Z', 'h', 'e', 'l', 'l', 'o', 'Z', 'Z', 'Z'}));
        REQUIRE_EQ(destination.front(), 'Z');
        REQUIRE_EQ(destination.back(), 'Z');
        REQUIRE_EQ(input.peek(), std::char_traits<char>::eof());
        input.finish();
        input.finish();
        REQUIRE_EQ(input.input_bytes(), valid.size());
        REQUIRE_EQ(input.output_bytes(), 5U);
    }
    DWORD error = ERROR_SUCCESS;
    REQUIRE_TRUE(flitzzip_test::source_writer_available(path, error));
    REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(path),
               std::string(reinterpret_cast<const char*>(valid.data()), valid.size()));
}

}  // namespace

// Purpose: Reject incorrect modulo-size trailers even when the entire payload and CRC remain valid.
// Inputs: Independent stored-DEFLATE "hello" frame with four wrong ISIZE values and both reader entry routes.
// Outputs: Terminal exact rejection, preserved caller buffers/source bytes, native owner release and valid reuse.
TEST_CASE(gzip_isize_mismatch_is_terminal_and_preserves_caller_buffers) {
    const auto root = test_temp_dir("gzip-isize-terminal");
    const auto path = root / "fixture.gz";
    const auto valid = gzip_header_fixture(0U);
    require_gzip_isize_valid_reuse(path, valid);
    for (const std::uint32_t wrong_size : {0U, 4U, 6U, 0xffffffffU}) {
        const auto damaged = gzip_wrong_isize_fixture(valid, wrong_size);
        for (const bool read_first : {false, true}) {
            write_gzip_header_fixture(path, damaged);
            require_gzip_isize_terminal(path, read_first);
            REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(path),
                       std::string(reinterpret_cast<const char*>(damaged.data()), damaged.size()));
            DWORD error = ERROR_SUCCESS;
            REQUIRE_TRUE(flitzzip_test::source_writer_available(path, error));
            require_gzip_isize_valid_reuse(path, valid);
        }
    }
}

// Purpose: Preserve caller-owned publication targets when a valid payload has an invalid size trailer.
// Inputs: Independently framed complete stream, existing member/sibling bytes and four isolated ISIZE mutations.
// Outputs: No partial publication/debris, exact source bytes and successful same-destination extraction afterward.
TEST_CASE(gzip_isize_mismatch_preserves_publication_target_then_recovers) {
    const auto root = test_temp_dir("gzip-isize-publication");
    const auto path = root / "fixture.gz";
    const auto valid = gzip_header_fixture(0U);
    for (const std::uint32_t wrong_size : {0U, 4U, 6U, 0xffffffffU}) {
        const auto destination = root / std::to_string(wrong_size);
        std::filesystem::create_directory(destination);
        std::ofstream(destination / "fixture", std::ios::binary) << "preserve caller target";
        std::ofstream(destination / "sibling.txt", std::ios::binary) << "unrelated owner";
        const auto damaged = gzip_wrong_isize_fixture(valid, wrong_size);
        write_gzip_header_fixture(path, damaged);
        require_gzip_isize_error([&] { (void)flitzzip::extract_gzip_file(path, destination, true); });
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(destination / "fixture"), "preserve caller target");
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(destination / "sibling.txt"), "unrelated owner");
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 2);
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(path),
                   std::string(reinterpret_cast<const char*>(damaged.data()), damaged.size()));
        DWORD error = ERROR_SUCCESS;
        REQUIRE_TRUE(flitzzip_test::source_writer_available(path, error));
        write_gzip_header_fixture(path, valid);
        const auto stats = flitzzip::extract_gzip_file(path, destination, true);
        REQUIRE_EQ(stats.entries, 1U);
        REQUIRE_EQ(stats.output_bytes, 5U);
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(destination / "fixture"), "hello");
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(destination / "sibling.txt"), "unrelated owner");
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 2);
        REQUIRE_TRUE(flitzzip_test::source_writer_available(path, error));
        REQUIRE_EQ(flitzzip_test::source_session_fixture_bytes(path),
                   std::string(reinterpret_cast<const char*>(valid.data()), valid.size()));
    }
}
