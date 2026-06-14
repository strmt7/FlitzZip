#include "core/archive_index.hpp"

#include "core/result.hpp"

#include <array>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>

namespace superzip {
namespace {

constexpr std::uint64_t kMinimumEntryMetadataBytes = 35U;
constexpr std::uint64_t kMinimumBlockMetadataBytes = 18U;

// Purpose: Write an unsigned integer in little-endian byte order.
// Inputs: `output` is an open binary stream and `value` is the integral value.
// Outputs: Appends `sizeof(T)` bytes; throws `ArchiveError` on stream failure.
template <typename T>
void write_le(std::ostream& output, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        output.put(static_cast<char>((static_cast<std::uint64_t>(value) >> (i * 8U)) & 0xFFU));
    }
    if (!output) {
        throw ArchiveError("failed to write archive metadata");
    }
}

// Purpose: Read an unsigned integer in little-endian byte order.
// Inputs: `input` is an open binary stream positioned at the integer.
// Outputs: Returns the decoded value; throws `ArchiveError` on truncation.
template <typename T>
T read_le(std::istream& input) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const int ch = input.get();
        if (ch == EOF) {
            throw ArchiveError("archive metadata is truncated");
        }
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) << (i * 8U);
    }
    return static_cast<T>(value);
}

// Purpose: Query the number of bytes left in a seekable metadata stream.
// Inputs: `input` is the archive-index stream after a successful read.
// Outputs: Returns remaining bytes when the stream is seekable; returns empty without changing caller-visible stream state otherwise.
std::optional<std::uint64_t> stream_bytes_remaining(std::istream& input) {
    const auto original_state = input.rdstate();
    const auto current = input.tellg();
    if (current == std::istream::pos_type(-1)) {
        input.clear(original_state);
        return std::nullopt;
    }

    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    input.seekg(current, std::ios::beg);
    if (end == std::istream::pos_type(-1) || !input) {
        input.clear(original_state);
        return std::nullopt;
    }
    input.clear(original_state);
    if (end < current) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(end - current);
}

// Purpose: Reject record counts that cannot fit in the remaining metadata bytes.
// Inputs: `input` is the seekable index stream, `count` is the declared record count, and `minimum_record_bytes` is the smallest valid encoded record.
// Outputs: Returns normally when the count is plausible; throws `ArchiveError` before any large reserve on impossible counts.
void reject_count_exceeding_stream(
    std::istream& input,
    std::uint64_t count,
    std::uint64_t minimum_record_bytes,
    const char* message) {
    const auto remaining = stream_bytes_remaining(input);
    if (remaining.has_value() && count > (*remaining / minimum_record_bytes)) {
        throw ArchiveError(message);
    }
}

// Purpose: Reject archive-index shapes that would exceed bounded parser memory.
// Inputs: `index` is the in-memory metadata tree about to be serialized.
// Outputs: Returns normally for bounded indexes; throws `ArchiveError` before count truncation or oversized metadata emission.
void reject_unbounded_index_shape(const ArchiveIndex& index) {
    if (index.entries.size() > kMaxArchiveEntries) {
        throw ArchiveError("archive entry count exceeds SuperZip resource limit");
    }
    std::uint64_t total_blocks = 0;
    for (const auto& entry : index.entries) {
        if (entry.blocks.size() > kMaxBlocksPerEntry) {
            throw ArchiveError("archive block count exceeds per-entry resource limit");
        }
        if (entry.blocks.size() > kMaxArchiveBlocks - total_blocks) {
            throw ArchiveError("archive block count exceeds total resource limit");
        }
        total_blocks += entry.blocks.size();
    }
}

}  // namespace

void write_u16(std::ostream& output, std::uint16_t value) {
    write_le(output, value);
}

void write_u32(std::ostream& output, std::uint32_t value) {
    write_le(output, value);
}

void write_u64(std::ostream& output, std::uint64_t value) {
    write_le(output, value);
}

std::uint16_t read_u16(std::istream& input) {
    return read_le<std::uint16_t>(input);
}

std::uint32_t read_u32(std::istream& input) {
    return read_le<std::uint32_t>(input);
}

std::uint64_t read_u64(std::istream& input) {
    return read_le<std::uint64_t>(input);
}

void write_archive_index(std::ostream& output, const ArchiveIndex& index) {
    reject_unbounded_index_shape(index);
    write_u32(output, kSuperZipMagic);
    write_u32(output, kSuperZipVersion);
    write_u32(output, static_cast<std::uint32_t>(index.entries.size()));
    for (const auto& entry : index.entries) {
        if (entry.path.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw ArchiveError("archive path is too long: " + entry.path);
        }
        write_u16(output, static_cast<std::uint16_t>(entry.path.size()));
        output.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
        output.put(entry.directory ? '\1' : '\0');
        write_u64(output, entry.uncompressed_size);
        write_u64(output, entry.payload_offset);
        write_u64(output, entry.payload_size);
        write_u32(output, entry.crc32);
        write_u32(output, static_cast<std::uint32_t>(entry.blocks.size()));
        for (const auto& block : entry.blocks) {
            output.put(static_cast<char>(block.kind));
            output.put(static_cast<char>(block.fill_value));
            write_u32(output, block.uncompressed_len);
            write_u64(output, block.encoded_offset);
            write_u32(output, block.encoded_len);
        }
    }
    if (!output) {
        throw ArchiveError("failed to write archive index");
    }
}

ArchiveIndex read_archive_index(std::istream& input) {
    ArchiveIndex index;
    const auto magic = read_u32(input);
    if (magic != kSuperZipMagic) {
        throw ArchiveError("not a SuperZip archive");
    }
    const auto version = read_u32(input);
    if (version != kSuperZipVersion) {
        throw ArchiveError("unsupported SuperZip archive version");
    }
    const auto entry_count = read_u32(input);
    if (entry_count > kMaxArchiveEntries) {
        throw ArchiveError("archive entry count is unreasonable");
    }
    reject_count_exceeding_stream(input, entry_count, kMinimumEntryMetadataBytes, "archive entry count exceeds index size");
    index.entries.reserve(entry_count);
    std::uint64_t total_blocks = 0;
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const auto path_len = read_u16(input);
        std::string path(path_len, '\0');
        input.read(path.data(), path_len);
        if (input.gcount() != path_len) {
            throw ArchiveError("archive path is truncated");
        }
        ArchiveEntry entry;
        entry.path = std::move(path);
        entry.directory = input.get() != 0;
        if (!input) {
            throw ArchiveError("archive entry flags are truncated");
        }
        entry.uncompressed_size = read_u64(input);
        entry.payload_offset = read_u64(input);
        entry.payload_size = read_u64(input);
        entry.crc32 = read_u32(input);
        const auto block_count = read_u32(input);
        if (block_count > kMaxBlocksPerEntry) {
            throw ArchiveError("archive block count is unreasonable");
        }
        if (block_count > kMaxArchiveBlocks - total_blocks) {
            throw ArchiveError("archive total block count is unreasonable");
        }
        total_blocks += block_count;
        reject_count_exceeding_stream(input, block_count, kMinimumBlockMetadataBytes, "archive block count exceeds index size");
        entry.blocks.reserve(block_count);
        for (std::uint32_t j = 0; j < block_count; ++j) {
            const int kind_raw = input.get();
            const int fill_raw = input.get();
            if (kind_raw == EOF || fill_raw == EOF) {
                throw ArchiveError("archive block is truncated");
            }
            auto kind = BlockKind::Raw;
            if (kind_raw == static_cast<int>(BlockKind::Fill)) {
                kind = BlockKind::Fill;
            } else if (kind_raw == static_cast<int>(BlockKind::Deflate)) {
                kind = BlockKind::Deflate;
            } else if (kind_raw == static_cast<int>(BlockKind::Pattern)) {
                kind = BlockKind::Pattern;
            } else if (kind_raw != static_cast<int>(BlockKind::Raw)) {
                throw ArchiveError("archive block has unknown encoding kind");
            }
            entry.blocks.push_back(BlockDescriptor{
                .kind = kind,
                .fill_value = static_cast<std::uint8_t>(fill_raw),
                .uncompressed_len = read_u32(input),
                .encoded_offset = read_u64(input),
                .encoded_len = read_u32(input),
            });
        }
        index.entries.push_back(std::move(entry));
    }
    return index;
}

}  // namespace superzip
