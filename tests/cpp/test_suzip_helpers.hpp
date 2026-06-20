#pragma once

#include "core/archive_index.hpp"
#include "core/archive.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>

namespace superzip_test {

inline constexpr std::uint32_t kTestFooterMagic = 0x465A5553;  // SUZF

// Purpose: Overwrite a little-endian 64-bit field at a fixed archive byte offset.
// Inputs: `path` identifies the archive, `offset` is the zero-based byte offset, and `value` is the replacement value.
// Outputs: Mutates the archive in place or throws through the stream state checks in the test body.
inline void write_u64_at(const std::filesystem::path& path, std::streamoff offset, std::uint64_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(offset, std::ios::beg);
    for (int i = 0; i < 8; ++i) {
        const char byte = static_cast<char>((value >> (i * 8)) & 0xFF);
        file.write(&byte, 1);
    }
}

// Purpose: Write the native archive footer for handcrafted validation tests.
// Inputs: `file` is positioned after the serialized index and `index_offset`/`index_size` describe that index.
// Outputs: Appends a footer matching the production SUZIP footer layout.
inline void write_test_footer(std::ostream& file, std::uint64_t index_offset, std::uint64_t index_size) {
    superzip::write_u32(file, kTestFooterMagic);
    superzip::write_u32(file, superzip::kSuperZipVersion);
    superzip::write_u64(file, index_offset);
    superzip::write_u64(file, index_size);
}

// Purpose: Read a test archive index through the public index serializer contract.
// Inputs: `path` identifies a native `.suzip` archive produced or handcrafted by a test.
// Outputs: Returns parsed index metadata so tests can assert block-kind contracts.
inline superzip::ArchiveIndex read_test_archive_index(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE_TRUE(file.is_open());
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    REQUIRE_TRUE(end != std::streampos(-1));
    const auto size = static_cast<std::uint64_t>(end);
    REQUIRE_TRUE(size >= 24U);
    file.seekg(static_cast<std::streamoff>(size - 24U), std::ios::beg);
    REQUIRE_EQ(superzip::read_u32(file), kTestFooterMagic);
    REQUIRE_EQ(superzip::read_u32(file), superzip::kSuperZipVersion);
    const auto index_offset = superzip::read_u64(file);
    const auto index_size = superzip::read_u64(file);
    REQUIRE_TRUE(index_offset <= size);
    REQUIRE_TRUE(index_size <= size - index_offset);
    file.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    std::string index_bytes(static_cast<std::size_t>(index_size), '\0');
    file.read(index_bytes.data(), static_cast<std::streamsize>(index_bytes.size()));
    REQUIRE_EQ(static_cast<std::uint64_t>(file.gcount()), index_size);
    std::istringstream index_stream(index_bytes, std::ios::binary);
    return superzip::read_archive_index(index_stream);
}

// Purpose: Detect whether any archive entry contains one block kind.
// Inputs: `index` is parsed `.suzip` metadata and `kind` is the block kind to find.
// Outputs: Returns true when any file entry contains at least one matching block.
inline bool archive_contains_block_kind(const superzip::ArchiveIndex& index, superzip::BlockKind kind) {
    return std::ranges::any_of(index.entries, [kind](const superzip::ArchiveEntry& entry) {
        return std::ranges::any_of(entry.blocks,
                                   [kind](const superzip::BlockDescriptor& block) { return block.kind == kind; });
    });
}

// Purpose: Count files left under an extraction destination after a failed operation.
// Inputs: `root` is the extraction directory that may or may not exist.
// Outputs: Returns the number of regular files visible under `root`.
inline std::uint64_t count_regular_files(const std::filesystem::path& root) {
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

// Purpose: Compare two regular files byte-for-byte for roundtrip tests.
// Inputs: `lhs` and `rhs` are readable file paths.
// Outputs: Returns true when size and full byte content match.
inline bool files_are_equal(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (std::filesystem::file_size(lhs) != std::filesystem::file_size(rhs)) {
        return false;
    }
    std::ifstream left(lhs, std::ios::binary);
    std::ifstream right(rhs, std::ios::binary);
    std::array<char, 8192> left_buffer{};
    std::array<char, 8192> right_buffer{};
    while (left && right) {
        left.read(left_buffer.data(), static_cast<std::streamsize>(left_buffer.size()));
        right.read(right_buffer.data(), static_cast<std::streamsize>(right_buffer.size()));
        if (left.gcount() != right.gcount()) {
            return false;
        }
        if (!std::equal(left_buffer.begin(), left_buffer.begin() + left.gcount(), right_buffer.begin())) {
            return false;
        }
    }
    return left.eof() && right.eof();
}

// Purpose: Flip one byte in an archive file for corruption-detection tests.
// Inputs: `path` identifies the archive, `offset` is a validated byte position, and `mask` selects bits to flip.
// Outputs: Mutates one archive byte in place.
inline void xor_archive_byte(const std::filesystem::path& path, std::uint64_t offset, unsigned char mask) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE_TRUE(file.is_open());
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    char byte = 0;
    file.read(&byte, 1);
    REQUIRE_EQ(file.gcount(), std::streamsize{1});
    byte = static_cast<char>(static_cast<unsigned char>(byte) ^ mask);
    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file.write(&byte, 1);
    REQUIRE_TRUE(file.good());
}

}  // namespace superzip_test
