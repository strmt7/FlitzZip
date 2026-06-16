#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "iso/iso_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kTestIsoSectorBytes = 2048U;
constexpr std::uint32_t kTestIsoPvdSector = 16U;
constexpr std::uint32_t kTestIsoTerminatorSector = 17U;
constexpr std::uint32_t kTestIsoRootSector = 20U;
constexpr std::uint32_t kTestIsoDirSector = 21U;
constexpr std::uint32_t kTestIsoRootFileSector = 22U;
constexpr std::uint32_t kTestIsoNestedFileSector = 23U;

enum class IsoFixtureMode {
    Good,
    TruncatedFileExtent,
    MultiExtentFile,
    ReservedName,
    DirectoryCycle,
};

// Purpose: Read a full text file for ISO extraction equality checks.
// Inputs: `path` is the extracted file to read.
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

// Purpose: Convert a text ISO identifier into raw directory-record bytes.
// Inputs: `text` is an ISO file identifier such as `README.TXT;1`.
// Outputs: Returns byte-for-byte identifier data.
std::vector<unsigned char> iso_identifier(std::string_view text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

// Purpose: Return the byte offset for a logical ISO sector.
// Inputs: `sector` is a 2048-byte logical block number.
// Outputs: Returns the byte offset inside a fixture image.
std::size_t sector_offset(std::uint32_t sector) {
    return static_cast<std::size_t>(sector) * kTestIsoSectorBytes;
}

// Purpose: Write an ISO 9660 both-endian 32-bit field.
// Inputs: `buffer` receives bytes, `offset` is the first field byte, and `value` is the decoded number.
// Outputs: Writes little-endian and big-endian copies.
void put_both_endian_u32(std::span<unsigned char> buffer, std::size_t offset, std::uint32_t value) {
    buffer[offset + 0U] = static_cast<unsigned char>(value & 0xFFU);
    buffer[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    buffer[offset + 2U] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    buffer[offset + 3U] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
    buffer[offset + 4U] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
    buffer[offset + 5U] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
    buffer[offset + 6U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    buffer[offset + 7U] = static_cast<unsigned char>(value & 0xFFU);
}

// Purpose: Write an ISO 9660 both-endian 16-bit field.
// Inputs: `buffer` receives bytes, `offset` is the first field byte, and `value` is the decoded number.
// Outputs: Writes little-endian and big-endian copies.
void put_both_endian_u16(std::span<unsigned char> buffer, std::size_t offset, std::uint16_t value) {
    buffer[offset + 0U] = static_cast<unsigned char>(value & 0xFFU);
    buffer[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    buffer[offset + 2U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    buffer[offset + 3U] = static_cast<unsigned char>(value & 0xFFU);
}

// Purpose: Build one ISO 9660 directory record for handcrafted fixtures.
// Inputs: `extent_sector`/`data_length` describe payload location, `flags` controls file/directory bits, and `identifier` is the raw file identifier.
// Outputs: Returns a padded directory record ready for a directory extent or PVD root field.
std::vector<unsigned char> make_iso_record(
    std::uint32_t extent_sector,
    std::uint32_t data_length,
    std::uint8_t flags,
    const std::vector<unsigned char>& identifier) {
    const auto raw_length = 33U + identifier.size();
    const auto record_length = raw_length + (raw_length % 2U == 0U ? 0U : 1U);
    REQUIRE_TRUE(record_length <= 255U);
    std::vector<unsigned char> record(record_length, 0);
    record[0] = static_cast<unsigned char>(record_length);
    put_both_endian_u32(std::span<unsigned char>(record.data(), record.size()), 2U, extent_sector);
    put_both_endian_u32(std::span<unsigned char>(record.data(), record.size()), 10U, data_length);
    record[25] = flags;
    record[26] = 0U;
    record[27] = 0U;
    put_both_endian_u16(std::span<unsigned char>(record.data(), record.size()), 28U, 1U);
    record[32] = static_cast<unsigned char>(identifier.size());
    std::copy(identifier.begin(), identifier.end(), record.begin() + 33);
    return record;
}

// Purpose: Append a directory record inside a sector without crossing the sector boundary.
// Inputs: `image` receives bytes, `sector` selects the directory extent, `cursor` is advanced, and `record` is the encoded entry.
// Outputs: Copies `record` into the fixture image.
void append_iso_directory_record(
    std::vector<unsigned char>& image,
    std::uint32_t sector,
    std::size_t& cursor,
    const std::vector<unsigned char>& record) {
    REQUIRE_TRUE(cursor + record.size() <= kTestIsoSectorBytes);
    const auto offset = sector_offset(sector) + cursor;
    std::copy(record.begin(), record.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
    cursor += record.size();
}

// Purpose: Write a small file payload into one ISO sector.
// Inputs: `image` receives bytes, `sector` selects the file extent, and `payload` is copied at the sector start.
// Outputs: Writes the payload and leaves remaining sector bytes zero-filled.
void write_iso_payload(std::vector<unsigned char>& image, std::uint32_t sector, std::string_view payload) {
    REQUIRE_TRUE(payload.size() <= kTestIsoSectorBytes);
    const auto offset = sector_offset(sector);
    std::copy(payload.begin(), payload.end(), image.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Write Primary Volume Descriptor and terminator sectors for a fixture.
// Inputs: `image` receives descriptor bytes and `root_record` is the encoded root directory record.
// Outputs: Creates enough ISO metadata for SuperZip format detection and parsing.
void write_iso_descriptors(std::vector<unsigned char>& image, const std::vector<unsigned char>& root_record) {
    const auto pvd_offset = sector_offset(kTestIsoPvdSector);
    image[pvd_offset] = 1U;
    std::copy_n("CD001", 5, image.begin() + static_cast<std::ptrdiff_t>(pvd_offset + 1U));
    image[pvd_offset + 6U] = 1U;
    put_both_endian_u32(
        std::span<unsigned char>(image.data() + static_cast<std::ptrdiff_t>(pvd_offset), kTestIsoSectorBytes),
        80U,
        static_cast<std::uint32_t>(image.size() / kTestIsoSectorBytes));
    put_both_endian_u16(
        std::span<unsigned char>(image.data() + static_cast<std::ptrdiff_t>(pvd_offset), kTestIsoSectorBytes),
        128U,
        kTestIsoSectorBytes);
    std::copy(root_record.begin(), root_record.end(), image.begin() + static_cast<std::ptrdiff_t>(pvd_offset + 156U));

    const auto terminator_offset = sector_offset(kTestIsoTerminatorSector);
    image[terminator_offset] = 255U;
    std::copy_n("CD001", 5, image.begin() + static_cast<std::ptrdiff_t>(terminator_offset + 1U));
    image[terminator_offset + 6U] = 1U;
}

// Purpose: Write a compact ISO 9660 image fixture with optional malformed entry mode.
// Inputs: `archive` is the output path and `mode` selects the metadata scenario.
// Outputs: Creates a complete fixture image on disk.
void write_iso_fixture(const std::filesystem::path& archive, IsoFixtureMode mode) {
    std::vector<unsigned char> image(32U * kTestIsoSectorBytes, 0);
    const std::string root_payload = "root payload\n";
    const std::string nested_payload = "nested payload\n";
    const std::vector<unsigned char> current{0U};
    const std::vector<unsigned char> parent{1U};
    const auto root_record = make_iso_record(kTestIsoRootSector, kTestIsoSectorBytes, 0x02U, current);
    write_iso_descriptors(image, root_record);

    const bool truncated = mode == IsoFixtureMode::TruncatedFileExtent;
    const bool multi_extent = mode == IsoFixtureMode::MultiExtentFile;
    const bool reserved = mode == IsoFixtureMode::ReservedName;
    const bool directory_cycle = mode == IsoFixtureMode::DirectoryCycle;
    const std::uint32_t root_file_sector = truncated ? 31U : kTestIsoRootFileSector;
    const std::uint32_t root_file_size = truncated ? (2U * kTestIsoSectorBytes) : static_cast<std::uint32_t>(root_payload.size());
    const std::uint8_t root_file_flags = multi_extent ? 0x80U : 0U;
    const auto root_file_name = reserved ? iso_identifier("CON.TXT;1") : iso_identifier("ROOT.TXT;1");

    std::size_t root_cursor = 0;
    append_iso_directory_record(image, kTestIsoRootSector, root_cursor, root_record);
    append_iso_directory_record(image, kTestIsoRootSector, root_cursor, make_iso_record(kTestIsoRootSector, kTestIsoSectorBytes, 0x02U, parent));
    if (directory_cycle) {
        append_iso_directory_record(image, kTestIsoRootSector, root_cursor, make_iso_record(kTestIsoRootSector, kTestIsoSectorBytes, 0x02U, iso_identifier("LOOP")));
    }
    append_iso_directory_record(image, kTestIsoRootSector, root_cursor, make_iso_record(kTestIsoDirSector, kTestIsoSectorBytes, 0x02U, iso_identifier("DIR")));
    append_iso_directory_record(image, kTestIsoRootSector, root_cursor, make_iso_record(root_file_sector, root_file_size, root_file_flags, root_file_name));

    std::size_t dir_cursor = 0;
    append_iso_directory_record(image, kTestIsoDirSector, dir_cursor, make_iso_record(kTestIsoDirSector, kTestIsoSectorBytes, 0x02U, current));
    append_iso_directory_record(image, kTestIsoDirSector, dir_cursor, make_iso_record(kTestIsoRootSector, kTestIsoSectorBytes, 0x02U, parent));
    append_iso_directory_record(image, kTestIsoDirSector, dir_cursor, make_iso_record(kTestIsoNestedFileSector, static_cast<std::uint32_t>(nested_payload.size()), 0U, iso_identifier("README.;1")));

    if (!truncated) {
        write_iso_payload(image, kTestIsoRootFileSector, root_payload);
    }
    write_iso_payload(image, kTestIsoNestedFileSector, nested_payload);

    std::ofstream output(archive, std::ios::binary);
    output.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
}

}  // namespace

// Purpose: Verify native `.iso` extraction reads files and directories from a basic ISO 9660 image.
// Inputs: A handcrafted ISO fixture with root and nested file records.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(iso_extraction_reads_basic_iso9660_files_and_directories) {
    const auto root = test_temp_dir("iso-basic");
    const auto archive = root / "sample.iso";
    write_iso_fixture(archive, IsoFixtureMode::Good);

    REQUIRE_EQ(superzip::detect_archive_format(archive), superzip::ArchiveFormat::Iso);
    const auto info = superzip::archive_format_info(superzip::ArchiveFormat::Iso);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);

    const auto output = root / "out";
    const auto stats = superzip::extract_iso(archive, output, false);
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("root payload\n").size() + std::string_view("nested payload\n").size()));
    REQUIRE_EQ(read_text_file(output / "ROOT.TXT"), "root payload\n");
    REQUIRE_EQ(read_text_file(output / "DIR" / "README"), "nested payload\n");
}

// Purpose: Verify ISO extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A valid ISO file entry and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(iso_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("iso-overwrite");
    const auto archive = root / "sample.iso";
    write_iso_fixture(archive, IsoFixtureMode::Good);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "ROOT.TXT") << "old";

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_iso(archive, output, false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "ROOT.TXT"), "old");
}

// Purpose: Verify ISO extraction rejects extents that would read past the image before publishing output.
// Inputs: A handcrafted ISO with one file record pointing past EOF.
// Outputs: Throws without writing regular files to the destination tree.
TEST_CASE(iso_extraction_rejects_truncated_file_extent_before_output) {
    const auto root = test_temp_dir("iso-truncated");
    const auto archive = root / "bad.iso";
    write_iso_fixture(archive, IsoFixtureMode::TruncatedFileExtent);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_iso(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify ISO extraction fails closed for unsupported multi-extent records.
// Inputs: A handcrafted ISO with the multi-extent flag set on a file.
// Outputs: Throws before any output files are created.
TEST_CASE(iso_extraction_rejects_multi_extent_records_before_output) {
    const auto root = test_temp_dir("iso-multiextent");
    const auto archive = root / "bad.iso";
    write_iso_fixture(archive, IsoFixtureMode::MultiExtentFile);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_iso(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify ISO extraction applies Windows-safe archive path validation before output.
// Inputs: A handcrafted ISO with a reserved Windows device name.
// Outputs: Throws `SecurityError` and creates no extracted files.
TEST_CASE(iso_extraction_rejects_reserved_windows_names_before_output) {
    const auto root = test_temp_dir("iso-reserved");
    const auto archive = root / "bad.iso";
    write_iso_fixture(archive, IsoFixtureMode::ReservedName);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_iso(archive, root / "out", false));
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify ISO extraction rejects named directory cycles before repeated scanning can exhaust resources.
// Inputs: A handcrafted ISO with a child directory record pointing back to the root extent.
// Outputs: Throws `ArchiveError` and creates no extracted files.
TEST_CASE(iso_extraction_rejects_directory_extent_cycles_before_output) {
    const auto root = test_temp_dir("iso-cycle");
    const auto archive = root / "bad.iso";
    write_iso_fixture(archive, IsoFixtureMode::DirectoryCycle);

    bool rejected = false;
    try {
        static_cast<void>(superzip::extract_iso(archive, root / "out", false));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}
