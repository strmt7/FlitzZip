#include "arc/arc_adapter.hpp"

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

constexpr unsigned char kArcHeaderMarker = 0x1AU;
constexpr std::uint8_t kArcMethodEnd = 0U;
constexpr std::uint8_t kArcMethodUnpackedOld = 1U;
constexpr std::uint8_t kArcMethodUnpackedNew = 2U;
constexpr std::size_t kArcNameBytes = 13U;
constexpr std::size_t kArcOldHeaderBytes = 25U;
constexpr std::size_t kArcNewHeaderBytes = 29U;
constexpr std::size_t kArcIoBufferBytes = 64U * 1024U;

struct ArcHeader {
    bool end_marker = false;
    std::uint8_t method = 0;
    std::uint32_t compressed_size = 0;
    std::uint32_t original_size = 0;
    std::uint16_t crc = 0;
    std::string name;
};

struct ArcEntryMetadata {
    std::string path;
    std::uint64_t payload_offset = 0;
    std::uint32_t size = 0;
    std::uint16_t crc = 0;
};

struct ArcScanResult {
    std::vector<ArcEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Decode a little-endian 16-bit ARC field.
// Inputs: `bytes` points to at least two bounded metadata bytes.
// Outputs: Returns the decoded unsigned integer.
std::uint16_t read_le16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U));
}

// Purpose: Decode a little-endian 32-bit ARC field.
// Inputs: `bytes` points to at least four bounded metadata bytes.
// Outputs: Returns the decoded unsigned integer.
std::uint32_t read_le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Purpose: Add two ARC byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels diagnostics.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_arc_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Read an exact byte count from an ARC stream.
// Inputs: `input` is the archive stream, `buffer` receives bytes, and `label` names the region.
// Outputs: Fills the buffer or throws when the archive is truncated.
void read_exact(std::istream& input, char* buffer, std::size_t size, const char* label) {
    input.read(buffer, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Seek to an absolute ARC archive offset after range validation.
// Inputs: `input` is seekable, `offset` is the absolute byte offset, and `label` names diagnostics.
// Outputs: Repositions the stream or throws on an unrepresentable or failed seek.
void seek_arc_offset(std::ifstream& input, std::uint64_t offset, const char* label) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek ") + label);
    }
}

// Purpose: Validate that a declared ARC payload range is inside the archive file.
// Inputs: `payload_offset` is the absolute payload start, `payload_size` is the declared compressed size, and `archive_size` is the file length.
// Outputs: Throws when the declared payload range is outside the archive.
void validate_payload_extent(std::uint64_t payload_offset, std::uint64_t payload_size, std::uint64_t archive_size) {
    const auto payload_end = checked_add_arc_bytes(payload_offset, payload_size, "ARC payload extent overflows");
    if (payload_end > archive_size) {
        throw ArchiveError("ARC payload extends past the end of the archive");
    }
}

// Purpose: Compute CRC-16/ARC over untrusted payload bytes.
// Inputs: `bytes` is a bounded payload chunk and `seed` is the prior finalized CRC state.
// Outputs: Returns the finalized CRC-16/ARC value.
std::uint16_t crc16_arc(std::span<const unsigned char> bytes, std::uint16_t seed = 0) {
    auto crc = seed;
    for (const auto byte : bytes) {
        crc = static_cast<std::uint16_t>(crc ^ byte);
        for (int bit = 0; bit < 8; ++bit) {
            const bool carry = (crc & 1U) != 0U;
            crc = static_cast<std::uint16_t>(crc >> 1U);
            if (carry) {
                crc = static_cast<std::uint16_t>(crc ^ 0xA001U);
            }
        }
    }
    return crc;
}

// Purpose: Decode the fixed-width ARC filename field.
// Inputs: `name_bytes` is the 13-byte ARC filename region from untrusted metadata.
// Outputs: Returns a normalized archive path key or throws for missing or unsafe names.
std::string read_arc_name(std::span<const unsigned char, kArcNameBytes> name_bytes) {
    std::size_t length = 0;
    while (length < name_bytes.size() && name_bytes[length] != 0U) {
        ++length;
    }
    if (length == 0U) {
        throw ArchiveError("ARC entry name is empty");
    }
    std::string raw_name(reinterpret_cast<const char*>(name_bytes.data()), length);
    std::ranges::replace(raw_name, '\\', '/');
    return normalize_archive_path_key(raw_name);
}

// Purpose: Parse one ARC file header.
// Inputs: `input` is positioned at an ARC header marker and `archive_size` bounds metadata reads.
// Outputs: Returns decoded header metadata or an end marker; leaves the stream positioned at payload data.
ArcHeader read_arc_header(std::ifstream& input, std::uint64_t archive_size) {
    const auto header_start_pos = input.tellg();
    if (header_start_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read ARC header offset");
    }
    const auto header_start = static_cast<std::uint64_t>(header_start_pos);
    if (header_start == archive_size) {
        throw ArchiveError("ARC archive is missing the end marker");
    }
    if (checked_add_arc_bytes(header_start, 2U, "ARC header prefix overflows") > archive_size) {
        throw ArchiveError("ARC header prefix extends past the end of the archive");
    }

    std::array<unsigned char, 2U> prefix{};
    read_exact(input, reinterpret_cast<char*>(prefix.data()), prefix.size(), "ARC header prefix");
    if (prefix[0] != kArcHeaderMarker) {
        throw ArchiveError("malformed ARC header marker");
    }
    const auto method = prefix[1];
    if (method == kArcMethodEnd) {
        return ArcHeader{.end_marker = true};
    }

    if (method == kArcMethodUnpackedOld) {
        if (checked_add_arc_bytes(header_start, kArcOldHeaderBytes, "ARC old header overflows") > archive_size) {
            throw ArchiveError("ARC old header extends past the end of the archive");
        }
    } else {
        if (checked_add_arc_bytes(header_start, kArcNewHeaderBytes, "ARC header overflows") > archive_size) {
            throw ArchiveError("ARC header extends past the end of the archive");
        }
    }

    std::array<unsigned char, kArcNewHeaderBytes - 2U> rest{};
    const auto rest_size = method == kArcMethodUnpackedOld ? kArcOldHeaderBytes - 2U : kArcNewHeaderBytes - 2U;
    read_exact(input, reinterpret_cast<char*>(rest.data()), rest_size, "ARC entry header");
    const auto compressed_size = read_le32(rest.data() + 13U);
    const auto original_size = method == kArcMethodUnpackedOld ? compressed_size : read_le32(rest.data() + 23U);

    std::array<unsigned char, kArcNameBytes> name_bytes{};
    std::copy_n(rest.begin(), name_bytes.size(), name_bytes.begin());
    return ArcHeader{
        .method = method,
        .compressed_size = compressed_size,
        .original_size = original_size,
        .crc = read_le16(rest.data() + 21U),
        .name = read_arc_name(std::span<const unsigned char, kArcNameBytes>(name_bytes)),
    };
}

// Purpose: Validate an unpacked ARC payload before destination writes.
// Inputs: `input` is positioned at payload data, `header` supplies size/CRC metadata, and `archive_size` bounds reads.
// Outputs: Leaves `input` after the payload and throws if the data does not match metadata.
void validate_unpacked_payload(std::ifstream& input, const ArcHeader& header, std::uint64_t archive_size) {
    const auto payload_pos = input.tellg();
    if (payload_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read ARC payload offset");
    }
    validate_payload_extent(static_cast<std::uint64_t>(payload_pos), header.compressed_size, archive_size);
    if (header.compressed_size != header.original_size) {
        throw ArchiveError("unpacked ARC entry has mismatched compressed and original sizes");
    }

    std::array<unsigned char, kArcIoBufferBytes> buffer{};
    std::uint64_t remaining = header.compressed_size;
    std::uint16_t crc = 0;
    while (remaining > 0U) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        read_exact(input, reinterpret_cast<char*>(buffer.data()), chunk, "ARC unpacked payload");
        crc = crc16_arc(std::span<const unsigned char>(buffer.data(), chunk), crc);
        remaining -= chunk;
    }
    if (crc != header.crc) {
        throw ArchiveError("ARC unpacked payload CRC mismatch");
    }
}

// Purpose: Scan a full SEA ARC archive and validate metadata plus unpacked payload CRCs before extraction.
// Inputs: `archive_path` is the ARC/ARK file to parse.
// Outputs: Returns trusted extraction metadata; throws on malformed headers, unsafe paths, unsupported methods, duplicates, or corrupt stored data.
ArcScanResult scan_arc(const std::filesystem::path& archive_path) {
    const auto archive_size = std::filesystem::file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open ARC archive: " + archive_path.string());
    }

    ArcScanResult result;
    std::vector<ArchivePathValidationEntry> validation_entries;
    while (true) {
        if (result.entries.size() >= kMaxArchiveEntries) {
            throw ArchiveError("ARC archive contains too many entries");
        }
        const auto header = read_arc_header(input, archive_size);
        if (header.end_marker) {
            break;
        }
        if (header.method != kArcMethodUnpackedOld && header.method != kArcMethodUnpackedNew) {
            throw ArchiveError("compressed ARC methods are not supported by this SuperZip build");
        }
        const auto payload_pos = input.tellg();
        if (payload_pos == std::istream::pos_type(-1)) {
            throw ArchiveError("failed to read ARC payload offset");
        }
        validate_unpacked_payload(input, header, archive_size);
        result.total_file_bytes = checked_add_arc_bytes(
            result.total_file_bytes,
            header.original_size,
            "ARC extracted byte count overflows");
        if (result.total_file_bytes > kMaxPipelineMemoryBytes) {
            throw ArchiveError("ARC extracted payload exceeds SuperZip resource limits");
        }
        validation_entries.push_back(ArchivePathValidationEntry{.path = header.name, .directory = false});
        result.entries.push_back(ArcEntryMetadata{
            .path = header.name,
            .payload_offset = static_cast<std::uint64_t>(payload_pos),
            .size = header.original_size,
            .crc = header.crc,
        });
    }

    const auto end_pos = input.tellg();
    if (end_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read ARC end marker offset");
    }
    if (static_cast<std::uint64_t>(end_pos) != archive_size) {
        throw ArchiveError("ARC archive contains trailing data after the end marker");
    }
    validate_archive_path_set(validation_entries);
    return result;
}

// Purpose: Publish one validated unpacked ARC file payload.
// Inputs: `input` is an archive stream, `entry` is trusted scan metadata, `target` is the destination file, and `overwrite` controls replacement.
// Outputs: Writes and atomically publishes the file, or throws without leaving a final partial output.
void extract_arc_file_payload(
    std::ifstream& input,
    const ArcEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    seek_arc_offset(input, entry.payload_offset, "ARC payload");
    std::filesystem::create_directories(target.parent_path());
    auto temporary_target = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary extracted file: " + target.string());
        }
        std::array<unsigned char, kArcIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        std::uint16_t crc = 0;
        while (remaining > 0U) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            read_exact(input, reinterpret_cast<char*>(buffer.data()), chunk, "ARC file payload");
            output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
            if (!output) {
                throw ArchiveError("failed to write temporary extracted file: " + target.string());
            }
            crc = crc16_arc(std::span<const unsigned char>(buffer.data(), chunk), crc);
            remaining -= chunk;
        }
        if (crc != entry.crc) {
            throw ArchiveError("ARC file payload CRC mismatch during publish");
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

OperationStats extract_arc(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto scanned = scan_arc(archive_path);
    std::filesystem::create_directories(destination);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open ARC archive: " + archive_path.string());
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());
    for (const auto& entry : scanned.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        extract_arc_file_payload(input, entry, target, overwrite);
        progress.add_bytes(entry.size);
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
