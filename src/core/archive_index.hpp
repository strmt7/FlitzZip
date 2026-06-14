#pragma once

#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace superzip {

constexpr std::uint32_t kSuperZipMagic = 0x505A5553;  // SUZP
constexpr std::uint32_t kSuperZipVersion = 1;

struct ArchiveEntry {
    std::string path;
    bool directory = false;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_size = 0;
    std::uint32_t crc32 = 0;
    std::vector<BlockDescriptor> blocks;
};

struct ArchiveIndex {
    std::vector<ArchiveEntry> entries;
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
};

// Purpose: Serialize the archive index and footer to an output stream.
// Inputs: `output` must be an open binary stream positioned after payload data; `index` contains validated entry metadata and block descriptors.
// Outputs: Writes bytes to `output`; throws `ArchiveError` when stream writes fail.
void write_archive_index(std::ostream& output, const ArchiveIndex& index);

// Purpose: Read and validate the SuperZip archive index and footer from an input stream.
// Inputs: `input` must be an open binary stream positioned anywhere in the archive.
// Outputs: Returns a validated `ArchiveIndex`; throws `ArchiveError` on bad magic, version, truncation, or inconsistent metadata.
ArchiveIndex read_archive_index(std::istream& input);

// Purpose: Write a little-endian unsigned 16-bit integer to a binary stream.
// Inputs: `output` is the destination stream and `value` is the integer to encode.
// Outputs: Appends two bytes; throws `ArchiveError` on stream failure.
void write_u16(std::ostream& output, std::uint16_t value);

// Purpose: Write a little-endian unsigned 32-bit integer to a binary stream.
// Inputs: `output` is the destination stream and `value` is the integer to encode.
// Outputs: Appends four bytes; throws `ArchiveError` on stream failure.
void write_u32(std::ostream& output, std::uint32_t value);

// Purpose: Write a little-endian unsigned 64-bit integer to a binary stream.
// Inputs: `output` is the destination stream and `value` is the integer to encode.
// Outputs: Appends eight bytes; throws `ArchiveError` on stream failure.
void write_u64(std::ostream& output, std::uint64_t value);

// Purpose: Read a little-endian unsigned 16-bit integer from a binary stream.
// Inputs: `input` is the source stream positioned at the encoded integer.
// Outputs: Returns the decoded value; throws `ArchiveError` on truncation or stream failure.
std::uint16_t read_u16(std::istream& input);

// Purpose: Read a little-endian unsigned 32-bit integer from a binary stream.
// Inputs: `input` is the source stream positioned at the encoded integer.
// Outputs: Returns the decoded value; throws `ArchiveError` on truncation or stream failure.
std::uint32_t read_u32(std::istream& input);

// Purpose: Read a little-endian unsigned 64-bit integer from a binary stream.
// Inputs: `input` is the source stream positioned at the encoded integer.
// Outputs: Returns the decoded value; throws `ArchiveError` on truncation or stream failure.
std::uint64_t read_u64(std::istream& input);

}  // namespace superzip
