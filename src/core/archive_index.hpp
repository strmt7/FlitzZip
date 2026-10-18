#pragma once

#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace flitzzip {

// Wire signatures are stable across the product/extension rename; existing archives need no conversion.
constexpr std::uint32_t kFlitzZipMagic = 0x505A5553;        // SUZP
constexpr std::uint32_t kFlitzZipFooterMagic = 0x465A5553;  // SUZF
constexpr std::uint64_t kFlitzZipFooterBytes = 2U * sizeof(std::uint32_t) + 2U * sizeof(std::uint64_t);
constexpr std::uint32_t kFlitzZipMinReadableVersion = 1;
constexpr std::uint32_t kFlitzZipVersion = 3;
constexpr std::uint32_t kFlitzZipMaxReadableVersion = 11;

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
    std::uint32_t version = kFlitzZipVersion;
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
};

// Purpose: Serialize only the native index records, leaving footer framing to the archive writer.
// Inputs: Caller-owned output positioned at the index start and bounded entry/block metadata.
// Outputs: Appends the index; rejects unsupported versions, block kinds, pattern bounds and failed writes.
void write_archive_index(std::ostream& output, const ArchiveIndex& index);

// Purpose: Parse bounded native index records from their exact starting position.
// Inputs: Caller-owned input positioned at the index magic; seeking is optional.
// Outputs: Parsed records or ArchiveError on malformed metadata. The archive reader separately validates the footer,
// exact index consumption, payload extents, entry aggregates and paths before decoding or publication.
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

}  // namespace flitzzip
