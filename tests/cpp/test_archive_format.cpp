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
// Inputs: `path` is the fixture location.
// Outputs: Creates a zero-entry SUZIP-shaped file with valid index/footer magic.
void write_minimal_suzip_fixture(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    const auto index_offset = static_cast<std::uint64_t>(output.tellp());
    superzip::write_u32(output, superzip::kSuperZipMagic);
    superzip::write_u32(output, superzip::kSuperZipVersion);
    superzip::write_u32(output, 0U);
    const auto index_size = static_cast<std::uint64_t>(output.tellp()) - index_offset;
    superzip::write_u32(output, superzip::kSuperZipFooterMagic);
    superzip::write_u32(output, superzip::kSuperZipVersion);
    superzip::write_u64(output, index_offset);
    superzip::write_u64(output, index_size);
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

TEST_CASE(archive_format_detects_real_archive_extensions) {
    const auto root = test_temp_dir("archive-format-extensions");
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.suzip"), superzip::ArchiveFormat::SuperZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.zip"), superzip::ArchiveFormat::Zip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.zipx"), superzip::ArchiveFormat::Zipx);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.7z"), superzip::ArchiveFormat::SevenZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.rar"), superzip::ArchiveFormat::Rar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar"), superzip::ArchiveFormat::Tar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.gz"), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tgz"), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.bz2"), superzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tbz"), superzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.xz"), superzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.txz"), superzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.lz"), superzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tlz"), superzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.cpio.gz"), superzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.cpgz"), superzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.lzma"), superzip::ArchiveFormat::Lzma);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.lz"), superzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.Z"), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.b64"), superzip::ArchiveFormat::Base64);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.cab"), superzip::ArchiveFormat::Cab);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.iso"), superzip::ArchiveFormat::Iso);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.ar"), superzip::ArchiveFormat::Ar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.arj"), superzip::ArchiveFormat::Arj);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.arc"), superzip::ArchiveFormat::Arc);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.ark"), superzip::ArchiveFormat::Arc);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.hqx"), superzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.macbin"), superzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.xxe"), superzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.uue"), superzip::ArchiveFormat::Uue);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.uu"), superzip::ArchiveFormat::Uue);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.lzh"), superzip::ArchiveFormat::Lha);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.wim"), superzip::ArchiveFormat::Wim);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.swm"), superzip::ArchiveFormat::SplitWim);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.xar"), superzip::ArchiveFormat::Xar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.deb"), superzip::ArchiveFormat::Deb);
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
    write_minimal_suzip_fixture(root / "renamed-native.bin");
    const auto uue_begin = std::array<unsigned char, 24>{'b', 'e', 'g', 'i', 'n', ' ', '6', '4', '4', ' ',  'p', 'a',
                                                         'y', 'l', 'o', 'a', 'd', '.', 't', 'x', 't', '\n', '`', '\n'};
    write_fixture(root / "uue.bin", uue_begin);
    const auto xxe_begin = std::array<unsigned char, 24>{'b', 'e', 'g', 'i', 'n', ' ', '6', '4', '4', ' ',  'p', 'a',
                                                         'y', 'l', 'o', 'a', 'd', '.', 't', 'x', 't', '\n', '+', '\n'};
    write_fixture(root / "xxe.bin", xxe_begin);

    REQUIRE_EQ(superzip::detect_archive_format(root / "zip.bin"), superzip::ArchiveFormat::Zip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "zipx.zipx"), superzip::ArchiveFormat::Zipx);
    REQUIRE_EQ(superzip::detect_archive_format(root / "seven.bin"), superzip::ArchiveFormat::SevenZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "rar.bin"), superzip::ArchiveFormat::Rar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "gz.bin"), superzip::ArchiveFormat::Gzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "package.cpgz"), superzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "unix-compress.bin"), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::detect_archive_format(root / "base64.bin"), superzip::ArchiveFormat::Base64);
    REQUIRE_EQ(superzip::detect_archive_format(root / "bz2.bin"), superzip::ArchiveFormat::Bzip2);
    REQUIRE_EQ(superzip::detect_archive_format(root / "xz.bin"), superzip::ArchiveFormat::Xz);
    REQUIRE_EQ(superzip::detect_archive_format(root / "lz.bin"), superzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.tar.lz"), superzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "zst.bin"), superzip::ArchiveFormat::Zstd);
    REQUIRE_EQ(superzip::detect_archive_format(root / "cab.bin"), superzip::ArchiveFormat::Cab);
    REQUIRE_EQ(superzip::detect_archive_format(root / "ar.bin"), superzip::ArchiveFormat::Ar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "arj.bin"), superzip::ArchiveFormat::Arj);
    REQUIRE_EQ(superzip::detect_archive_format(root / "arc.arc"), superzip::ArchiveFormat::Arc);
    REQUIRE_EQ(superzip::detect_archive_format(root / "hqx.bin"), superzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(superzip::detect_archive_format(root / "macbinary.bin"), superzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(superzip::detect_archive_format(root / "rpm.bin"), superzip::ArchiveFormat::Rpm);
    REQUIRE_EQ(superzip::detect_archive_format(root / "wim.bin"), superzip::ArchiveFormat::Wim);
    REQUIRE_EQ(superzip::detect_archive_format(root / "xar.bin"), superzip::ArchiveFormat::Xar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "renamed-native.bin"), superzip::ArchiveFormat::SuperZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "xxe.bin"), superzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(superzip::detect_archive_format(root / "uue.bin"), superzip::ArchiveFormat::Uue);

    write_fixture(root / "package.deb", std::array<unsigned char, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
    REQUIRE_EQ(superzip::detect_archive_format(root / "package.deb"), superzip::ArchiveFormat::Deb);
}

TEST_CASE(archive_format_does_not_false_positive_zip_based_containers) {
    const auto root = test_temp_dir("archive-format-excluded-aliases");
    // Guardrail: ZIP-magic document containers are not archive formats in SuperZip.
    write_fixture(root / "document.docx", std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04});

    REQUIRE_TRUE(!superzip::parse_archive_format_token("docx").has_value());
    REQUIRE_TRUE(!superzip::parse_archive_format_token("pptx").has_value());
    REQUIRE_TRUE(!superzip::parse_archive_format_token("xlsx").has_value());
    REQUIRE_EQ(superzip::detect_archive_format(root / "document.docx"), superzip::ArchiveFormat::Unknown);
    REQUIRE_EQ(superzip::parse_archive_format_token("tar").value(), superzip::ArchiveFormat::Tar);
    REQUIRE_EQ(superzip::parse_archive_format_token("tgz").value(), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("tbz").value(), superzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(superzip::parse_archive_format_token("txz").value(), superzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(superzip::parse_archive_format_token("tlz").value(), superzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("tzst").value(), superzip::ArchiveFormat::TarZstd);
    REQUIRE_EQ(superzip::parse_archive_format_token("cpio.gz").value(), superzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("cpgz").value(), superzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("gzip").value(), superzip::ArchiveFormat::Gzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("bzip2").value(), superzip::ArchiveFormat::Bzip2);
    REQUIRE_EQ(superzip::parse_archive_format_token("lzma").value(), superzip::ArchiveFormat::Lzma);
    REQUIRE_EQ(superzip::parse_archive_format_token("lzip").value(), superzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("lz").value(), superzip::ArchiveFormat::Lzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("zstd").value(), superzip::ArchiveFormat::Zstd);
    REQUIRE_EQ(superzip::parse_archive_format_token("zipx").value(), superzip::ArchiveFormat::Zipx);
    REQUIRE_EQ(superzip::parse_archive_format_token("compress").value(), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::parse_archive_format_token("unix-compress").value(), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::parse_archive_format_token("b64").value(), superzip::ArchiveFormat::Base64);
    REQUIRE_EQ(superzip::parse_archive_format_token("base64").value(), superzip::ArchiveFormat::Base64);
    REQUIRE_EQ(superzip::parse_archive_format_token("xxe").value(), superzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(superzip::parse_archive_format_token("xxencode").value(), superzip::ArchiveFormat::Xxe);
    REQUIRE_EQ(superzip::parse_archive_format_token("uu").value(), superzip::ArchiveFormat::Uue);
    REQUIRE_EQ(superzip::parse_archive_format_token("uuencode").value(), superzip::ArchiveFormat::Uue);
    REQUIRE_EQ(superzip::parse_archive_format_token("arj").value(), superzip::ArchiveFormat::Arj);
    REQUIRE_EQ(superzip::parse_archive_format_token("arc").value(), superzip::ArchiveFormat::Arc);
    REQUIRE_EQ(superzip::parse_archive_format_token("ark").value(), superzip::ArchiveFormat::Arc);
    REQUIRE_EQ(superzip::parse_archive_format_token("hqx").value(), superzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(superzip::parse_archive_format_token("binhex").value(), superzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(superzip::parse_archive_format_token("binhex4").value(), superzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(superzip::parse_archive_format_token("binhex40").value(), superzip::ArchiveFormat::Hqx);
    REQUIRE_EQ(superzip::parse_archive_format_token("macbinary").value(), superzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(superzip::parse_archive_format_token("macbin").value(), superzip::ArchiveFormat::MacBinary);
    REQUIRE_EQ(superzip::parse_archive_format_token("swm").value(), superzip::ArchiveFormat::SplitWim);
}

TEST_CASE(archive_format_display_names_are_precise_user_facing_format_names) {
    for (const auto& info : superzip::archive_format_registry()) {
        const auto display_name = lowercase_ascii(info.display_name);
        REQUIRE_TRUE(display_name.find("compatibility") == std::string::npos);
        REQUIRE_TRUE(display_name.find("single file") == std::string::npos);
        REQUIRE_TRUE(display_name.find("encoded file") == std::string::npos);
        REQUIRE_TRUE(display_name.find("stream") == std::string::npos);
        REQUIRE_TRUE(display_name.find(" file") == std::string::npos);
        REQUIRE_TRUE(display_name.find(" files") == std::string::npos);
    }
    REQUIRE_EQ(std::string(superzip::archive_format_info(superzip::ArchiveFormat::SuperZip).display_name),
               std::string("SuperZip (.suzip)"));
    REQUIRE_EQ(std::string(superzip::archive_format_info(superzip::ArchiveFormat::TarGzip).display_name),
               std::string("TAR.GZ (.tar.gz, .tgz)"));
    REQUIRE_EQ(std::string(superzip::archive_format_info(superzip::ArchiveFormat::Base64).display_name),
               std::string("Base64 (.b64)"));
    REQUIRE_EQ(std::string(superzip::archive_format_info(superzip::ArchiveFormat::Uue).display_name),
               std::string("UUencode (.uue, .uu)"));
}

TEST_CASE(archive_format_transfer_encodings_are_extract_only) {
    const auto& base64 = superzip::archive_format_info(superzip::ArchiveFormat::Base64);
    const auto& xxe = superzip::archive_format_info(superzip::ArchiveFormat::Xxe);
    const auto& uue = superzip::archive_format_info(superzip::ArchiveFormat::Uue);

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

    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.tar.gz"), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.tar.xz"), superzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.tar.lz"), superzip::ArchiveFormat::TarLzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.cpio.gz"), superzip::ArchiveFormat::CpioGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.cpgz"), superzip::ArchiveFormat::CpioGzip);
}
