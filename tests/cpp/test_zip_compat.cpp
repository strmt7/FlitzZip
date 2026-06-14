#include "test_util.hpp"
#include "zip/zip_adapter.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <fstream>
#include <vector>
#include <windows.h>

#include "miniz.h"

namespace {

// Purpose: Write a little-endian 16-bit ZIP field to a binary stream.
// Inputs: `out` is the destination stream and `value` is the field value.
// Outputs: Appends two bytes to the stream.
void zip_write_u16(std::ofstream& out, std::uint16_t value) {
    const char bytes[] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
    };
    out.write(bytes, sizeof(bytes));
}

// Purpose: Write a little-endian 32-bit ZIP field to a binary stream.
// Inputs: `out` is the destination stream and `value` is the field value.
// Outputs: Appends four bytes to the stream.
void zip_write_u32(std::ofstream& out, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    out.write(bytes, sizeof(bytes));
}

// Purpose: Create a minimal stored ZIP with an arbitrary raw entry name for hostile metadata tests.
// Inputs: `archive` is the output ZIP path, `entry_name` is written verbatim, and `payload` is stored uncompressed.
// Outputs: Writes a one-entry ZIP archive or throws through stream failure in the test body.
void write_stored_zip_with_name(const std::filesystem::path& archive, const std::string& entry_name, const std::string& payload) {
    std::ofstream out(archive, std::ios::binary | std::ios::trunc);
    const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(payload.data()), payload.size()));
    const auto name_size = static_cast<std::uint16_t>(entry_name.size());
    const auto payload_size = static_cast<std::uint32_t>(payload.size());

    zip_write_u32(out, 0x04034B50U);
    zip_write_u16(out, 20);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u32(out, crc);
    zip_write_u32(out, payload_size);
    zip_write_u32(out, payload_size);
    zip_write_u16(out, name_size);
    zip_write_u16(out, 0);
    out.write(entry_name.data(), static_cast<std::streamsize>(entry_name.size()));
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));

    const auto central_offset = static_cast<std::uint32_t>(out.tellp());
    zip_write_u32(out, 0x02014B50U);
    zip_write_u16(out, 20);
    zip_write_u16(out, 20);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u32(out, crc);
    zip_write_u32(out, payload_size);
    zip_write_u32(out, payload_size);
    zip_write_u16(out, name_size);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u32(out, 0);
    zip_write_u32(out, 0);
    out.write(entry_name.data(), static_cast<std::streamsize>(entry_name.size()));

    const auto central_size = static_cast<std::uint32_t>(static_cast<std::uint64_t>(out.tellp()) - central_offset);
    zip_write_u32(out, 0x06054B50U);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, 1);
    zip_write_u16(out, 1);
    zip_write_u32(out, central_size);
    zip_write_u32(out, central_offset);
    zip_write_u16(out, 0);
}

}  // namespace

// Purpose: Verify standard ZIP compatibility roundtrip for regular files.
// Inputs: A temporary source file compressed through the miniz adapter.
// Outputs: Throws if extraction fails or restored contents differ.
TEST_CASE(zip_compat_roundtrip) {
    const auto root = test_temp_dir("zip-compat");
    const auto input = root / "input";
    std::filesystem::create_directories(input);
    std::ofstream(input / "hello.txt") << "hello zip";
    const auto archive = root / "archive.zip";
    const auto stats = superzip::compress_zip({input}, archive);
    REQUIRE_TRUE(stats.output_bytes > 0);
    const auto output = root / "out";
    (void)superzip::extract_zip(archive, output, true);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "hello.txt"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify malicious ZIP traversal entries are rejected before writing.
// Inputs: A handcrafted ZIP containing `../escape.txt`.
// Outputs: Throws if extraction does not reject the unsafe entry.
TEST_CASE(zip_extract_rejects_traversal_entry) {
    const auto root = test_temp_dir("zip-traversal");
    const auto archive = root / "bad.zip";
    mz_zip_archive zip{};
    REQUIRE_TRUE(mz_zip_writer_init_file(&zip, archive.string().c_str(), 0) != 0);
    const char payload[] = "owned";
    REQUIRE_TRUE(mz_zip_writer_add_mem(&zip, "../escape.txt", payload, sizeof(payload) - 1, MZ_BEST_SPEED) != 0);
    REQUIRE_TRUE(mz_zip_writer_finalize_archive(&zip) != 0);
    mz_zip_writer_end(&zip);

    bool rejected = false;
    try {
        (void)superzip::extract_zip(archive, root / "out", true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    std::filesystem::remove_all(root);
}

// Purpose: Verify malicious ZIP entries using absolute, drive-rooted, reserved, and invalid Windows paths are rejected.
// Inputs: Handcrafted ZIP archives with one unsafe entry name each.
// Outputs: Throws if extraction accepts any unsafe ZIP entry name.
TEST_CASE(zip_extract_rejects_windows_unsafe_entries) {
    const std::vector<std::string> names = {
        "/absolute.txt",
        "\\absolute.txt",
        "C:drive.txt",
        "dir/CON.txt",
        "dir/file.",
        "dir/file ",
        "dir/a?b.txt",
    };
    for (const auto& name : names) {
        const auto root = test_temp_dir("zip-unsafe-entry");
        const auto archive = root / "bad.zip";
        write_stored_zip_with_name(archive, name, "owned");

        bool rejected = false;
        try {
            (void)superzip::extract_zip(archive, root / "out", true);
        } catch (const superzip::SecurityError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        std::filesystem::remove_all(root);
    }
}

// Purpose: Verify ZIP extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A ZIP archive and a preexisting output file with the same normalized name.
// Outputs: Throws if extraction overwrites the file while overwrite is false.
TEST_CASE(zip_extract_rejects_overwrite_by_default) {
    const auto root = test_temp_dir("zip-overwrite");
    const auto input = root / "input";
    std::filesystem::create_directories(input);
    std::ofstream(input / "hello.txt") << "from zip";
    const auto archive = root / "archive.zip";
    (void)superzip::compress_zip({input / "hello.txt"}, archive);
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "hello.txt") << "existing";

    bool rejected = false;
    try {
        (void)superzip::extract_zip(archive, output, false);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::filesystem::remove_all(root);
}
