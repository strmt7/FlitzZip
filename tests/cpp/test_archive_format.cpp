#include "test_util.hpp"

#include "core/archive_format.hpp"

#include <array>
#include <fstream>
#include <span>

namespace {

// Purpose: Write a small binary fixture for archive-format detection tests.
// Inputs: `path` is the fixture location and `bytes` is the exact payload.
// Outputs: Creates or replaces the fixture file.
void write_fixture(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE(archive_format_detects_real_archive_extensions) {
    const auto root = test_temp_dir("archive-format-extensions");
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.suzip"), superzip::ArchiveFormat::SuperZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.zip"), superzip::ArchiveFormat::Zip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.7z"), superzip::ArchiveFormat::SevenZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.rar"), superzip::ArchiveFormat::Rar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar"), superzip::ArchiveFormat::Tar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.gz"), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tgz"), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.bz2"), superzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tbz"), superzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.tar.xz"), superzip::ArchiveFormat::TarXz);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.Z"), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.cab"), superzip::ArchiveFormat::Cab);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.iso"), superzip::ArchiveFormat::Iso);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.ar"), superzip::ArchiveFormat::Ar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.lzh"), superzip::ArchiveFormat::Lha);
    REQUIRE_EQ(superzip::detect_archive_format(root / "sample.deb"), superzip::ArchiveFormat::Deb);
}

TEST_CASE(archive_format_detects_real_archive_magic_bytes) {
    const auto root = test_temp_dir("archive-format-magic");
    write_fixture(root / "zip.bin", std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04});
    write_fixture(root / "seven.bin", std::array<unsigned char, 6>{0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C});
    write_fixture(root / "rar.bin", std::array<unsigned char, 7>{'R', 'a', 'r', '!', 0x1A, 0x07, 0x00});
    write_fixture(root / "gz.bin", std::array<unsigned char, 2>{0x1F, 0x8B});
    write_fixture(root / "unix-compress.bin", std::array<unsigned char, 3>{0x1F, 0x9D, 0x90});
    write_fixture(root / "bz2.bin", std::array<unsigned char, 3>{'B', 'Z', 'h'});
    write_fixture(root / "xz.bin", std::array<unsigned char, 6>{0xFD, '7', 'z', 'X', 'Z', 0x00});
    write_fixture(root / "zst.bin", std::array<unsigned char, 4>{0x28, 0xB5, 0x2F, 0xFD});
    write_fixture(root / "cab.bin", std::array<unsigned char, 4>{'M', 'S', 'C', 'F'});
    write_fixture(root / "ar.bin", std::array<unsigned char, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
    write_fixture(root / "rpm.bin", std::array<unsigned char, 4>{0xED, 0xAB, 0xEE, 0xDB});

    REQUIRE_EQ(superzip::detect_archive_format(root / "zip.bin"), superzip::ArchiveFormat::Zip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "seven.bin"), superzip::ArchiveFormat::SevenZip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "rar.bin"), superzip::ArchiveFormat::Rar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "gz.bin"), superzip::ArchiveFormat::Gzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "unix-compress.bin"), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::detect_archive_format(root / "bz2.bin"), superzip::ArchiveFormat::Bzip2);
    REQUIRE_EQ(superzip::detect_archive_format(root / "xz.bin"), superzip::ArchiveFormat::Xz);
    REQUIRE_EQ(superzip::detect_archive_format(root / "zst.bin"), superzip::ArchiveFormat::Zstd);
    REQUIRE_EQ(superzip::detect_archive_format(root / "cab.bin"), superzip::ArchiveFormat::Cab);
    REQUIRE_EQ(superzip::detect_archive_format(root / "ar.bin"), superzip::ArchiveFormat::Ar);
    REQUIRE_EQ(superzip::detect_archive_format(root / "rpm.bin"), superzip::ArchiveFormat::Rpm);

    write_fixture(root / "package.deb", std::array<unsigned char, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
    REQUIRE_EQ(superzip::detect_archive_format(root / "package.deb"), superzip::ArchiveFormat::Deb);
}

TEST_CASE(archive_format_rejects_zip_based_non_archive_containers) {
    const auto root = test_temp_dir("archive-format-excluded-aliases");
    // Office documents are ZIP-based containers, not SuperZip archive formats.
    write_fixture(root / "document.docx", std::array<unsigned char, 4>{'P', 'K', 0x03, 0x04});

    REQUIRE_TRUE(!superzip::parse_archive_format_token("docx").has_value());
    REQUIRE_TRUE(!superzip::parse_archive_format_token("pptx").has_value());
    REQUIRE_TRUE(!superzip::parse_archive_format_token("xlsx").has_value());
    REQUIRE_EQ(superzip::detect_archive_format(root / "document.docx"), superzip::ArchiveFormat::Unknown);
    REQUIRE_EQ(superzip::parse_archive_format_token("tar").value(), superzip::ArchiveFormat::Tar);
    REQUIRE_EQ(superzip::parse_archive_format_token("tgz").value(), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("tbz").value(), superzip::ArchiveFormat::TarBzip2);
    REQUIRE_EQ(superzip::parse_archive_format_token("gzip").value(), superzip::ArchiveFormat::Gzip);
    REQUIRE_EQ(superzip::parse_archive_format_token("bzip2").value(), superzip::ArchiveFormat::Bzip2);
    REQUIRE_EQ(superzip::parse_archive_format_token("compress").value(), superzip::ArchiveFormat::UnixCompress);
    REQUIRE_EQ(superzip::parse_archive_format_token("unix-compress").value(), superzip::ArchiveFormat::UnixCompress);
}

TEST_CASE(archive_format_prefers_compound_tar_extensions_over_stream_magic) {
    const auto root = test_temp_dir("archive-format-compound");
    write_fixture(root / "logs.tar.gz", std::array<unsigned char, 2>{0x1F, 0x8B});
    write_fixture(root / "logs.tar.xz", std::array<unsigned char, 6>{0xFD, '7', 'z', 'X', 'Z', 0x00});

    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.tar.gz"), superzip::ArchiveFormat::TarGzip);
    REQUIRE_EQ(superzip::detect_archive_format(root / "logs.tar.xz"), superzip::ArchiveFormat::TarXz);
}
