#include "test_util.hpp"

#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <span>

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
