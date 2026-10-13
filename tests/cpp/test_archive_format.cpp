#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/archive_index.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace {

// Purpose: Write a small binary fixture for archive-format detection tests.
// Inputs: `path` is the fixture location and `bytes` is the exact payload.
// Outputs: Creates or replaces the fixture file.
void write_fixture(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Write a small text fixture for archive-format detection tests.
// Inputs: `path` is the fixture location and `text` is the exact payload.
// Outputs: Creates or replaces the fixture file.
void write_text_fixture(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Purpose: Write a minimal native archive shell for format-detection tests.
// Inputs: `path` is the fixture location, `version` is readable, and `prefix` mimics encoded payload bytes.
// Outputs: Creates a zero-entry FLZIP-shaped file with valid index/footer magic.
void write_minimal_flzip_fixture(const std::filesystem::path& path, std::uint32_t version = flitzzip::kFlitzZipVersion,
                                 std::span<const unsigned char> prefix = {}) {
    std::ofstream output(path, std::ios::binary);
    if (!prefix.empty()) {
        output.write(reinterpret_cast<const char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    }
    const auto index_offset = static_cast<std::uint64_t>(output.tellp());
    flitzzip::write_u32(output, flitzzip::kFlitzZipMagic);
    flitzzip::write_u32(output, version);
    flitzzip::write_u32(output, 0U);
    const auto index_size = static_cast<std::uint64_t>(output.tellp()) - index_offset;
    flitzzip::write_u32(output, flitzzip::kFlitzZipFooterMagic);
    flitzzip::write_u32(output, version);
    flitzzip::write_u64(output, index_offset);
    flitzzip::write_u64(output, index_size);
}

// Purpose: Update a CRC-16/XMODEM register for MacBinary format-detection fixtures.
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

// Purpose: Create a minimal MacBinary II header fixture for format-detection tests.
// Inputs: `path` is the fixture location.
// Outputs: Writes one 128-byte MacBinary II header with a valid CRC and zero-length forks.
void write_minimal_macbinary_fixture(const std::filesystem::path& path) {
    std::array<unsigned char, 128> header{};
    constexpr std::string_view name = "payload.txt";
    header[1] = static_cast<unsigned char>(name.size());
    for (std::size_t i = 0; i < name.size(); ++i) {
        header[2U + i] = static_cast<unsigned char>(name[i]);
    }
    header[122] = 0x81U;
    header[123] = 0x81U;
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < 124U; ++i) {
        update_crc16_xmodem(crc, static_cast<std::uint8_t>(header[i]));
    }
    header[124] = static_cast<unsigned char>((crc >> 8U) & 0xFFU);
    header[125] = static_cast<unsigned char>(crc & 0xFFU);
    write_fixture(path, header);
}

// Purpose: Return a lowercase copy for display-label policy tests.
// Inputs: `text` is ASCII registry metadata.
// Outputs: Returns a lowercase string for case-insensitive substring checks.
std::string lowercase_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered;
}

}  // namespace

// Purpose: Keep the renamed native format byte-compatible with an independently specified predecessor archive.
// Inputs: Literal version-one index/footer bytes, tested under both filename extensions.
// Outputs: Fails if detection, parsing, the public token, or default extension changes the compatibility contract.
TEST_CASE(flzip_rename_preserves_existing_archive_bytes) {
    constexpr std::array<unsigned char, 36> legacy_empty_archive{'S', 'U', 'Z', 'P', 1,  0, 0, 0, 0, 0, 0, 0,
                                                                 'S', 'U', 'Z', 'F', 1,  0, 0, 0, 0, 0, 0, 0,
                                                                 0,   0,   0,   0,   12, 0, 0, 0, 0, 0, 0, 0};
    const auto root = test_temp_dir("flzip_rename");
    for (const auto* filename : {"existing.suzip", "renamed.flzip"}) {
        const auto path = root / filename;
        write_fixture(path, legacy_empty_archive);
        REQUIRE_EQ(flitzzip::detect_archive_format(path), flitzzip::ArchiveFormat::FlitzZip);
        std::ifstream input(path, std::ios::binary);
        const auto index = flitzzip::read_archive_index(input);
        REQUIRE_EQ(index.version, 1U);
        REQUIRE_TRUE(index.entries.empty());
    }
    REQUIRE_EQ(flitzzip::parse_archive_format_token("flzip").value(), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("FLZIP").value(), flitzzip::ArchiveFormat::FlitzZip);
    std::filesystem::remove_all(root);
}

// Purpose: Recognize registered extensions without converting a Unicode basename through the ANSI code page.
// Inputs: Every extension row in lowercase and uppercase below a CJK/supplementary-plane filename.
// Outputs: Requires identical extension/display handling for both spellings; generic .bin still needs magic.
TEST_CASE(archive_format_unicode_filename_extensions) {
    for (const auto& row : flitzzip::archive_format_extension_registry()) {
        std::string extension(row.extension);
        for (const bool uppercase : {false, true}) {
            if (uppercase) {
                std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
                    return static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch);
                });
            }
            const auto name =
                std::u8string(u8"\u65e5\u672c-\U0001f4c1") + std::u8string(extension.begin(), extension.end());
            const std::filesystem::path path(name);
            const auto expected =
                std::string_view(row.extension) == ".bin" ? flitzzip::ArchiveFormat::Unknown : row.format;
            REQUIRE_EQ(flitzzip::detect_archive_format_by_extension(path), expected);
            REQUIRE_EQ(std::string_view(flitzzip::archive_format_extension_info_for_path(row.format, path).extension),
                       std::string_view(row.extension));
        }
    }
}

TEST_CASE(archive_format_detects_real_archive_extensions) {
    const auto root = test_temp_dir("archive-format-extensions");
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.flzip"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.zip"), flitzzip::ArchiveFormat::Zip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.zipx"), flitzzip::ArchiveFormat::Zipx);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.7z"), flitzzip::ArchiveFormat::SevenZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.rar"), flitzzip::ArchiveFormat::Rar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tar"), flitzzip::ArchiveFormat::Tar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tar.gz"), flitzzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tgz"), flitzzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tar.bz2"), flitzzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tbz"), flitzzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tar.xz"), flitzzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.txz"), flitzzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tar.lz"), flitzzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.tlz"), flitzzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.cpio.gz"), flitzzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.cpgz"), flitzzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.lzma"), flitzzip::ArchiveFormat::Lzma);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.lz"), flitzzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.Z"), flitzzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.b64"), flitzzip::ArchiveFormat::Base64);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.cab"), flitzzip::ArchiveFormat::Cab);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.iso"), flitzzip::ArchiveFormat::Iso);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.ar"), flitzzip::ArchiveFormat::Ar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.arj"), flitzzip::ArchiveFormat::Arj);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.arc"), flitzzip::ArchiveFormat::Arc);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.ark"), flitzzip::ArchiveFormat::Arc);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.hqx"), flitzzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.macbin"), flitzzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.xxe"), flitzzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.uue"), flitzzip::ArchiveFormat::Uue);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.uu"), flitzzip::ArchiveFormat::Uue);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.lzh"), flitzzip::ArchiveFormat::Lha);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.wim"), flitzzip::ArchiveFormat::Wim);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.swm"), flitzzip::ArchiveFormat::SplitWim);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.xar"), flitzzip::ArchiveFormat::Xar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "sample.deb"), flitzzip::ArchiveFormat::Deb);
}

TEST_CASE(archive_format_detects_real_archive_magic_bytes) {
    const auto root = test_temp_dir("archive-format-magic");
    write_fixture(root / "zip.bin", std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04});
    write_fixture(root / "zipx.zipx", std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04});
    write_fixture(root / "seven.bin", std::array<unsigned char, 6>{0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C});
    write_fixture(root / "rar.bin", std::array<unsigned char, 7>{'R', 'a', 'r', '!', 0x1A, 0x07, 0x00});
    write_fixture(root / "gz.bin", std::array<unsigned char, 2>{0x1F, 0x8B});
    write_fixture(root / "package.cpgz", std::array<unsigned char, 2>{0x1F, 0x8B});
    write_fixture(root / "unix-compress.bin", std::array<unsigned char, 3>{0x1F, 0x9D, 0x90});
    const auto b64_begin = std::array<unsigned char, 38>{
        'b', 'e', 'g', 'i', 'n', '-', 'b', 'a', 's', 'e',  '6', '4', ' ', '6', '4', '4', ' ', 'p', 'a',
        'y', 'l', 'o', 'a', 'd', '.', 't', 'x', 't', '\n', 'U', '2', 'F', 'm', 'Z', 'Q', '=', '=', '\n'};
    write_fixture(root / "base64.bin", b64_begin);
    write_fixture(root / "bz2.bin", std::array<unsigned char, 3>{'B', 'Z', 'h'});
    write_fixture(root / "xz.bin", std::array<unsigned char, 6>{0xFD, '7', 'z', 'X', 'Z', 0x00});
    write_fixture(root / "lz.bin", std::array<unsigned char, 6>{'L', 'Z', 'I', 'P', 1, 20});
    write_fixture(root / "logs.tar.lz", std::array<unsigned char, 6>{'L', 'Z', 'I', 'P', 1, 20});
    write_fixture(root / "zst.bin", std::array<unsigned char, 4>{0x28, 0xB5, 0x2F, 0xFD});
    write_fixture(root / "cab.bin", std::array<unsigned char, 4>{'M', 'S', 'C', 'F'});
    write_fixture(root / "ar.bin", std::array<unsigned char, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
    write_fixture(root / "arj.bin", std::array<unsigned char, 4>{0x60, 0xEA, 0x00, 0x00});
    write_fixture(root / "arc.arc", std::array<unsigned char, 2>{0x1A, 0x00});
    write_text_fixture(root / "hqx.bin", "(This file must be converted with BinHex 4.0)\n:abc:\n");
    write_minimal_macbinary_fixture(root / "macbinary.bin");
    write_fixture(root / "rpm.bin", std::array<unsigned char, 4>{0xED, 0xAB, 0xEE, 0xDB});
    write_fixture(root / "wim.bin", std::array<unsigned char, 8>{'M', 'S', 'W', 'I', 'M', 0x00, 0x00, 0x00});
    write_fixture(root / "xar.bin", std::array<unsigned char, 4>{'x', 'a', 'r', '!'});
    write_minimal_flzip_fixture(root / "renamed-native.bin");
    write_minimal_flzip_fixture(root / "renamed-native-v4.bin", 4U);
    write_minimal_flzip_fixture(root / "renamed-native-v5.bin", 5U);
    const std::array<unsigned char, 4> zstd_magic{0x28, 0xB5, 0x2F, 0xFD};
    write_minimal_flzip_fixture(root / "native-zstd.flzip", 6U, zstd_magic);
    write_minimal_flzip_fixture(root / "renamed-native-zstd.bin", 6U, zstd_magic);
    write_minimal_flzip_fixture(root / "renamed-native-zstd.zst", 6U, zstd_magic);
    const auto uue_begin = std::array<unsigned char, 24>{'b', 'e', 'g', 'i', 'n', ' ', '6', '4', '4', ' ',  'p', 'a',
                                                         'y', 'l', 'o', 'a', 'd', '.', 't', 'x', 't', '\n', '`', '\n'};
    write_fixture(root / "uue.bin", uue_begin);
    const auto xxe_begin = std::array<unsigned char, 24>{'b', 'e', 'g', 'i', 'n', ' ', '6', '4', '4', ' ',  'p', 'a',
                                                         'y', 'l', 'o', 'a', 'd', '.', 't', 'x', 't', '\n', '+', '\n'};
    write_fixture(root / "xxe.bin", xxe_begin);

    REQUIRE_EQ(flitzzip::detect_archive_format(root / "zip.bin"), flitzzip::ArchiveFormat::Zip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "zipx.zipx"), flitzzip::ArchiveFormat::Zipx);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "seven.bin"), flitzzip::ArchiveFormat::SevenZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "rar.bin"), flitzzip::ArchiveFormat::Rar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "gz.bin"), flitzzip::ArchiveFormat::Gzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "package.cpgz"), flitzzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "unix-compress.bin"), flitzzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "base64.bin"), flitzzip::ArchiveFormat::Base64);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "bz2.bin"), flitzzip::ArchiveFormat::Bzip2);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "xz.bin"), flitzzip::ArchiveFormat::Xz);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "lz.bin"), flitzzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "logs.tar.lz"), flitzzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "zst.bin"), flitzzip::ArchiveFormat::Zstd);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "cab.bin"), flitzzip::ArchiveFormat::Cab);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "ar.bin"), flitzzip::ArchiveFormat::Ar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "arj.bin"), flitzzip::ArchiveFormat::Arj);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "arc.arc"), flitzzip::ArchiveFormat::Arc);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "hqx.bin"), flitzzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "macbinary.bin"), flitzzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "rpm.bin"), flitzzip::ArchiveFormat::Rpm);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "wim.bin"), flitzzip::ArchiveFormat::Wim);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "xar.bin"), flitzzip::ArchiveFormat::Xar);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "renamed-native.bin"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "renamed-native-v4.bin"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "renamed-native-v5.bin"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "native-zstd.flzip"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "renamed-native-zstd.bin"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "renamed-native-zstd.zst"), flitzzip::ArchiveFormat::FlitzZip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "xxe.bin"), flitzzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "uue.bin"), flitzzip::ArchiveFormat::Uue);

    write_fixture(root / "package.deb", std::array<unsigned char, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "package.deb"), flitzzip::ArchiveFormat::Deb);
}

TEST_CASE(archive_format_does_not_false_positive_zip_based_containers) {
    const auto root = test_temp_dir("archive-format-excluded-aliases");
    // Guardrail: ZIP-magic document containers are not archive formats in FlitzZip.
    write_fixture(root / "document.docx", std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04});

    REQUIRE_TRUE(!flitzzip::parse_archive_format_token("docx").has_value());
    REQUIRE_TRUE(!flitzzip::parse_archive_format_token("pptx").has_value());
    REQUIRE_TRUE(!flitzzip::parse_archive_format_token("xlsx").has_value());
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "document.docx"), flitzzip::ArchiveFormat::Unknown);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("tar").value(), flitzzip::ArchiveFormat::Tar);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("tgz").value(), flitzzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("tbz").value(), flitzzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("txz").value(), flitzzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("tlz").value(), flitzzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("tzst").value(), flitzzip::ArchiveFormat::TarZstd);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("cpio.gz").value(), flitzzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("cpgz").value(), flitzzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("gzip").value(), flitzzip::ArchiveFormat::Gzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("bzip2").value(), flitzzip::ArchiveFormat::Bzip2);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("lzma").value(), flitzzip::ArchiveFormat::Lzma);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("lzip").value(), flitzzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("lz").value(), flitzzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("zstd").value(), flitzzip::ArchiveFormat::Zstd);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("zipx").value(), flitzzip::ArchiveFormat::Zipx);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("compress").value(), flitzzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("unix-compress").value(), flitzzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("b64").value(), flitzzip::ArchiveFormat::Base64);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("base64").value(), flitzzip::ArchiveFormat::Base64);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("xxe").value(), flitzzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("xxencode").value(), flitzzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("uu").value(), flitzzip::ArchiveFormat::Uue);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("uuencode").value(), flitzzip::ArchiveFormat::Uue);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("arj").value(), flitzzip::ArchiveFormat::Arj);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("arc").value(), flitzzip::ArchiveFormat::Arc);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("ark").value(), flitzzip::ArchiveFormat::Arc);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("hqx").value(), flitzzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("binhex").value(), flitzzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("binhex4").value(), flitzzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("binhex40").value(), flitzzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("macbinary").value(), flitzzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("macbin").value(), flitzzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(flitzzip::parse_archive_format_token("swm").value(), flitzzip::ArchiveFormat::SplitWim);
}

TEST_CASE(archive_format_display_names_are_precise_user_facing_format_names) {
    for (const auto& info : flitzzip::archive_format_registry()) {
        const auto display_name = lowercase_ascii(info.display_name);
        REQUIRE_TRUE(display_name.find("compatibility") == std::string::npos);
        REQUIRE_TRUE(display_name.find("single file") == std::string::npos);
        REQUIRE_TRUE(display_name.find("encoded file") == std::string::npos);
        REQUIRE_TRUE(display_name.find("stream") == std::string::npos);
        REQUIRE_TRUE(display_name.find(" file") == std::string::npos);
        REQUIRE_TRUE(display_name.find(" files") == std::string::npos);
        REQUIRE_TRUE(display_name.find(", .") == std::string::npos);
    }
    REQUIRE_EQ(std::string(flitzzip::archive_format_info(flitzzip::ArchiveFormat::FlitzZip).display_name),
               std::string("FlitzZip (.flzip)"));
    REQUIRE_EQ(std::string(flitzzip::archive_format_info(flitzzip::ArchiveFormat::TarGzip).display_name),
               std::string("TAR.GZ (.tar.gz)"));
    REQUIRE_EQ(std::string(flitzzip::archive_format_info(flitzzip::ArchiveFormat::Base64).display_name),
               std::string("Base64 (.b64)"));
    REQUIRE_EQ(std::string(flitzzip::archive_format_info(flitzzip::ArchiveFormat::Uue).display_name),
               std::string("UUencode (.uue)"));
}

TEST_CASE(archive_format_extension_display_names_are_exact_per_extension) {
    for (const auto& info : flitzzip::archive_format_extension_registry()) {
        REQUIRE_TRUE(std::string_view(info.extension).starts_with("."));
        REQUIRE_TRUE(std::string_view(info.display_name).find(", .") == std::string_view::npos);
    }
    REQUIRE_EQ(std::string(flitzzip::archive_format_extension_info_for_extension(".tgz").display_name),
               std::string("TAR.GZ (.tgz)"));
    REQUIRE_EQ(std::string(flitzzip::archive_format_extension_info_for_extension(".zstd").display_name),
               std::string("Zstandard (.zstd)"));
    REQUIRE_EQ(std::string(flitzzip::archive_format_extension_info_for_extension(".ark").display_name),
               std::string("SEA ARC/ARK (.ark)"));
    REQUIRE_EQ(std::string(flitzzip::archive_format_extension_info_for_extension(".uu").display_name),
               std::string("UUencode (.uu)"));
    REQUIRE_EQ(
        std::string(flitzzip::archive_format_extension_info_for_path(flitzzip::ArchiveFormat::MacBinary, "payload.bin")
                        .display_name),
        std::string("MacBinary (.bin)"));
    REQUIRE_EQ(
        std::string(flitzzip::archive_format_extension_info_for_path(flitzzip::ArchiveFormat::Zstd, "payload.data")
                        .display_name),
        std::string("Zstandard (.zst)"));
}

TEST_CASE(archive_format_extension_only_detection_uses_registry_without_magic_probe) {
    const auto root = test_temp_dir("archive-format-extension-only");
    REQUIRE_EQ(flitzzip::detect_archive_format_by_extension(root / "logs.tar.gz"), flitzzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format_by_extension(root / "logs.tgz"), flitzzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format_by_extension(root / "payload.Z"), flitzzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(flitzzip::detect_archive_format_by_extension(root / "sample.docx"), flitzzip::ArchiveFormat::Unknown);
    REQUIRE_EQ(flitzzip::detect_archive_format_by_extension(root / "folder"), flitzzip::ArchiveFormat::Unknown);
}

TEST_CASE(archive_format_transfer_encodings_are_extract_only) {
    const auto& base64 = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Base64);
    const auto& xxe = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Xxe);
    const auto& uue = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Uue);

    REQUIRE_TRUE(!base64.can_create);
    REQUIRE_TRUE(base64.can_extract);
    REQUIRE_TRUE(!xxe.can_create);
    REQUIRE_TRUE(xxe.can_extract);
    REQUIRE_TRUE(!uue.can_create);
    REQUIRE_TRUE(uue.can_extract);
}

TEST_CASE(archive_format_prefers_compound_tar_extensions_over_stream_magic) {
    const auto root = test_temp_dir("archive-format-compound");
    write_fixture(root / "logs.tar.gz", std::array<unsigned char, 2>{0x1F, 0x8B});
    write_fixture(root / "logs.tar.xz", std::array<unsigned char, 6>{0xFD, '7', 'z', 'X', 'Z', 0x00});
    write_fixture(root / "logs.tar.lz", std::array<unsigned char, 6>{'L', 'Z', 'I', 'P', 1, 20});
    write_fixture(root / "logs.cpio.gz", std::array<unsigned char, 2>{0x1F, 0x8B});
    write_fixture(root / "logs.cpgz", std::array<unsigned char, 2>{0x1F, 0x8B});

    REQUIRE_EQ(flitzzip::detect_archive_format(root / "logs.tar.gz"), flitzzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "logs.tar.xz"), flitzzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "logs.tar.lz"), flitzzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "logs.cpio.gz"), flitzzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(flitzzip::detect_archive_format(root / "logs.cpgz"), flitzzip::ArchiveFormat::CpioGzip);
}
