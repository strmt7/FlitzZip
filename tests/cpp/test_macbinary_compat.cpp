#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "macbinary/macbinary_adapter.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace {

// Purpose: Write exact binary bytes to disk for MacBinary tests.
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
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
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

// Purpose: Round a fixture length to the next MacBinary 128-byte boundary.
// Inputs: `value` is a fork length.
// Outputs: Returns `value` padded to a 128-byte packet boundary.
std::size_t round_up_macbinary_block(std::size_t value) {
    return value == 0U ? 0U : ((value + 127U) / 128U) * 128U;
}

// Purpose: Write a big-endian 16-bit integer into a MacBinary fixture header.
// Inputs: `header` is mutated, `offset` is the first byte, and `value` is encoded.
// Outputs: Writes two bytes into `header`.
void write_be16(std::array<unsigned char, 128>& header, std::size_t offset, std::uint16_t value) {
    header[offset] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    header[offset + 1U] = static_cast<unsigned char>(value & 0xFFU);
}

// Purpose: Write a big-endian 32-bit integer into a MacBinary fixture header.
// Inputs: `header` is mutated, `offset` is the first byte, and `value` is encoded.
// Outputs: Writes four bytes into `header`.
void write_be32(std::array<unsigned char, 128>& header, std::size_t offset, std::uint32_t value) {
    header[offset] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
    header[offset + 1U] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    header[offset + 2U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    header[offset + 3U] = static_cast<unsigned char>(value & 0xFFU);
}

// Purpose: Update a test-side CRC-16/XMODEM register with one byte.
// Inputs: `crc` is the mutable register and `byte` is the next header byte.
// Outputs: Mutates `crc`.
void update_crc16_xmodem(std::uint16_t& crc, std::uint8_t byte) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8U));
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
        if ((crc & 0x8000U) != 0U) {
            crc = static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U);
        } else {
            crc = static_cast<std::uint16_t>(crc << 1U);
        }
    }
}

// Purpose: Compute a MacBinary II/III fixture header CRC.
// Inputs: `header` contains the 128-byte fixture header before the stored CRC is written.
// Outputs: Returns the CRC over bytes 0 through 123.
std::uint16_t macbinary_header_crc(const std::array<unsigned char, 128>& header) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < 124U; ++i) {
        update_crc16_xmodem(crc, static_cast<std::uint8_t>(header[i]));
    }
    return crc;
}

// Purpose: Build a complete MacBinary fixture with optional MacBinary II header CRC.
// Inputs: `name` is the embedded output name, `data` is the data fork, `resource` is the resource fork, `with_crc` selects MacBinary II markers, and `corrupt_crc` flips the stored CRC.
// Outputs: Returns a complete MacBinary byte stream.
std::vector<unsigned char> make_macbinary_fixture(
    std::string_view name,
    const std::vector<unsigned char>& data,
    const std::vector<unsigned char>& resource = {},
    bool with_crc = true,
    bool corrupt_crc = false) {
    std::array<unsigned char, 128> header{};
    header[1] = static_cast<unsigned char>(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        header[2U + i] = static_cast<unsigned char>(name[i]);
    }
    header[65] = static_cast<unsigned char>('B');
    header[66] = static_cast<unsigned char>('I');
    header[67] = static_cast<unsigned char>('N');
    header[68] = static_cast<unsigned char>('A');
    header[69] = static_cast<unsigned char>('S');
    header[70] = static_cast<unsigned char>('Z');
    header[71] = static_cast<unsigned char>('I');
    header[72] = static_cast<unsigned char>('P');
    write_be32(header, 83U, static_cast<std::uint32_t>(data.size()));
    write_be32(header, 87U, static_cast<std::uint32_t>(resource.size()));
    if (with_crc) {
        header[122] = 0x81U;
        header[123] = 0x81U;
        write_be16(header, 124U, static_cast<std::uint16_t>(macbinary_header_crc(header) ^ (corrupt_crc ? 1U : 0U)));
    }

    std::vector<unsigned char> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), data.begin(), data.end());
    bytes.resize(128U + round_up_macbinary_block(data.size()), 0U);
    bytes.insert(bytes.end(), resource.begin(), resource.end());
    bytes.resize(128U + round_up_macbinary_block(data.size()) + round_up_macbinary_block(resource.size()), 0U);
    return bytes;
}

}  // namespace

// Purpose: Verify MacBinary extraction restores only the data fork and discards resource-fork metadata.
// Inputs: A valid MacBinary II fixture with binary data and a non-empty resource fork.
// Outputs: Throws if detection, extraction stats, or restored bytes differ.
TEST_CASE(macbinary_extracts_data_fork_and_discards_resource_fork) {
    const auto root = test_temp_dir("macbinary-data-fork");
    const auto archive = root / "payload.bin";
    const std::vector<unsigned char> payload{'M', 'a', 'c', 0x00U, 'B', 'i', 'n', 0xFFU};
    const std::vector<unsigned char> resource{'R', 'S', 'R', 'C'};
    write_binary_file(archive, make_macbinary_fixture("payload.dat", payload, resource));

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::MacBinary);
    const auto output = root / "out";
    const auto stats = superzip::extract_macbinary_file(archive, output, false);

    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_binary_file(output / "payload.dat"), payload);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(1));
    std::filesystem::remove_all(root);
}

// Purpose: Verify extension-selected legacy MacBinary I extraction still works when no header CRC exists.
// Inputs: A structurally valid `.macbin` fixture without MacBinary II version markers.
// Outputs: Throws if extraction fails or restored data differs.
TEST_CASE(macbinary_extracts_legacy_v1_macbin_by_extension) {
    const auto root = test_temp_dir("macbinary-v1");
    const auto archive = root / "legacy.macbin";
    const std::vector<unsigned char> payload{'v', '1'};
    write_binary_file(archive, make_macbinary_fixture("legacy.txt", payload, {}, false));

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::MacBinary);
    const auto output = root / "out";
    const auto stats = superzip::extract_macbinary_file(archive, output, false);

    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(payload.size()));
    REQUIRE_EQ(read_binary_file(output / "legacy.txt"), payload);
    std::filesystem::remove_all(root);
}

// Purpose: Verify legacy MacBinary I streams do not claim generic `.bin` files without strong markers.
// Inputs: A structurally valid MacBinary I header saved with the ambiguous `.bin` extension.
// Outputs: Throws if generic binary detection recognizes the unmarked stream as MacBinary.
TEST_CASE(macbinary_does_not_auto_detect_legacy_v1_bin_without_crc_marker) {
    const auto root = test_temp_dir("macbinary-v1-bin");
    const auto archive = root / "legacy.bin";
    write_binary_file(archive, make_macbinary_fixture("legacy.txt", {'v', '1'}, {}, false));

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Unknown);
    std::filesystem::remove_all(root);
}

// Purpose: Verify generic `.bin` files without MacBinary markers are not auto-detected as MacBinary.
// Inputs: A non-MacBinary binary file using the ambiguous `.bin` extension.
// Outputs: Throws if format detection falsely recognizes MacBinary.
TEST_CASE(macbinary_does_not_false_positive_generic_bin) {
    const auto root = test_temp_dir("macbinary-generic-bin");
    const auto archive = root / "random.bin";
    write_binary_file(archive, {'n', 'o', 't', ' ', 'm', 'a', 'c'});

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Unknown);
    std::filesystem::remove_all(root);
}

// Purpose: Verify MacBinary extraction rejects unsafe embedded names before publishing.
// Inputs: A valid MacBinary II fixture whose header name attempts traversal.
// Outputs: Throws if extraction succeeds or writes outside the destination.
TEST_CASE(macbinary_extract_rejects_unsafe_header_path) {
    const auto root = test_temp_dir("macbinary-unsafe-path");
    const auto archive = root / "unsafe.bin";
    write_binary_file(archive, make_macbinary_fixture("..\\escape.txt", {'x'}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify classic Mac path separators are rejected before output publication.
// Inputs: A valid MacBinary II fixture whose header name contains `:`.
// Outputs: Throws if extraction accepts the unsafe platform-specific separator.
TEST_CASE(macbinary_extract_rejects_classic_colon_separator) {
    const auto root = test_temp_dir("macbinary-colon");
    const auto archive = root / "colon.bin";
    write_binary_file(archive, make_macbinary_fixture("bad:name.txt", {'x'}));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify a bad MacBinary II header CRC prevents output publication.
// Inputs: A fixture with a flipped stored header CRC bit.
// Outputs: Throws if extraction succeeds or leaves a final file behind.
TEST_CASE(macbinary_extract_rejects_bad_header_crc_without_output) {
    const auto root = test_temp_dir("macbinary-bad-crc");
    const auto archive = root / "bad.bin";
    write_binary_file(archive, make_macbinary_fixture("payload.txt", {'o', 'k'}, {}, true, true));

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify truncated MacBinary data forks do not publish partial outputs.
// Inputs: A valid fixture truncated before the declared data fork completes.
// Outputs: Throws if extraction succeeds or leaves a final file behind.
TEST_CASE(macbinary_extract_rejects_truncated_data_without_output) {
    const auto root = test_temp_dir("macbinary-truncated-data");
    const auto archive = root / "truncated.bin";
    auto bytes = make_macbinary_fixture("payload.txt", {'d', 'a', 't', 'a'});
    bytes.resize(130U);
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify truncated MacBinary resource forks are rejected even though only data forks are published.
// Inputs: A valid fixture truncated before the declared resource fork completes.
// Outputs: Throws if extraction ignores the malformed metadata extent or publishes output.
TEST_CASE(macbinary_extract_rejects_truncated_resource_without_output) {
    const auto root = test_temp_dir("macbinary-truncated-resource");
    const auto archive = root / "truncated-resource.bin";
    auto bytes = make_macbinary_fixture("payload.txt", {'d', 'a', 't', 'a'}, {'r', 's', 'r', 'c'});
    bytes.resize(128U + round_up_macbinary_block(4U) + 2U);
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / "payload.txt"));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify MacBinary signed fork lengths are rejected before copy loops run.
// Inputs: A header with the high bit set in the data-fork length field.
// Outputs: Throws if the unsupported signed length is accepted.
TEST_CASE(macbinary_extract_rejects_signed_fork_length_without_output) {
    const auto root = test_temp_dir("macbinary-signed-length");
    const auto archive = root / "signed.bin";
    auto bytes = make_macbinary_fixture("payload.txt", {'x'});
    bytes[83] = 0x80U;
    write_binary_file(archive, bytes);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify a zero-length MacBinary data fork publishes an empty regular file safely.
// Inputs: A valid MacBinary II fixture with no data-fork bytes.
// Outputs: Throws if the empty output file is missing or non-empty.
TEST_CASE(macbinary_extracts_zero_length_data_fork) {
    const auto root = test_temp_dir("macbinary-empty-data");
    const auto archive = root / "empty.bin";
    write_binary_file(archive, make_macbinary_fixture("empty.txt", {}));

    const auto output = root / "out";
    const auto stats = superzip::extract_macbinary_file(archive, output, false);

    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(0));
    REQUIRE_TRUE(std::filesystem::exists(output / "empty.txt"));
    REQUIRE_EQ(read_binary_file(output / "empty.txt").size(), static_cast<std::size_t>(0));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(1));
    std::filesystem::remove_all(root);
}

// Purpose: Verify MacBinary extraction refuses to overwrite existing files by default.
// Inputs: A valid fixture extracted over an existing destination file.
// Outputs: Throws if overwrite refusal is not enforced.
TEST_CASE(macbinary_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("macbinary-overwrite");
    const auto archive = root / "payload.bin";
    write_binary_file(archive, make_macbinary_fixture("payload.txt", {'n', 'e', 'w'}));

    const auto output = root / "out";
    std::filesystem::create_directories(output);
    write_binary_file(output / "payload.txt", {'o', 'l', 'd'});

    bool rejected = false;
    try {
        (void)superzip::extract_macbinary_file(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_binary_file(output / "payload.txt"), std::vector<unsigned char>({'o', 'l', 'd'}));
    std::filesystem::remove_all(root);
}
