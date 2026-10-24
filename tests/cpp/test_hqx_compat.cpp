#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "hqx/hqx_adapter.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint8_t kHqxRleMarker = 0x90U;
constexpr std::string_view kHqxAlphabet = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
constexpr std::string_view kHqxComment = "(This file must be converted with BinHex 4.0)";

// Purpose: Write exact text bytes to disk for handcrafted HQX fixtures.
// Inputs: `path` is the output file and `text` is the exact payload.
// Outputs: Creates or replaces the file.
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Write exact binary bytes to disk for overwrite-guard fixtures.
// Inputs: `path` is the output file and `bytes` is the exact payload.
// Outputs: Creates or replaces the file.
void write_binary_file(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Read a full file as unsigned binary bytes for equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for cleanup assertions.
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

// Purpose: Update a test-side BinHex CRC-16 register with one byte.
// Inputs: `crc` is the mutable register and `byte` is the next data byte.
// Outputs: Mutates `crc` using the documented BinHex bit order.
void update_hqx_crc(std::uint16_t& crc, std::uint8_t byte) {
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
        const bool carried = (crc & 0x8000U) != 0U;
        crc = static_cast<std::uint16_t>((crc << 1U) | ((byte >> 7U) & 0x01U));
        if (carried) {
            crc = static_cast<std::uint16_t>(crc ^ 0x1021U);
        }
        byte = static_cast<std::uint8_t>(byte << 1U);
    }
}

// Purpose: Compute a finalized test-side BinHex CRC over a byte vector.
// Inputs: `bytes` is the data region whose following two stored CRC bytes are modeled as zero during calculation.
// Outputs: Returns the finalized CRC expected in the HQX stream.
std::uint16_t hqx_crc_for(const std::vector<unsigned char>& bytes) {
    std::uint16_t crc = 0;
    for (const auto byte : bytes) {
        update_hqx_crc(crc, static_cast<std::uint8_t>(byte));
    }
    update_hqx_crc(crc, 0U);
    update_hqx_crc(crc, 0U);
    return crc;
}

// Purpose: Append one big-endian 16-bit integer to a test fixture buffer.
// Inputs: `bytes` is mutated and `value` is the integer to encode.
// Outputs: Appends two bytes.
void append_be16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Append one big-endian 32-bit integer to a test fixture buffer.
// Inputs: `bytes` is mutated and `value` is the integer to encode.
// Outputs: Appends four bytes.
void append_be32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Encode logical BinHex bytes with the minimal required RLE escaping.
// Inputs: `logical` is the header/fork/CRC byte stream before RLE.
// Outputs: Returns an RLE stream that escapes literal marker bytes and leaves other bytes uncompressed.
std::vector<unsigned char> rle_escape(const std::vector<unsigned char>& logical) {
    std::vector<unsigned char> escaped;
    escaped.reserve(logical.size());
    for (const auto byte : logical) {
        escaped.push_back(byte);
        if (byte == kHqxRleMarker) {
            escaped.push_back(0U);
        }
    }
    return escaped;
}

// Purpose: Encode RLE bytes into the HQX six-bit alphabet.
// Inputs: `bytes` is the RLE stream to encode.
// Outputs: Returns printable HQX characters without line wrapping.
std::string hqx_encode_sixbit(const std::vector<unsigned char>& bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t i = 0; i < bytes.size(); i += 3U) {
        const auto a = bytes[i];
        const auto has_b = i + 1U < bytes.size();
        const auto has_c = i + 2U < bytes.size();
        const auto b = has_b ? bytes[i + 1U] : 0U;
        const auto c = has_c ? bytes[i + 2U] : 0U;
        encoded.push_back(kHqxAlphabet[a >> 2U]);
        encoded.push_back(kHqxAlphabet[((a << 4U) | (b >> 4U)) & 0x3FU]);
        if (has_b) {
            encoded.push_back(kHqxAlphabet[((b << 2U) | (c >> 6U)) & 0x3FU]);
        }
        if (has_c) {
            encoded.push_back(kHqxAlphabet[c & 0x3FU]);
        }
    }
    return encoded;
}

// Purpose: Wrap HQX characters with the standard comment, start colon, line breaks, and end colon.
// Inputs: `encoded` contains only HQX alphabet characters.
// Outputs: Returns a complete text-mode `.hqx` fixture.
std::string wrap_hqx_text(const std::string& encoded) {
    std::string text;
    text.append(kHqxComment);
    text.append("\n:");
    std::size_t column = 0;
    for (const auto ch : encoded) {
        if (column == 64U) {
            text.push_back('\n');
            column = 0;
        }
        text.push_back(ch);
        ++column;
    }
    text.append(":\n");
    return text;
}

// Purpose: Serialize independent BinHex header, fork and CRC bytes before transfer encoding.
// Inputs: `name` is the embedded output name, `data` is the data fork, `resource` is the resource fork, and corruption
// flags flip stored CRC bits. Outputs: Returns logical bytes before RLE or six-bit encoding.
std::vector<unsigned char> make_hqx_logical_fixture(std::string_view name, const std::vector<unsigned char>& data,
                                                    const std::vector<unsigned char>& resource = {},
                                                    bool corrupt_header_crc = false, bool corrupt_data_crc = false,
                                                    bool corrupt_resource_crc = false) {
    std::vector<unsigned char> header;
    header.push_back(static_cast<unsigned char>(name.size()));
    header.insert(header.end(), name.begin(), name.end());
    header.push_back(0U);
    header.insert(header.end(), {'B', 'I', 'N', 'A'});
    header.insert(header.end(), {'S', 'Z', 'I', 'P'});
    append_be16(header, 0U);
    append_be32(header, static_cast<std::uint32_t>(data.size()));
    append_be32(header, static_cast<std::uint32_t>(resource.size()));

    std::vector<unsigned char> logical = header;
    append_be16(logical, static_cast<std::uint16_t>(hqx_crc_for(header) ^ (corrupt_header_crc ? 1U : 0U)));
    logical.insert(logical.end(), data.begin(), data.end());
    append_be16(logical, static_cast<std::uint16_t>(hqx_crc_for(data) ^ (corrupt_data_crc ? 1U : 0U)));
    logical.insert(logical.end(), resource.begin(), resource.end());
    append_be16(logical, static_cast<std::uint16_t>(hqx_crc_for(resource) ^ (corrupt_resource_crc ? 1U : 0U)));
    return logical;
}

// Purpose: Preserve the existing literal-escape BinHex fixture interface over one canonical logical serializer.
// Inputs: Filename, forks and optional CRC corruption controls. Outputs: Complete HQX text with literal RLE escapes.
std::string make_hqx_fixture(std::string_view name, const std::vector<unsigned char>& data,
                             const std::vector<unsigned char>& resource = {}, bool corrupt_header_crc = false,
                             bool corrupt_data_crc = false, bool corrupt_resource_crc = false) {
    const auto logical =
        make_hqx_logical_fixture(name, data, resource, corrupt_header_crc, corrupt_data_crc, corrupt_resource_crc);
    return wrap_hqx_text(hqx_encode_sixbit(rle_escape(logical)));
}

// Purpose: Encode real repeat runs using RFC 1741 Appendix A, independently of the production decoder.
// Inputs: Logical bytes, including literal marker values. Outputs: Runs split at 255 bytes and escaped markers.
std::vector<unsigned char> hqx_encode_runs(const std::vector<unsigned char>& logical) {
    std::vector<unsigned char> encoded;
    for (std::size_t offset = 0; offset < logical.size();) {
        std::size_t count = 1U;
        while (count < 255U && count < logical.size() - offset && logical[offset + count] == logical[offset]) {
            ++count;
        }
        const auto literals = count >= 3U ? 1U : count;
        for (std::size_t i = 0; i < literals; ++i) {
            encoded.push_back(logical[offset]);
            if (logical[offset] == kHqxRleMarker) {
                encoded.push_back(0U);
            }
        }
        if (count >= 3U) {
            encoded.push_back(kHqxRleMarker);
            encoded.push_back(static_cast<unsigned char>(count));
        }
        offset += count;
    }
    return encoded;
}

// Purpose: Require malformed transfer streams to preserve caller files and remove every temporary artifact.
// Inputs: Isolated root, exact transfer bytes and an expected failure fragment (empty accepts any parser error).
// Outputs: Real extraction rejects with one unchanged sentinel and no extra file or directory.
void require_hqx_rejected_without_publication(const std::filesystem::path& root, const std::string& text,
                                              std::string_view diagnostic = {}) {
    std::filesystem::create_directories(root);
    const auto archive = root / "input.hqx";
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    const auto sentinel = output / "existing.bin";
    const std::vector<unsigned char> original{0x10U, 0x90U, 0xFFU};
    write_binary_file(sentinel, original);
    write_text_file(archive, text);
    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, true);
    } catch (const flitzzip::ArchiveError& error) {
        if (std::string_view(error.what()).find(diagnostic) == std::string_view::npos) {
            throw std::runtime_error("Unexpected BinHex rejection for " + root.filename().string() + ": " +
                                     std::string(error.what()).substr(0U, 512U));
        }
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_binary_file(sentinel), original);
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(output), std::filesystem::directory_iterator{}), 1);
}

// Purpose: Corrupt the first encoded payload character with a byte outside the HQX alphabet.
// Inputs: `text` is a valid `.hqx` fixture.
// Outputs: Returns a copy that must fail before any final output is published.
std::string corrupt_first_hqx_payload_char(std::string text) {
    const auto start = text.find(':');
    for (std::size_t i = start + 1U; i < text.size(); ++i) {
        if (text[i] != '\r' && text[i] != '\n' && text[i] != '\t' && text[i] != ' ') {
            text[i] = '~';
            return text;
        }
    }
    return text;
}

}  // namespace

// Purpose: Verify BinHex extraction restores only the data fork and validates the stream by auto-detection.
// Inputs: A valid `.hqx` fixture with binary data and a non-empty resource fork.
// Outputs: Throws if detection, extraction statistics, or restored data bytes differ.
// Purpose: Decode real RLE runs at marker/count boundaries while independently validating both fork CRCs.
// Inputs: RFC 1741 run sizes around 3 and 255, including zero, literal markers and high bytes in both forks.
// Outputs: Exact data fork only; a partial preamble match does not consume the following valid transfer comment.
TEST_CASE(hqx_extracts_real_rle_runs_and_recovers_preamble_matching) {
    const auto root = test_temp_dir("hqx-real-runs");
    std::vector<unsigned char> payload;
    for (const auto byte : {0x00U, 0x90U, 0xFFU}) {
        for (const std::size_t count : {1U, 2U, 3U, 254U, 255U, 256U, 511U}) {
            payload.insert(payload.end(), count, static_cast<unsigned char>(byte));
            payload.push_back(0x37U);
        }
    }
    const std::vector<unsigned char> resource(511U, 0x90U);
    const auto logical = make_hqx_logical_fixture("payload.bin", payload, resource);
    const auto encoded = hqx_encode_runs(logical);
    REQUIRE_TRUE(encoded.size() < logical.size());
    const auto archive = root / "runs.hqx";
    write_text_file(archive, "Ignored partial marker (Thisx\n" + wrap_hqx_text(hqx_encode_sixbit(encoded)));
    const auto destination = root / "out";
    const auto result = flitzzip::extract_hqx_file(archive, destination, false);
    REQUIRE_EQ(result.entries, 1U);
    REQUIRE_EQ(result.output_bytes, payload.size());
    REQUIRE_EQ(read_binary_file(destination / "payload.bin"), payload);
    REQUIRE_EQ(count_regular_files(destination), 1U);
    std::filesystem::remove_all(root);
}

// Purpose: Preserve empty data/resource fork completion and publish real zero-byte files rather than omit them.
// Inputs: Every empty/nonempty fork combination with independently generated CRCs and transfer framing.
// Outputs: Exactly one regular data-fork file with the original bytes; resource-only input still publishes an empty
// file.
TEST_CASE(hqx_empty_forks_publish_exact_zero_byte_files) {
    const auto root = test_temp_dir("hqx-empty-forks");
    const std::array<std::vector<unsigned char>, 2> forks{{{}, {0x90U, 0x00U, 0xFFU}}};
    for (std::size_t data = 0; data < forks.size(); ++data) {
        for (std::size_t resource = 0; resource < forks.size(); ++resource) {
            const auto fixture = root / (std::to_string(data) + "-" + std::to_string(resource));
            std::filesystem::create_directories(fixture);
            const auto archive = fixture / "input.hqx";
            const auto output = fixture / "out";
            write_text_file(archive, make_hqx_fixture("zero.bin", forks[data], forks[resource]));
            const auto result = flitzzip::extract_hqx_file(archive, output, false);
            REQUIRE_EQ(result.entries, 1U);
            REQUIRE_EQ(result.output_bytes, forks[data].size());
            REQUIRE_TRUE(std::filesystem::is_regular_file(output / "zero.bin"));
            REQUIRE_EQ(read_binary_file(output / "zero.bin"), forks[data]);
            REQUIRE_EQ(count_regular_files(output), 1U);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Refuse every truncated logical BinHex record after otherwise complete transfer/RLE framing.
// Inputs: All shorter prefixes of independent header/data/resource/CRC bytes and an unchanged caller sentinel.
// Outputs: No partial publication or staging debris, including after data-fork writes; valid reuse still succeeds.
TEST_CASE(hqx_logical_truncations_preserve_destination_and_allow_recovery) {
    const auto root = test_temp_dir("hqx-logical-truncations");
    const std::vector<unsigned char> payload{0x00U, 0x90U, 0xFFU, 0x23U, 0x47U};
    const std::vector<unsigned char> resource{0x90U, 0x00U, 0x77U};
    const auto logical = make_hqx_logical_fixture("payload.bin", payload, resource);
    for (std::size_t length = 0; length < logical.size(); ++length) {
        const std::vector<unsigned char> prefix(logical.begin(), logical.begin() + static_cast<std::ptrdiff_t>(length));
        const auto fixture = root / std::to_string(length);
        require_hqx_rejected_without_publication(fixture, wrap_hqx_text(hqx_encode_sixbit(rle_escape(prefix))),
                                                 "before all CRC-checked sections were complete");
        write_text_file(fixture / "input.hqx", make_hqx_fixture("payload.bin", payload, resource));
        const auto result = flitzzip::extract_hqx_file(fixture / "input.hqx", fixture / "out", true);
        REQUIRE_EQ(result.output_bytes, payload.size());
        REQUIRE_EQ(read_binary_file(fixture / "out" / "payload.bin"), payload);
        REQUIRE_EQ(count_regular_files(fixture / "out"), 2U);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Validate outer transfer framing, RLE grammar and filename-length bounds before publishing any file.
// Inputs: Independently malformed preambles, delimiters, residual sextets, RLE controls and logical trailing bytes.
// Outputs: Exact diagnostic category and an unchanged destination with no staging debris for every malformed stream.
TEST_CASE(hqx_transfer_and_rle_grammar_reject_malformed_boundaries) {
    const auto root = test_temp_dir("hqx-transfer-framing");
    auto logical = make_hqx_logical_fixture("payload.bin", {0x11U}, {0x22U});
    const auto valid = make_hqx_fixture("payload.bin", {0x11U}, {0x22U});
    logical.push_back(0x33U);
    const std::vector<std::pair<std::string, std::string_view>> cases{
        {"Incomplete transfer comment", "required header comment"},
        {std::string(kHqxComment), "payload-start colon"},
        {std::string(kHqxComment) + "\nX", "not followed by a payload-start colon"},
        {std::string(65536U, 'x') + valid, "preamble exceeds"},
        {valid.substr(0U, valid.rfind(':')), "terminating colon"},
        {valid + "unexpected trailing bytes", "trailing data after the terminating colon"},
        {wrap_hqx_text("!"), "invalid trailing six-bit group"},
        {wrap_hqx_text(hqx_encode_sixbit({0x90U})), "missing its repeat count"},
        {wrap_hqx_text(hqx_encode_sixbit({0x90U, 0x02U})), "repeat appears before any byte"},
        {wrap_hqx_text(hqx_encode_sixbit({0x00U})), "filename length is outside"},
        {wrap_hqx_text(hqx_encode_sixbit({0x40U})), "filename length is outside"},
        {wrap_hqx_text(hqx_encode_sixbit(rle_escape(logical))), "decoded data after the resource fork"},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        require_hqx_rejected_without_publication(root / std::to_string(index), cases[index].first, cases[index].second);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Admit exact fork-work ceilings and reject their successors without allocating or writing those fork sizes.
// Inputs: Tiny CRC-correct headers declaring boundary, over-boundary and overflowing-32-bit fork sums.
// Outputs: Boundary headers reach the expected physical truncation; excessive work rejects at metadata admission.
TEST_CASE(hqx_declared_fork_work_has_exact_metadata_boundaries) {
    const auto root = test_temp_dir("hqx-fork-work-admission");
    constexpr std::uint32_t total_work_limit = 1024U * 1024U * 1024U;
    constexpr std::uint32_t resource_limit = 256U * 1024U * 1024U;
    struct ForkCase {
        std::uint32_t data;
        std::uint32_t resource;
        bool admitted;
    };
    for (const std::size_t name_length : {1U, 11U, 63U}) {
        // RFC 1741: name plus 20 header bytes and three two-byte CRCs consume decoded work too.
        const auto maximum_data = total_work_limit - static_cast<std::uint32_t>(name_length + 26U);
        const std::array<ForkCase, 7> cases{{
            {maximum_data, 0U, true},
            {maximum_data + 1U, 0U, false},
            {0U, resource_limit, true},
            {0U, resource_limit + 1U, false},
            {maximum_data - resource_limit, resource_limit, true},
            {maximum_data - resource_limit + 1U, resource_limit, false},
            {std::numeric_limits<std::uint32_t>::max(), 1U, false},
        }};
        for (std::size_t index = 0; index < cases.size(); ++index) {
            auto logical = make_hqx_logical_fixture(std::string(name_length, 'x'), {});
            const auto header_size = static_cast<std::size_t>(logical.front()) + 20U;
            REQUIRE_TRUE(logical.size() >= header_size + 2U);
            const auto [data, resource, admitted] = cases[index];
            for (std::size_t byte = 0; byte < 4U; ++byte) {
                logical[header_size - 8U + byte] = static_cast<unsigned char>(data >> ((3U - byte) * 8U));
                logical[header_size - 4U + byte] = static_cast<unsigned char>(resource >> ((3U - byte) * 8U));
            }
            const std::vector<unsigned char> header(logical.begin(),
                                                    logical.begin() + static_cast<std::ptrdiff_t>(header_size));
            const auto crc = hqx_crc_for(header);
            logical[header_size] = static_cast<unsigned char>(crc >> 8U);
            logical[header_size + 1U] = static_cast<unsigned char>(crc);
            const auto expected = admitted ? "before all CRC-checked sections were complete"
                                           : "declared fork work exceeds FlitzZip resource limits";
            const auto fixture = root / (std::to_string(name_length) + "-" + std::to_string(index));
            require_hqx_rejected_without_publication(fixture, wrap_hqx_text(hqx_encode_sixbit(rle_escape(logical))),
                                                     expected);
        }
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(hqx_extracts_data_fork_and_discards_resource_fork) {
    const auto root = test_temp_dir("hqx-data-fork");
    const auto archive = root / "payload.hqx";
    const std::vector<unsigned char> payload{'S', 'u', 'p', 'e', 'r', 0x90U, 'Z', 'i', 'p', 0x00U, 0xFFU};
    const std::vector<unsigned char> resource{'R', 'S', 'R', 'C'};
    write_text_file(archive, make_hqx_fixture("payload.bin", payload, resource));

    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Hqx);
    const auto output = root / "out";
    const auto stats = flitzzip::extract_hqx_file(archive, output, false);

    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_binary_file(output / "payload.bin"), payload);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(1));
    flitzzip_test::export_compat_fixture(archive, output);
    std::filesystem::remove_all(root);
}

// Purpose: Verify BinHex extraction rejects unsafe embedded names before publishing.
// Inputs: A valid `.hqx` fixture whose header name attempts path traversal.
// Outputs: Throws if extraction succeeds or writes outside the destination.
TEST_CASE(hqx_extract_rejects_unsafe_header_path) {
    const auto root = test_temp_dir("hqx-unsafe-path");
    const auto archive = root / "unsafe.hqx";
    write_text_file(archive, make_hqx_fixture("..\\escape.txt", {'x'}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify a bad BinHex header CRC prevents any output publication.
// Inputs: A fixture with one flipped stored header CRC bit.
// Outputs: Throws if extraction succeeds or leaves a final output file behind.
TEST_CASE(hqx_extract_rejects_bad_header_crc_without_output) {
    const auto root = test_temp_dir("hqx-bad-header-crc");
    const auto archive = root / "bad-header.hqx";
    write_text_file(archive, make_hqx_fixture("payload.txt", {'o', 'k'}, {}, true));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify a bad data-fork CRC prevents final output even after temporary data writes.
// Inputs: A fixture with valid header metadata and one flipped stored data CRC bit.
// Outputs: Throws if extraction succeeds or publishes the decoded data fork.
TEST_CASE(hqx_extract_rejects_bad_data_crc_without_output) {
    const auto root = test_temp_dir("hqx-bad-data-crc");
    const auto archive = root / "bad-data.hqx";
    write_text_file(archive, make_hqx_fixture("payload.txt", {'b', 'a', 'd'}, {}, false, true));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify resource-fork CRC validation is enforced even though resource bytes are discarded.
// Inputs: A fixture with a non-empty resource fork and one flipped stored resource CRC bit.
// Outputs: Throws if extraction succeeds or publishes a final data file.
TEST_CASE(hqx_extract_rejects_bad_resource_crc_without_output) {
    const auto root = test_temp_dir("hqx-bad-resource-crc");
    const auto archive = root / "bad-resource.hqx";
    write_text_file(archive, make_hqx_fixture("payload.txt", {'d', 'a', 't', 'a'}, {'r'}, false, false, true));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify invalid HQX alphabet bytes are rejected before final-file publication.
// Inputs: A valid fixture with the first encoded payload byte mutated to `~`.
// Outputs: Throws if extraction succeeds or leaves a final output file.
TEST_CASE(hqx_extract_rejects_invalid_hqx_character_without_output) {
    const auto root = test_temp_dir("hqx-invalid-char");
    const auto archive = root / "invalid.hqx";
    write_text_file(archive, corrupt_first_hqx_payload_char(make_hqx_fixture("payload.txt", {'x'})));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, true);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify BinHex extraction refuses to overwrite existing files by default.
// Inputs: A valid fixture extracted over an existing destination file.
// Outputs: Throws if overwrite refusal is not enforced.
TEST_CASE(hqx_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("hqx-overwrite");
    const auto archive = root / "payload.hqx";
    write_text_file(archive, make_hqx_fixture("payload.txt", {'n', 'e', 'w'}));

    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_binary_file(output / "payload.txt", {'o', 'l', 'd'});

    bool rejected = false;
    try {
        (void)flitzzip::extract_hqx_file(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_binary_file(output / "payload.txt"), std::vector<unsigned char>({'o', 'l', 'd'}));
    std::filesystem::remove_all(root);
}
