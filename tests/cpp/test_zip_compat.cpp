#include "test_util.hpp"
#include "core/archive_format.hpp"
#include "zip/zip_adapter.hpp"
#include "core/result.hpp"

#include <cstdint>
#include <fstream>
#include <string>
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

struct StoredZipEntry {
    std::string name;
    std::string payload;
};

// Purpose: Create a minimal stored ZIP with arbitrary raw entry names for hostile metadata tests.
// Inputs: `archive` is the output ZIP path and `entries` are written verbatim as stored file entries.
// Outputs: Writes a ZIP archive or throws through stream failure in the test body.
void write_stored_zip_with_entries(const std::filesystem::path& archive, const std::vector<StoredZipEntry>& entries) {
    std::ofstream out(archive, std::ios::binary | std::ios::trunc);
    struct CentralEntry {
        StoredZipEntry entry;
        std::uint32_t crc = 0;
        std::uint32_t local_offset = 0;
    };
    std::vector<CentralEntry> central_entries;
    central_entries.reserve(entries.size());
    for (const auto& entry : entries) {
        const auto crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(entry.payload.data()), entry.payload.size()));
        const auto name_size = static_cast<std::uint16_t>(entry.name.size());
        const auto payload_size = static_cast<std::uint32_t>(entry.payload.size());
        const auto local_offset = static_cast<std::uint32_t>(out.tellp());

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
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        out.write(entry.payload.data(), static_cast<std::streamsize>(entry.payload.size()));
        central_entries.push_back(CentralEntry{
            .entry = entry,
            .crc = crc,
            .local_offset = local_offset,
        });
    }

    const auto central_offset = static_cast<std::uint32_t>(out.tellp());
    for (const auto& central_entry : central_entries) {
        const auto name_size = static_cast<std::uint16_t>(central_entry.entry.name.size());
        const auto payload_size = static_cast<std::uint32_t>(central_entry.entry.payload.size());
        zip_write_u32(out, 0x02014B50U);
        zip_write_u16(out, 20);
        zip_write_u16(out, 20);
        zip_write_u16(out, 0);
        zip_write_u16(out, 0);
        zip_write_u16(out, 0);
        zip_write_u16(out, 0);
        zip_write_u32(out, central_entry.crc);
        zip_write_u32(out, payload_size);
        zip_write_u32(out, payload_size);
        zip_write_u16(out, name_size);
        zip_write_u16(out, 0);
        zip_write_u16(out, 0);
        zip_write_u16(out, 0);
        zip_write_u16(out, 0);
        zip_write_u32(out, 0);
        zip_write_u32(out, central_entry.local_offset);
        out.write(central_entry.entry.name.data(), static_cast<std::streamsize>(central_entry.entry.name.size()));
    }

    const auto central_size = static_cast<std::uint32_t>(static_cast<std::uint64_t>(out.tellp()) - central_offset);
    zip_write_u32(out, 0x06054B50U);
    zip_write_u16(out, 0);
    zip_write_u16(out, 0);
    zip_write_u16(out, static_cast<std::uint16_t>(entries.size()));
    zip_write_u16(out, static_cast<std::uint16_t>(entries.size()));
    zip_write_u32(out, central_size);
    zip_write_u32(out, central_offset);
    zip_write_u16(out, 0);
}

// Purpose: Create a minimal stored ZIP with one arbitrary raw entry name for hostile metadata tests.
// Inputs: `archive` is the output ZIP path, `entry_name` is written verbatim, and `payload` is stored uncompressed.
// Outputs: Writes a one-entry ZIP archive or throws through stream failure in the test body.
void write_stored_zip_with_name(const std::filesystem::path& archive, const std::string& entry_name, const std::string& payload) {
    write_stored_zip_with_entries(archive, {StoredZipEntry{.name = entry_name, .payload = payload}});
}

// Purpose: Flip one byte in the local-file payload of a handcrafted stored ZIP.
// Inputs: `archive` is a ZIP written by `write_stored_zip_with_name`, and `entry_name` gives the local name length.
// Outputs: Mutates the payload in place so extraction fails CRC validation after writing decoded bytes.
void corrupt_stored_zip_payload(const std::filesystem::path& archive, const std::string& entry_name) {
    constexpr std::streamoff local_header_bytes = 30;
    std::fstream file(archive, std::ios::binary | std::ios::in | std::ios::out);
    char value = 0;
    file.seekg(local_header_bytes + static_cast<std::streamoff>(entry_name.size()), std::ios::beg);
    file.read(&value, 1);
    value ^= 0x7F;
    file.seekp(local_header_bytes + static_cast<std::streamoff>(entry_name.size()), std::ios::beg);
    file.write(&value, 1);
}

// Purpose: Count files left under an extraction destination after a failed ZIP operation.
// Inputs: `root` is the extraction directory that may or may not exist.
// Outputs: Returns the number of regular files visible under `root`.
std::uint64_t count_regular_files(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        return 0;
    }
    std::uint64_t count = 0;
    for (const auto& item : std::filesystem::recursive_directory_iterator(root)) {
        if (item.is_regular_file()) {
            ++count;
        }
    }
    return count;
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

// Purpose: Verify `.zipx` files route through the ZIP-compatible reader without being treated as plain `.zip` in detection.
// Inputs: A standard ZIP archive copied to a `.zipx` path.
// Outputs: Throws if ZIPX detection or extraction regresses.
TEST_CASE(zipx_extracts_zip_compatible_records) {
    const auto root = test_temp_dir("zipx-compatible");
    const auto input = root / "input";
    std::filesystem::create_directories(input);
    std::ofstream(input / "hello.txt") << "hello zipx";
    const auto zip_archive = root / "archive.zip";
    const auto zipx_archive = root / "archive.zipx";
    (void)superzip::compress_zip({input}, zip_archive);
    std::filesystem::copy_file(zip_archive, zipx_archive);

    REQUIRE_EQ(superzip::detect_archive_format(zipx_archive), superzip::ArchiveFormat::Zipx);
    const auto output = root / "out";
    (void)superzip::extract_zip(zipx_archive, output, false);
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
        std::string("dir/control") + static_cast<char>(0x1F) + ".txt",
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

// Purpose: Verify ZIP extraction rejects duplicate normalized entry paths before filesystem output.
// Inputs: A handcrafted ZIP containing `dir/file.txt` and equivalent `dir//file.txt` entries.
// Outputs: Throws if ZIP extraction accepts ambiguous duplicate metadata.
TEST_CASE(zip_extract_rejects_duplicate_normalized_entry_paths) {
    const auto root = test_temp_dir("zip-duplicate-paths");
    const auto archive = root / "duplicate.zip";
    write_stored_zip_with_entries(
        archive,
        {
            StoredZipEntry{.name = "dir/file.txt", .payload = "first"},
            StoredZipEntry{.name = "dir//file.txt", .payload = "second"},
        });

    bool rejected = false;
    try {
        (void)superzip::extract_zip(archive, root / "out", true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
}

// Purpose: Verify ZIP extraction rejects a file entry that conflicts with a child entry path.
// Inputs: A handcrafted ZIP containing file `dir` and child file `dir/child.txt`.
// Outputs: Throws before extraction creates either output file.
TEST_CASE(zip_extract_rejects_file_entry_with_child_entry) {
    const auto root = test_temp_dir("zip-file-child-conflict");
    const auto archive = root / "conflict.zip";
    write_stored_zip_with_entries(
        archive,
        {
            StoredZipEntry{.name = "dir", .payload = "parent"},
            StoredZipEntry{.name = "dir/child.txt", .payload = "child"},
        });

    bool rejected = false;
    try {
        (void)superzip::extract_zip(archive, root / "out", true);
    } catch (const superzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
    std::filesystem::remove_all(root);
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

// Purpose: Verify failed ZIP extraction removes temporary data before publishing a final file.
// Inputs: A stored ZIP with a corrupted payload byte that fails miniz CRC validation.
// Outputs: Throws if extraction succeeds, leaves a partial file, or replaces an existing target.
TEST_CASE(zip_extract_removes_partial_file_after_crc_failure) {
    const auto root = test_temp_dir("zip-crc-cleanup");
    const auto archive = root / "corrupt.zip";
    const std::string entry_name = "file.bin";
    std::string payload(64 * 1024, '\0');
    std::uint32_t state = 0x87654321U;
    for (auto& byte : payload) {
        state = (state * 1664525U) + 1013904223U;
        byte = static_cast<char>((state >> 24U) & 0xFFU);
    }
    write_stored_zip_with_name(archive, entry_name, payload);
    corrupt_stored_zip_payload(archive, entry_name);

    const auto output = root / "out";
    bool rejected = false;
    try {
        (void)superzip::extract_zip(archive, output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output / entry_name));
    REQUIRE_EQ(count_regular_files(output), static_cast<std::uint64_t>(0));

    const auto overwrite_output = root / "overwrite-out";
    std::filesystem::create_directories(overwrite_output);
    const std::string preserved_text = "existing zip output should survive failed extraction";
    {
        std::ofstream preserved(overwrite_output / entry_name, std::ios::binary);
        preserved << preserved_text;
    }
    rejected = false;
    try {
        (void)superzip::extract_zip(archive, overwrite_output, true);
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(overwrite_output), static_cast<std::uint64_t>(1));
    REQUIRE_EQ(std::filesystem::file_size(overwrite_output / entry_name), static_cast<std::uintmax_t>(preserved_text.size()));
    std::ifstream preserved(overwrite_output / entry_name, std::ios::binary);
    std::string actual(preserved_text.size(), '\0');
    preserved.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    preserved.close();
    REQUIRE_EQ(actual, preserved_text);
    std::filesystem::remove_all(root);
}
