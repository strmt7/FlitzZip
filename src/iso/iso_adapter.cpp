#include "iso/iso_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kIsoSectorBytes = 2048U;
constexpr std::uint64_t kIsoPrimaryVolumeDescriptorSector = 16U;
constexpr std::uint64_t kIsoMaxVolumeDescriptors = 64U;
constexpr std::uint64_t kIsoDirectoryRecordRootOffset = 156U;
constexpr std::uint8_t kIsoVolumeDescriptorPrimary = 1U;
constexpr std::uint8_t kIsoVolumeDescriptorTerminator = 255U;
constexpr std::uint8_t kIsoDirectoryFlag = 0x02U;
constexpr std::uint8_t kIsoMultiExtentFlag = 0x80U;
constexpr std::size_t kIsoMinimumDirectoryRecordBytes = 34U;
constexpr std::size_t kIsoIoBufferBytes = 256U * 1024U;
constexpr std::string_view kIsoStandardIdentifier = "CD001";

struct IsoDirectoryRecord {
    std::string name;
    bool special = false;
    bool directory = false;
    std::uint32_t extent_sector = 0;
    std::uint32_t data_length = 0;
};

struct IsoEntryMetadata {
    std::string path;
    bool directory = false;
    std::uint64_t payload_offset = 0;
    std::uint64_t size = 0;
};

struct IsoPendingDirectory {
    std::string path;
    std::uint32_t extent_sector = 0;
    std::uint32_t data_length = 0;
};

struct IsoScanResult {
    std::vector<IsoEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Add two ISO byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_iso_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Multiply two ISO byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the product or throws `ArchiveError` before wraparound.
std::uint64_t checked_mul_iso_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw ArchiveError(message);
    }
    return lhs * rhs;
}

// Purpose: Read exactly one byte range from an input stream.
// Inputs: `input` is the ISO stream, `buffer` receives bytes, and `label` names the region.
// Outputs: Fills the buffer or throws when the image is truncated.
void read_exact(std::istream& input, char* buffer, std::size_t size, const char* label) {
    input.read(buffer, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Seek a file stream to a bounded ISO byte offset.
// Inputs: `input` is a seekable ISO stream, `offset` is the absolute byte offset, and `label` names diagnostics.
// Outputs: Positions the stream or throws when the seek cannot be represented/performed.
void seek_iso_offset(std::ifstream& input, std::uint64_t offset, const char* label) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek ") + label);
    }
}

// Purpose: Parse an ISO 9660 both-endian 32-bit field and require both copies to agree.
// Inputs: `bytes` points to the little-endian field followed by its big-endian duplicate, and `name` labels diagnostics.
// Outputs: Returns the decoded value or throws when the duplicate disagrees.
std::uint32_t parse_iso_both_endian_u32(const unsigned char* bytes, const char* name) {
    const std::uint32_t little =
        static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
    const std::uint32_t big =
        (static_cast<std::uint32_t>(bytes[4]) << 24U) |
        (static_cast<std::uint32_t>(bytes[5]) << 16U) |
        (static_cast<std::uint32_t>(bytes[6]) << 8U) |
        static_cast<std::uint32_t>(bytes[7]);
    if (little != big) {
        throw ArchiveError(std::string("ISO both-endian field mismatch: ") + name);
    }
    return little;
}

// Purpose: Return whether an ISO identifier byte is a decimal digit.
// Inputs: `ch` is one byte from an ISO file identifier.
// Outputs: Returns true only for ASCII digits.
bool is_ascii_digit(char ch) {
    return std::isdigit(static_cast<unsigned char>(ch)) != 0;
}

// Purpose: Normalize one ISO 9660 file identifier into a host-safe archive component candidate.
// Inputs: `identifier` is the raw directory-record identifier bytes.
// Outputs: Returns a component name with `;version` and a synthetic trailing dot removed, or a special-marker result for current/parent records.
std::pair<std::string, bool> normalize_iso_identifier(std::span<const unsigned char> identifier) {
    if (identifier.size() == 1U && identifier[0] == 0U) {
        return {".", true};
    }
    if (identifier.size() == 1U && identifier[0] == 1U) {
        return {"..", true};
    }
    if (identifier.empty()) {
        throw ArchiveError("ISO directory record has an empty identifier");
    }

    std::string name;
    name.reserve(identifier.size());
    for (const unsigned char byte : identifier) {
        if (byte == '/' || byte == '\\') {
            throw SecurityError("ISO identifier contains a path separator");
        }
        name.push_back(static_cast<char>(byte));
    }

    const auto semicolon = name.rfind(';');
    if (semicolon != std::string::npos && semicolon + 1U < name.size() &&
        std::all_of(name.begin() + static_cast<std::ptrdiff_t>(semicolon + 1U), name.end(), is_ascii_digit)) {
        name.resize(semicolon);
    }
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    if (name.empty()) {
        throw ArchiveError("ISO identifier normalizes to an empty name");
    }
    return {std::move(name), false};
}

// Purpose: Convert one ISO directory record into validated metadata.
// Inputs: `record` is the raw record bytes and `label` identifies the parser context.
// Outputs: Returns decoded record metadata, or throws on malformed/unsupported fields.
IsoDirectoryRecord parse_iso_directory_record(std::span<const unsigned char> record, const char* label) {
    if (record.size() < kIsoMinimumDirectoryRecordBytes) {
        throw ArchiveError(std::string(label) + " record is too short");
    }
    if (record[26] != 0U || record[27] != 0U) {
        throw ArchiveError("ISO interleaved files are not supported");
    }
    const auto name_length = static_cast<std::size_t>(record[32]);
    if (name_length == 0U || 33U + name_length > record.size()) {
        throw ArchiveError(std::string(label) + " identifier length is invalid");
    }
    const auto flags = record[25];
    if ((flags & kIsoMultiExtentFlag) != 0U) {
        throw ArchiveError("ISO multi-extent files are not supported");
    }
    const auto [name, special] = normalize_iso_identifier(record.subspan(33U, name_length));
    return IsoDirectoryRecord{
        .name = name,
        .special = special,
        .directory = (flags & kIsoDirectoryFlag) != 0U,
        .extent_sector = parse_iso_both_endian_u32(record.data() + 2U, "extent sector"),
        .data_length = parse_iso_both_endian_u32(record.data() + 10U, "data length"),
    };
}

// Purpose: Validate that an ISO extent is fully inside the image and compute its byte offset.
// Inputs: `extent_sector`/`data_length` describe the ISO extent and `archive_size` is the image length.
// Outputs: Returns the payload byte offset or throws before reads can cross EOF.
std::uint64_t validate_iso_extent(std::uint32_t extent_sector, std::uint32_t data_length, std::uint64_t archive_size) {
    const auto offset = checked_mul_iso_bytes(extent_sector, kIsoSectorBytes, "ISO extent offset overflow");
    const auto end = checked_add_iso_bytes(offset, data_length, "ISO extent end overflow");
    if (end > archive_size) {
        throw ArchiveError("ISO extent extends past the end of the image");
    }
    return offset;
}

// Purpose: Read a bounded ISO extent into memory for directory parsing.
// Inputs: `input` is the ISO stream, `extent_sector`/`data_length` describe the directory extent, and `archive_size` bounds reads.
// Outputs: Returns the directory bytes or throws on truncation/resource-limit violations.
std::vector<unsigned char> read_iso_directory_extent(
    std::ifstream& input,
    std::uint32_t extent_sector,
    std::uint32_t data_length,
    std::uint64_t archive_size) {
    if (data_length > kMaxArchiveIndexBytes) {
        throw ArchiveError("ISO directory extent exceeds SuperZip metadata limits");
    }
    const auto offset = validate_iso_extent(extent_sector, data_length, archive_size);
    seek_iso_offset(input, offset, "ISO directory extent");
    std::vector<unsigned char> bytes(data_length);
    if (!bytes.empty()) {
        read_exact(input, reinterpret_cast<char*>(bytes.data()), bytes.size(), "ISO directory extent");
    }
    return bytes;
}

// Purpose: Move a directory-scan offset to the next ISO sector boundary inside a directory extent.
// Inputs: `offset` is the current byte offset and `size` is the extent byte count.
// Outputs: Returns the next sector-aligned offset, clamped to the extent size.
std::size_t next_iso_sector_boundary(std::size_t offset, std::size_t size) {
    const auto next = ((static_cast<std::uint64_t>(offset) / kIsoSectorBytes) + 1U) * kIsoSectorBytes;
    return static_cast<std::size_t>(std::min<std::uint64_t>(next, size));
}

// Purpose: Join an ISO child component below a parent archive path.
// Inputs: `parent` is slash-separated and `child` is one normalized ISO identifier.
// Outputs: Returns the child archive path without touching the host filesystem.
std::string join_iso_archive_path(const std::string& parent, const std::string& child) {
    if (parent.empty()) {
        return child;
    }
    return parent + "/" + child;
}

// Purpose: Locate and parse the ISO Primary Volume Descriptor root directory.
// Inputs: `input` is the ISO stream and `archive_size` is the image length.
// Outputs: Returns the decoded root directory record or throws if no valid PVD exists.
IsoDirectoryRecord read_iso_root_record(std::ifstream& input, std::uint64_t archive_size) {
    std::array<unsigned char, kIsoSectorBytes> descriptor{};
    for (std::uint64_t ordinal = 0; ordinal < kIsoMaxVolumeDescriptors; ++ordinal) {
        const auto sector = checked_add_iso_bytes(
            kIsoPrimaryVolumeDescriptorSector,
            ordinal,
            "ISO volume descriptor sector overflow");
        const auto offset = checked_mul_iso_bytes(sector, kIsoSectorBytes, "ISO volume descriptor offset overflow");
        if (checked_add_iso_bytes(offset, kIsoSectorBytes, "ISO volume descriptor end overflow") > archive_size) {
            break;
        }
        seek_iso_offset(input, offset, "ISO volume descriptor");
        read_exact(input, reinterpret_cast<char*>(descriptor.data()), descriptor.size(), "ISO volume descriptor");
        if (std::string_view(reinterpret_cast<const char*>(descriptor.data() + 1U), kIsoStandardIdentifier.size()) !=
            kIsoStandardIdentifier || descriptor[6] != 1U) {
            continue;
        }
        if (descriptor[0] == kIsoVolumeDescriptorPrimary) {
            const auto record_length = static_cast<std::size_t>(descriptor[kIsoDirectoryRecordRootOffset]);
            if (record_length == 0U) {
                throw ArchiveError("ISO root directory record length is invalid");
            }
            auto root = parse_iso_directory_record(
                std::span<const unsigned char>(
                    descriptor.data() + kIsoDirectoryRecordRootOffset,
                    record_length),
                "ISO root directory");
            if (!root.directory) {
                throw ArchiveError("ISO root directory record is not a directory");
            }
            return root;
        }
        if (descriptor[0] == kIsoVolumeDescriptorTerminator) {
            break;
        }
    }
    throw ArchiveError("ISO primary volume descriptor was not found");
}

// Purpose: Scan a full ISO image and validate metadata before extraction.
// Inputs: `archive_path` is the ISO file to parse.
// Outputs: Returns trusted extraction metadata; throws on malformed records, unsafe paths, duplicates, or unsupported layout.
IsoScanResult scan_iso(const std::filesystem::path& archive_path) {
    const auto archive_size = std::filesystem::file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open ISO image: " + archive_path.string());
    }

    const auto root = read_iso_root_record(input, archive_size);
    validate_iso_extent(root.extent_sector, root.data_length, archive_size);

    IsoScanResult result;
    std::vector<ArchivePathValidationEntry> validation_entries;
    std::set<std::pair<std::uint32_t, std::uint32_t>> visited_directories;
    visited_directories.insert({root.extent_sector, root.data_length});
    std::vector<IsoPendingDirectory> pending{IsoPendingDirectory{
        .path = {},
        .extent_sector = root.extent_sector,
        .data_length = root.data_length,
    }};

    for (std::size_t directory_index = 0; directory_index < pending.size(); ++directory_index) {
        if (pending.size() > kMaxArchiveEntries) {
            throw ArchiveError("ISO image contains too many directories");
        }
        const auto directory = pending[directory_index];
        const auto bytes = read_iso_directory_extent(input, directory.extent_sector, directory.data_length, archive_size);
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto record_length = static_cast<std::size_t>(bytes[offset]);
            if (record_length == 0U) {
                offset = next_iso_sector_boundary(offset, bytes.size());
                continue;
            }
            if (record_length < kIsoMinimumDirectoryRecordBytes || offset + record_length > bytes.size()) {
                throw ArchiveError("ISO directory record length is invalid");
            }
            const auto record = parse_iso_directory_record(
                std::span<const unsigned char>(bytes.data() + static_cast<std::ptrdiff_t>(offset), record_length),
                "ISO directory");
            offset += record_length;
            if (record.special) {
                continue;
            }
            if (result.entries.size() >= kMaxArchiveEntries) {
                throw ArchiveError("ISO image contains too many entries");
            }
            const auto payload_offset = validate_iso_extent(record.extent_sector, record.data_length, archive_size);
            const auto path = join_iso_archive_path(directory.path, record.name);
            validation_entries.push_back(ArchivePathValidationEntry{
                .path = path,
                .directory = record.directory,
            });
            result.entries.push_back(IsoEntryMetadata{
                .path = path,
                .directory = record.directory,
                .payload_offset = payload_offset,
                .size = record.data_length,
            });
            if (record.directory) {
                if (!visited_directories.insert({record.extent_sector, record.data_length}).second) {
                    throw ArchiveError("ISO directory extent cycle or duplicate is not supported");
                }
                pending.push_back(IsoPendingDirectory{
                    .path = path,
                    .extent_sector = record.extent_sector,
                    .data_length = record.data_length,
                });
            } else {
                result.total_file_bytes = checked_add_iso_bytes(
                    result.total_file_bytes,
                    record.data_length,
                    "ISO extracted payload byte count overflow");
            }
        }
    }

    validate_archive_path_set(validation_entries);
    return result;
}

// Purpose: Copy a validated ISO file payload into a temporary output file.
// Inputs: `input` is the ISO stream, `entry` is trusted scan metadata, `target` is the final path, and `overwrite` controls replacement.
// Outputs: Publishes the verified file or throws without partially publishing target bytes.
void extract_iso_file_payload(
    std::ifstream& input,
    const IsoEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    seek_iso_offset(input, entry.payload_offset, "ISO file payload");

    std::filesystem::create_directories(target.parent_path());
    auto temporary_target = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary extracted file: " + target.string());
        }
        std::array<char, kIsoIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            read_exact(input, buffer.data(), chunk, "ISO file payload");
            output.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!output) {
                throw ArchiveError("failed to write temporary extracted file: " + target.string());
            }
            remaining -= chunk;
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

OperationStats extract_iso(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto scanned = scan_iso(archive_path);
    std::filesystem::create_directories(destination);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open ISO image: " + archive_path.string());
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());
    for (const auto& entry : scanned.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        if (entry.directory) {
            std::filesystem::create_directories(target);
            progress.finish_entry();
            continue;
        }
        extract_iso_file_payload(input, entry, target, overwrite);
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
