#include "arj/arj_adapter.hpp"

#include "core/checksum.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace superzip {
namespace {

constexpr std::array<unsigned char, 2U> kArjHeaderId{0x60U, 0xEAU};
constexpr std::uint16_t kMaxArjBasicHeaderBytes = 2600U;
constexpr std::uint64_t kMaxArjExtendedHeaderBytes = 1024U * 1024U;
constexpr std::size_t kArjBaseHeaderBytes = 30U;
constexpr std::size_t kArjIoBufferBytes = 64U * 1024U;
constexpr std::uint8_t kArjFlagGarbled = 0x01U;
constexpr std::uint8_t kArjFlagVolume = 0x04U;
constexpr std::uint8_t kArjFlagExtendedFilePosition = 0x08U;
constexpr std::uint8_t kArjMethodStored = 0U;
constexpr std::uint8_t kArjFileBinary = 0U;
constexpr std::uint8_t kArjFileText = 1U;
constexpr std::uint8_t kArjFileComment = 2U;
constexpr std::uint8_t kArjFileDirectory = 3U;

struct ArjHeader {
    bool end_marker = false;
    std::uint8_t flags = 0;
    std::uint8_t method = 0;
    std::uint8_t file_type = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t original_size = 0;
    std::uint32_t data_crc = 0;
    std::string name;
};

struct ArjEntryMetadata {
    std::string path;
    std::uint64_t payload_offset = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    bool directory = false;
};

struct ArjScanResult {
    std::vector<ArjEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Decode a little-endian 16-bit ARJ field.
// Inputs: `bytes` points to at least two bytes of trusted bounded metadata.
// Outputs: Returns the decoded unsigned integer.
std::uint16_t read_le16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U));
}

// Purpose: Decode a little-endian 32-bit ARJ field.
// Inputs: `bytes` points to at least four bytes of trusted bounded metadata.
// Outputs: Returns the decoded unsigned integer.
std::uint32_t read_le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Purpose: Compute the SuperZip CRC-32 over unsigned ARJ bytes.
// Inputs: `bytes` is a bounded byte span and `seed` is the prior finalized CRC state.
// Outputs: Returns the finalized CRC-32 value used by ARJ metadata and payload checks.
std::uint32_t crc32_unsigned(std::span<const unsigned char> bytes, std::uint32_t seed = 0) {
    return crc32(std::as_bytes(bytes), seed);
}

// Purpose: Add two ARJ byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels diagnostics.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_arj_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Read an exact byte count from an ARJ stream.
// Inputs: `input` is the archive stream, `buffer` receives bytes, and `label` names the region.
// Outputs: Fills the buffer or throws when the archive is truncated.
void read_exact(std::istream& input, char* buffer, std::size_t size, const char* label) {
    input.read(buffer, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Seek to an absolute ARJ archive offset after range validation.
// Inputs: `input` is seekable, `offset` is the absolute byte offset, and `label` names diagnostics.
// Outputs: Repositions the stream or throws on an unrepresentable or failed seek.
void seek_arj_offset(std::ifstream& input, std::uint64_t offset, const char* label) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek ") + label);
    }
}

// Purpose: Validate that a declared ARJ payload range is inside the archive file.
// Inputs: `payload_offset` is the absolute payload start, `payload_size` is the declared compressed size, and `archive_size` is the file length.
// Outputs: Throws when the declared payload range is outside the archive.
void validate_payload_extent(std::uint64_t payload_offset, std::uint64_t payload_size, std::uint64_t archive_size) {
    const auto payload_end = checked_add_arj_bytes(payload_offset, payload_size, "ARJ payload extent overflows");
    if (payload_end > archive_size) {
        throw ArchiveError("ARJ payload extends past the end of the archive");
    }
}

// Purpose: Extract the first NUL-terminated string from an ARJ header suffix.
// Inputs: `header` is the basic header and `offset` is the first filename byte.
// Outputs: Returns the filename and advances `offset` after the NUL terminator; throws on malformed data.
std::string read_header_string(const std::vector<unsigned char>& header, std::size_t& offset) {
    if (offset >= header.size()) {
        throw ArchiveError("ARJ header string is missing");
    }
    const auto begin = offset;
    while (offset < header.size() && header[offset] != 0U) {
        ++offset;
    }
    if (offset >= header.size()) {
        throw ArchiveError("ARJ header string is not NUL terminated");
    }
    std::string value(
        reinterpret_cast<const char*>(header.data() + begin),
        offset - begin);
    ++offset;
    return value;
}

// Purpose: Normalize an ARJ member path before archive-wide path validation.
// Inputs: `raw_path` is untrusted ARJ filename metadata and may contain backslashes.
// Outputs: Returns a slash-separated safe relative path key or throws `SecurityError`.
std::string normalize_arj_entry_path(std::string raw_path) {
    std::ranges::replace(raw_path, '\\', '/');
    return normalize_archive_path_key(raw_path);
}

// Purpose: Read and validate all ARJ extended headers following one basic header.
// Inputs: `input` is positioned at the first extended-header size field.
// Outputs: Leaves the stream after the terminating zero-size extended header; throws on malformed CRC or oversized metadata.
void skip_arj_extended_headers(std::ifstream& input) {
    std::uint64_t total_extended_bytes = 0;
    while (true) {
        std::array<unsigned char, 2U> size_bytes{};
        read_exact(input, reinterpret_cast<char*>(size_bytes.data()), size_bytes.size(), "ARJ extended header size");
        const auto size = read_le16(size_bytes.data());
        if (size == 0U) {
            return;
        }
        total_extended_bytes = checked_add_arj_bytes(total_extended_bytes, size, "ARJ extended header byte count overflows");
        if (total_extended_bytes > kMaxArjExtendedHeaderBytes) {
            throw ArchiveError("ARJ extended headers exceed SuperZip resource limits");
        }
        std::vector<unsigned char> bytes(size);
        read_exact(input, reinterpret_cast<char*>(bytes.data()), bytes.size(), "ARJ extended header");
        std::array<unsigned char, 4U> crc_bytes{};
        read_exact(input, reinterpret_cast<char*>(crc_bytes.data()), crc_bytes.size(), "ARJ extended header CRC");
        if (read_le32(crc_bytes.data()) != crc32_unsigned(bytes)) {
            throw ArchiveError("ARJ extended header CRC mismatch");
        }
    }
}

// Purpose: Parse one ARJ basic header and verify its header CRC.
// Inputs: `input` is positioned at an ARJ header id and `archive_size` bounds metadata reads.
// Outputs: Returns decoded header metadata or an end marker; leaves the stream after extended-header metadata.
ArjHeader read_arj_header(std::ifstream& input, std::uint64_t archive_size) {
    const auto header_start_pos = input.tellg();
    if (header_start_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read ARJ header offset");
    }
    const auto header_start = static_cast<std::uint64_t>(header_start_pos);
    if (checked_add_arj_bytes(header_start, 4U, "ARJ header offset overflow") > archive_size) {
        throw ArchiveError("ARJ header extends past the end of the archive");
    }

    std::array<unsigned char, 4U> prefix{};
    read_exact(input, reinterpret_cast<char*>(prefix.data()), prefix.size(), "ARJ header prefix");
    if (!std::equal(kArjHeaderId.begin(), kArjHeaderId.end(), prefix.begin())) {
        throw ArchiveError("malformed ARJ header id");
    }
    const auto header_size = read_le16(prefix.data() + 2U);
    if (header_size == 0U) {
        return ArjHeader{.end_marker = true};
    }
    if (header_size > kMaxArjBasicHeaderBytes) {
        throw ArchiveError("ARJ basic header exceeds format limit");
    }
    const auto header_crc_end = checked_add_arj_bytes(
        checked_add_arj_bytes(header_start, 4U, "ARJ header data start overflows"),
        checked_add_arj_bytes(header_size, 4U, "ARJ header CRC extent overflows"),
        "ARJ header data extent overflows");
    if (header_crc_end > archive_size) {
        throw ArchiveError("ARJ header extends past the end of the archive");
    }

    std::vector<unsigned char> header(header_size);
    read_exact(input, reinterpret_cast<char*>(header.data()), header.size(), "ARJ basic header");
    std::array<unsigned char, 4U> crc_bytes{};
    read_exact(input, reinterpret_cast<char*>(crc_bytes.data()), crc_bytes.size(), "ARJ basic header CRC");
    if (read_le32(crc_bytes.data()) != crc32_unsigned(header)) {
        throw ArchiveError("ARJ basic header CRC mismatch");
    }
    skip_arj_extended_headers(input);
    if (header.size() < kArjBaseHeaderBytes ||
        header[0] < kArjBaseHeaderBytes ||
        header[0] > header.size()) {
        throw ArchiveError("ARJ basic header is malformed");
    }

    std::size_t string_offset = header[0];
    auto name = read_header_string(header, string_offset);
    (void)read_header_string(header, string_offset);
    return ArjHeader{
        .flags = header[4],
        .method = header[5],
        .file_type = header[6],
        .compressed_size = read_le32(header.data() + 12U),
        .original_size = read_le32(header.data() + 16U),
        .data_crc = read_le32(header.data() + 20U),
        .name = std::move(name),
    };
}

// Purpose: Validate ARJ flags that would require unsupported security or multipart policy.
// Inputs: `header` is decoded untrusted ARJ metadata.
// Outputs: Returns normally for supported flags; throws on encrypted, split, or extended-position entries.
void reject_unsupported_arj_flags(const ArjHeader& header) {
    if ((header.flags & kArjFlagGarbled) != 0U) {
        throw ArchiveError("encrypted ARJ entries are not supported");
    }
    if ((header.flags & kArjFlagVolume) != 0U) {
        throw ArchiveError("multi-volume ARJ entries are not supported");
    }
    if ((header.flags & kArjFlagExtendedFilePosition) != 0U) {
        throw ArchiveError("split ARJ file-position metadata is not supported");
    }
}

// Purpose: Validate a stored ARJ payload before destination writes.
// Inputs: `input` is positioned at the payload, `header` supplies size/CRC metadata, and `archive_size` bounds reads.
// Outputs: Leaves `input` after the payload and throws if the stored data does not match metadata.
void validate_stored_payload(std::ifstream& input, const ArjHeader& header, std::uint64_t archive_size) {
    const auto payload_pos = input.tellg();
    if (payload_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read ARJ payload offset");
    }
    validate_payload_extent(static_cast<std::uint64_t>(payload_pos), header.compressed_size, archive_size);
    if (header.compressed_size != header.original_size) {
        throw ArchiveError("stored ARJ entry has mismatched compressed and original sizes");
    }

    std::array<unsigned char, kArjIoBufferBytes> buffer{};
    std::uint64_t remaining = header.compressed_size;
    std::uint32_t crc = 0;
    while (remaining > 0U) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        read_exact(input, reinterpret_cast<char*>(buffer.data()), chunk, "ARJ stored payload");
        crc = crc32_unsigned(std::span<const unsigned char>(buffer.data(), chunk), crc);
        remaining -= chunk;
    }
    if (crc != header.data_crc) {
        throw ArchiveError("ARJ stored payload CRC mismatch");
    }
}

// Purpose: Convert a decoded ARJ local header into trusted extraction metadata.
// Inputs: `input` is positioned at payload data, `header` is decoded untrusted metadata, `archive_size` bounds payload extents, and `validation_entries` receives path-validation records.
// Outputs: Returns metadata for a supported file or directory entry; throws on unsupported types, methods, paths, or payload CRC mismatch.
ArjEntryMetadata scan_arj_entry(
    std::ifstream& input,
    const ArjHeader& header,
    std::uint64_t archive_size,
    std::vector<ArchivePathValidationEntry>& validation_entries) {
    reject_unsupported_arj_flags(header);
    if (header.file_type != kArjFileBinary &&
        header.file_type != kArjFileText &&
        header.file_type != kArjFileDirectory) {
        throw ArchiveError("unsupported ARJ entry type");
    }
    const auto normalized = normalize_arj_entry_path(header.name);
    const bool directory = header.file_type == kArjFileDirectory;
    validation_entries.push_back(ArchivePathValidationEntry{.path = normalized, .directory = directory});

    const auto payload_pos = input.tellg();
    if (payload_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read ARJ payload offset");
    }
    if (directory) {
        if (header.compressed_size != 0U || header.original_size != 0U) {
            throw ArchiveError("ARJ directory entry unexpectedly contains payload bytes");
        }
    } else {
        if (header.method != kArjMethodStored) {
            throw ArchiveError("compressed ARJ methods are not supported by this SuperZip build");
        }
        validate_stored_payload(input, header, archive_size);
    }
    return ArjEntryMetadata{
        .path = normalized,
        .payload_offset = static_cast<std::uint64_t>(payload_pos),
        .size = header.original_size,
        .crc = header.data_crc,
        .directory = directory,
    };
}

// Purpose: Scan a full ARJ archive and validate metadata plus stored payload CRCs before extraction.
// Inputs: `archive_path` is the ARJ file to parse.
// Outputs: Returns trusted extraction metadata; throws on malformed headers, unsafe paths, unsupported methods, duplicates, or corrupt stored data.
ArjScanResult scan_arj(const std::filesystem::path& archive_path) {
    const auto archive_size = std::filesystem::file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open ARJ archive: " + archive_path.string());
    }
    const auto main_header = read_arj_header(input, archive_size);
    if (main_header.end_marker || main_header.file_type != kArjFileComment) {
        throw ArchiveError("ARJ archive is missing the main header");
    }
    if ((main_header.flags & kArjFlagVolume) != 0U) {
        throw ArchiveError("multi-volume ARJ archives are not supported");
    }

    ArjScanResult result;
    std::vector<ArchivePathValidationEntry> validation_entries;
    while (true) {
        if (result.entries.size() >= kMaxArchiveEntries) {
            throw ArchiveError("ARJ archive contains too many entries");
        }
        const auto header = read_arj_header(input, archive_size);
        if (header.end_marker) {
            break;
        }
        const auto entry = scan_arj_entry(input, header, archive_size, validation_entries);
        result.total_file_bytes = checked_add_arj_bytes(
            result.total_file_bytes,
            entry.directory ? 0U : entry.size,
            "ARJ extracted byte count overflows");
        if (result.total_file_bytes > kMaxPipelineMemoryBytes) {
            throw ArchiveError("ARJ extracted payload exceeds SuperZip resource limits");
        }
        result.entries.push_back(entry);
    }
    validate_archive_path_set(validation_entries);
    return result;
}

// Purpose: Publish one validated stored ARJ file payload.
// Inputs: `input` is an archive stream, `entry` is trusted scan metadata, `target` is the destination file, and `overwrite` controls replacement.
// Outputs: Writes and atomically publishes the file, or throws without leaving a final partial output.
void extract_arj_file_payload(
    std::ifstream& input,
    const ArjEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    seek_arj_offset(input, entry.payload_offset, "ARJ payload");
    std::filesystem::create_directories(target.parent_path());
    auto temporary_target = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary extracted file: " + target.string());
        }
        std::array<unsigned char, kArjIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        std::uint32_t crc = 0;
        while (remaining > 0U) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            read_exact(input, reinterpret_cast<char*>(buffer.data()), chunk, "ARJ file payload");
            output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
            if (!output) {
                throw ArchiveError("failed to write temporary extracted file: " + target.string());
            }
            crc = crc32_unsigned(std::span<const unsigned char>(buffer.data(), chunk), crc);
            remaining -= chunk;
        }
        if (crc != entry.crc) {
            throw ArchiveError("ARJ file payload CRC mismatch during publish");
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize temporary extracted file: " + target.string());
        }
        commit_verified_file(temporary_target.file, target, overwrite);
        cleanup_file_publish_target(temporary_target);
    } catch (...) {
        cleanup_file_publish_target(temporary_target);
        throw;
    }
}

}  // namespace

OperationStats extract_arj(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto scanned = scan_arj(archive_path);
    std::filesystem::create_directories(destination);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open ARJ archive: " + archive_path.string());
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());
    for (const auto& entry : scanned.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        if (entry.directory) {
            std::filesystem::create_directories(target);
        } else {
            extract_arj_file_payload(input, entry, target, overwrite);
            progress.add_bytes(entry.size);
        }
        progress.finish_entry();
    }

    OperationStats stats;
    stats.input_bytes = std::filesystem::file_size(archive_path);
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(scanned.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
