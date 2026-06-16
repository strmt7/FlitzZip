#include "cab/cab_format.hpp"

#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kCabFixedHeaderBytes = 36U;
constexpr std::size_t kCabFolderRecordBytes = 8U;
constexpr std::size_t kCabFileRecordBytes = 16U;
constexpr std::size_t kCabDataBlockHeaderBytes = 8U;
constexpr std::uint16_t kCabFlagPrevCabinet = 0x0001U;
constexpr std::uint16_t kCabFlagNextCabinet = 0x0002U;
constexpr std::uint16_t kCabFlagReservePresent = 0x0004U;
constexpr std::uint16_t kCabFolderContinuedFromPrevious = 0xFFFD;
constexpr std::uint16_t kCabFolderContinuedToNext = 0xFFFE;
constexpr std::uint16_t kCabFolderContinuedPrevAndNext = 0xFFFF;
constexpr std::uint16_t kCabCompressionTypeMask = 0x000FU;
constexpr std::uint16_t kCabCompressionTypeMaxFdiSupported = 0x0003U;
constexpr std::uint32_t kMaxCabFolders = 4096U;
constexpr std::uint32_t kMaxCabDataBlocks = 1'000'000U;
constexpr std::uint32_t kMaxCabNameBytes = 64U * 1024U;
constexpr std::uint64_t kMaxCabTotalFileBytes = kMaxPipelineMemoryBytes;
constexpr std::array<unsigned char, 4U> kCabMagic{'M', 'S', 'C', 'F'};

static_assert(
    std::numeric_limits<std::uint16_t>::max() <= kMaxArchiveEntries,
    "CAB file-count field can exceed the global archive entry limit; add a runtime check.");

// Purpose: Add two CAB byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_cab_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Multiply two CAB byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the product or throws `ArchiveError` before wraparound.
std::uint64_t checked_mul_cab_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (lhs != 0U && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw ArchiveError(message);
    }
    return lhs * rhs;
}

// Purpose: Read exactly one byte range from a CAB stream.
// Inputs: `input` is the CAB stream, `buffer` receives bytes, and `label` names the region.
// Outputs: Fills the buffer or throws when the file is truncated.
void read_exact(std::istream& input, char* buffer, std::size_t size, const char* label) {
    input.read(buffer, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Seek a CAB stream to a bounded absolute byte offset.
// Inputs: `input` is seekable, `offset` is the target byte offset, and `label` names diagnostics.
// Outputs: Positions the stream or throws when the seek cannot be represented/performed.
void seek_cab_offset(std::ifstream& input, std::uint64_t offset, const char* label) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek ") + label);
    }
}

// Purpose: Decode one little-endian 16-bit CAB field.
// Inputs: `bytes` points to at least two bytes.
// Outputs: Returns the decoded unsigned integer.
std::uint16_t read_le16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U));
}

// Purpose: Decode one little-endian 32-bit CAB field.
// Inputs: `bytes` points to at least four bytes.
// Outputs: Returns the decoded unsigned integer.
std::uint32_t read_le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Purpose: Read a NUL-terminated CAB filename with a strict byte limit.
// Inputs: `input` is positioned at the filename and `archive_limit` bounds reads.
// Outputs: Returns the raw filename without the NUL terminator.
std::string read_cab_name(std::ifstream& input, std::uint64_t archive_limit) {
    const auto start_pos = input.tellg();
    if (start_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read CAB filename offset");
    }
    const auto start = static_cast<std::uint64_t>(start_pos);
    std::string name;
    while (true) {
        const auto current_pos = input.tellg();
        if (current_pos == std::istream::pos_type(-1)) {
            throw ArchiveError("failed to read CAB filename position");
        }
        if (static_cast<std::uint64_t>(current_pos) >= archive_limit) {
            throw ArchiveError("CAB filename extends past cabinet boundary");
        }
        if (name.size() >= kMaxCabNameBytes) {
            throw ArchiveError("CAB filename exceeds SuperZip resource limit");
        }
        char ch = '\0';
        read_exact(input, &ch, 1U, "CAB filename");
        if (ch == '\0') {
            if (name.empty()) {
                throw ArchiveError("CAB filename is empty");
            }
            return name;
        }
        name.push_back(ch);
        if (checked_add_cab_bytes(start, name.size(), "CAB filename offset overflow") > archive_limit) {
            throw ArchiveError("CAB filename extends past cabinet boundary");
        }
    }
}

struct CabHeader {
    std::uint32_t cb_cabinet = 0;
    std::uint32_t coff_files = 0;
    std::uint16_t folder_count = 0;
    std::uint16_t file_count = 0;
    std::uint16_t flags = 0;
    std::uint16_t header_reserve_bytes = 0;
    std::uint8_t folder_reserve_bytes = 0;
    std::uint8_t data_reserve_bytes = 0;
};

struct CabFolderRecord {
    std::uint32_t data_offset = 0;
    std::uint16_t data_block_count = 0;
    std::uint16_t compression_type = 0;
};

// Purpose: Read the fixed CAB header and optional reserve-size descriptor.
// Inputs: `input` is positioned at the start and `archive_size` bounds offsets.
// Outputs: Returns decoded header fields and leaves the stream at the first folder record.
CabHeader read_cab_header(std::ifstream& input, std::uint64_t archive_size) {
    std::array<unsigned char, kCabFixedHeaderBytes> header{};
    read_exact(input, reinterpret_cast<char*>(header.data()), header.size(), "CAB header");
    if (!std::equal(kCabMagic.begin(), kCabMagic.end(), header.begin())) {
        throw ArchiveError("malformed CAB header magic");
    }
    CabHeader result;
    result.cb_cabinet = read_le32(header.data() + 8U);
    result.coff_files = read_le32(header.data() + 16U);
    const auto minor = header[24];
    const auto major = header[25];
    result.folder_count = read_le16(header.data() + 26U);
    result.file_count = read_le16(header.data() + 28U);
    result.flags = read_le16(header.data() + 30U);
    if (major != 1U || minor != 3U) {
        throw ArchiveError("unsupported CAB version");
    }
    if (result.cb_cabinet < kCabFixedHeaderBytes || result.cb_cabinet > archive_size) {
        throw ArchiveError("CAB declared size is outside the file bounds");
    }
    if ((result.flags & (kCabFlagPrevCabinet | kCabFlagNextCabinet)) != 0U) {
        throw ArchiveError("spanned CAB sets are not supported");
    }
    if (result.folder_count > kMaxCabFolders) {
        throw ArchiveError("CAB folder count exceeds SuperZip resource limit");
    }
    if (result.file_count > 0U && result.folder_count == 0U) {
        throw ArchiveError("CAB contains files but no folder table");
    }
    if ((result.flags & kCabFlagReservePresent) != 0U) {
        std::array<unsigned char, 4U> reserve{};
        read_exact(input, reinterpret_cast<char*>(reserve.data()), reserve.size(), "CAB reserve sizes");
        result.header_reserve_bytes = read_le16(reserve.data());
        result.folder_reserve_bytes = reserve[2];
        result.data_reserve_bytes = reserve[3];
        if (result.header_reserve_bytes > 0U) {
            const auto after_reserve = checked_add_cab_bytes(
                kCabFixedHeaderBytes + reserve.size(),
                result.header_reserve_bytes,
                "CAB header reserve overflow");
            if (after_reserve > result.cb_cabinet) {
                throw ArchiveError("CAB header reserve extends past cabinet boundary");
            }
            seek_cab_offset(input, after_reserve, "CAB folder table");
        }
    }
    return result;
}

// Purpose: Validate one folder's CFDATA block extents before FDI decompression.
// Inputs: `input` is seekable, `folder` identifies the block table, `header` supplies reserve sizes, and `archive_limit` is the declared CAB size.
// Outputs: Returns normally when every declared data block is bounded by the cabinet.
void validate_cab_data_blocks(
    std::ifstream& input,
    const CabFolderRecord& folder,
    const CabHeader& header,
    std::uint64_t archive_limit) {
    if (folder.data_block_count == 0U) {
        return;
    }

    seek_cab_offset(input, folder.data_offset, "CAB folder data");
    for (std::uint16_t block_index = 0; block_index < folder.data_block_count; ++block_index) {
        const auto block_header_pos = input.tellg();
        if (block_header_pos == std::istream::pos_type(-1)) {
            throw ArchiveError("failed to read CAB data block offset");
        }
        const auto block_header_offset = static_cast<std::uint64_t>(block_header_pos);
        if (checked_add_cab_bytes(block_header_offset, kCabDataBlockHeaderBytes, "CAB data block header overflow") > archive_limit) {
            throw ArchiveError("CAB data block header extends past cabinet boundary");
        }

        std::array<unsigned char, kCabDataBlockHeaderBytes> block{};
        read_exact(input, reinterpret_cast<char*>(block.data()), block.size(), "CAB data block header");
        const auto compressed_size = read_le16(block.data() + 4U);
        const auto data_start_pos = input.tellg();
        if (data_start_pos == std::istream::pos_type(-1)) {
            throw ArchiveError("failed to read CAB data block payload offset");
        }
        const auto data_start = static_cast<std::uint64_t>(data_start_pos);
        const auto payload_start = checked_add_cab_bytes(data_start, header.data_reserve_bytes, "CAB data reserve overflow");
        const auto payload_end = checked_add_cab_bytes(payload_start, compressed_size, "CAB compressed data overflow");
        if (payload_end > archive_limit) {
            throw ArchiveError("CAB compressed data block extends past cabinet boundary");
        }
        seek_cab_offset(input, payload_end, "CAB next data block");
    }
}

// Purpose: Validate the CAB folder table before reading file records.
// Inputs: `input` is positioned at the first folder, `header` is decoded metadata, and `archive_limit` is the declared cabinet size.
// Outputs: Returns normally when folder records are bounded and non-spanning.
void scan_cab_folders(std::ifstream& input, const CabHeader& header, std::uint64_t archive_limit) {
    const auto record_size = checked_add_cab_bytes(kCabFolderRecordBytes, header.folder_reserve_bytes, "CAB folder record overflow");
    const auto table_bytes = checked_mul_cab_bytes(header.folder_count, record_size, "CAB folder table overflow");
    const auto start_pos = input.tellg();
    if (start_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read CAB folder table offset");
    }
    const auto end = checked_add_cab_bytes(static_cast<std::uint64_t>(start_pos), table_bytes, "CAB folder table end overflow");
    if (end > archive_limit) {
        throw ArchiveError("CAB folder table extends past cabinet boundary");
    }

    std::vector<CabFolderRecord> folders;
    folders.reserve(header.folder_count);
    std::uint64_t total_data_blocks = 0;
    for (std::uint16_t index = 0; index < header.folder_count; ++index) {
        std::array<unsigned char, kCabFolderRecordBytes> folder{};
        read_exact(input, reinterpret_cast<char*>(folder.data()), folder.size(), "CAB folder record");
        const auto coff_cab_start = read_le32(folder.data());
        const auto data_block_count = read_le16(folder.data() + 4U);
        const auto compression_type = static_cast<std::uint16_t>(read_le16(folder.data() + 6U) & kCabCompressionTypeMask);
        if (coff_cab_start >= archive_limit) {
            throw ArchiveError("CAB folder data offset is outside the cabinet");
        }
        if (compression_type > kCabCompressionTypeMaxFdiSupported) {
            throw ArchiveError("CAB folder compression type is not supported by Windows FDI");
        }
        if (data_block_count == 0U && header.file_count > 0U) {
            throw ArchiveError("CAB folder has no data blocks for file payloads");
        }
        total_data_blocks = checked_add_cab_bytes(total_data_blocks, data_block_count, "CAB data block count overflow");
        if (total_data_blocks > kMaxCabDataBlocks) {
            throw ArchiveError("CAB data block count exceeds SuperZip resource limit");
        }
        folders.push_back(CabFolderRecord{
            .data_offset = coff_cab_start,
            .data_block_count = data_block_count,
            .compression_type = compression_type,
        });
        if (header.folder_reserve_bytes > 0U) {
            const auto reserve_start = input.tellg();
            if (reserve_start == std::istream::pos_type(-1)) {
                throw ArchiveError("failed to read CAB folder reserve offset");
            }
            seek_cab_offset(
                input,
                checked_add_cab_bytes(
                    static_cast<std::uint64_t>(reserve_start),
                    header.folder_reserve_bytes,
                    "CAB folder reserve end overflow"),
                "CAB folder reserve");
        }
    }

    for (const auto& folder : folders) {
        validate_cab_data_blocks(input, folder, header, archive_limit);
    }
}

// Purpose: Read and validate CAB file records.
// Inputs: `input` is seekable, `header` is decoded metadata, and `archive_limit` is the declared cabinet size.
// Outputs: Returns validated entry metadata and total uncompressed bytes.
CabMetadata scan_cab_files(std::ifstream& input, const CabHeader& header, std::uint64_t archive_limit) {
    if (header.coff_files < kCabFixedHeaderBytes || header.coff_files >= archive_limit) {
        throw ArchiveError("CAB file table offset is outside the cabinet");
    }
    seek_cab_offset(input, header.coff_files, "CAB file table");

    CabMetadata result;
    std::vector<ArchivePathValidationEntry> validation_entries;
    for (std::uint16_t index = 0; index < header.file_count; ++index) {
        std::array<unsigned char, kCabFileRecordBytes> file{};
        read_exact(input, reinterpret_cast<char*>(file.data()), file.size(), "CAB file record");
        const auto file_size = read_le32(file.data());
        const auto folder_offset = read_le32(file.data() + 4U);
        const auto folder_index = read_le16(file.data() + 8U);
        if (folder_index == kCabFolderContinuedFromPrevious ||
            folder_index == kCabFolderContinuedToNext ||
            folder_index == kCabFolderContinuedPrevAndNext) {
            throw ArchiveError("CAB file spans cabinet boundaries");
        }
        if (folder_index >= header.folder_count) {
            throw ArchiveError("CAB file references an invalid folder index");
        }
        if (folder_offset > std::numeric_limits<std::uint32_t>::max() - file_size) {
            throw ArchiveError("CAB file folder offset overflows");
        }

        const auto normalized = normalize_cab_entry_path(read_cab_name(input, archive_limit));
        validation_entries.push_back(ArchivePathValidationEntry{
            .path = normalized,
            .directory = false,
        });
        result.entries.push_back(CabEntryInfo{
            .path = normalized,
            .size = file_size,
        });
        result.total_file_bytes = checked_add_cab_bytes(
            result.total_file_bytes,
            file_size,
            "CAB uncompressed payload byte count overflow");
        if (result.total_file_bytes > kMaxCabTotalFileBytes) {
            throw ArchiveError("CAB uncompressed payload exceeds SuperZip resource limit");
        }
    }
    validate_archive_path_set(validation_entries);
    return result;
}

}  // namespace

std::string normalize_cab_entry_path(std::string raw_path) {
    std::ranges::replace(raw_path, '\\', '/');
    return normalize_archive_path_key(raw_path);
}

CabMetadata scan_cab_metadata(const std::filesystem::path& archive_path) {
    const auto archive_size = std::filesystem::file_size(archive_path);
    if (archive_size < kCabFixedHeaderBytes) {
        throw ArchiveError("CAB file is too small");
    }
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open CAB archive: " + archive_path.string());
    }
    const auto header = read_cab_header(input, archive_size);
    const auto archive_limit = static_cast<std::uint64_t>(header.cb_cabinet);
    scan_cab_folders(input, header, archive_limit);
    return scan_cab_files(input, header, archive_limit);
}

}  // namespace superzip
