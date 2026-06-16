#include "cpio/cpio_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace superzip {
namespace {

constexpr std::size_t kCpioHeaderBytes = 110U;
constexpr std::size_t kCpioAlignment = 4U;
constexpr std::size_t kCpioIoBufferBytes = 64U * 1024U;
constexpr std::uint32_t kMaxCpioNameBytes = 64U * 1024U;
constexpr std::string_view kCpioNewAsciiMagic = "070701";
constexpr std::string_view kCpioCrcMagic = "070702";
constexpr std::string_view kCpioTrailerName = "TRAILER!!!";
constexpr std::uint32_t kCpioModeMask = 0170000U;
constexpr std::uint32_t kCpioRegularMode = 0100000U;
constexpr std::uint32_t kCpioDirectoryMode = 0040000U;

struct CpioHeader {
    bool crc = false;
    std::uint32_t ino = 0;
    std::uint32_t mode = 0;
    std::uint32_t nlink = 0;
    std::uint32_t filesize = 0;
    std::uint32_t namesize = 0;
    std::uint32_t check = 0;
};

struct CpioEntryMetadata {
    std::string path;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t payload_offset = 0;
};

struct CpioScanResult {
    std::vector<CpioEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Add two CPIO byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_cpio_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Return the number of bytes needed to align a CPIO section.
// Inputs: `size` is the current unaligned byte count.
// Outputs: Returns a value in `[0, 3]`.
std::uint64_t cpio_padding(std::uint64_t size) {
    const auto remainder = size % kCpioAlignment;
    return remainder == 0 ? 0 : kCpioAlignment - remainder;
}

// Purpose: Write exactly one byte range to a CPIO output stream.
// Inputs: `output` is the destination stream and `bytes` is the payload to append.
// Outputs: Appends all bytes or throws `ArchiveError` on write failure.
void write_cpio_bytes(std::ostream& output, const char* bytes, std::size_t size) {
    output.write(bytes, static_cast<std::streamsize>(size));
    if (!output) {
        throw ArchiveError("failed to write CPIO archive");
    }
}

// Purpose: Write zero bytes for CPIO section alignment.
// Inputs: `output` is the destination stream and `padding` is the exact byte count.
// Outputs: Appends zero padding or throws `ArchiveError` on write failure.
void write_zero_padding(std::ostream& output, std::uint64_t padding) {
    if (padding == 0) {
        return;
    }
    std::array<char, kCpioAlignment> zeros{};
    write_cpio_bytes(output, zeros.data(), static_cast<std::size_t>(padding));
}

// Purpose: Read an exact byte count from an input stream.
// Inputs: `input` is the archive stream, `buffer` receives bytes, and `label` names the region.
// Outputs: Fills the buffer or throws when the archive is truncated.
void read_exact(std::istream& input, char* buffer, std::size_t size, const char* label) {
    input.read(buffer, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Seek forward by an exact byte count in a CPIO file stream.
// Inputs: `input` is a seekable archive stream, `size` is the byte count, and `label` names the region.
// Outputs: Advances the stream or throws on truncation/seek failure.
void skip_file_bytes(std::istream& input, std::uint64_t size, const char* label) {
    if (size == 0) {
        return;
    }
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " is too large to seek safely");
    }
    input.seekg(static_cast<std::streamoff>(size), std::ios::cur);
    if (!input) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Parse one eight-digit hexadecimal CPIO header field.
// Inputs: `field` points to exactly eight ASCII hex digits and `name` labels diagnostics.
// Outputs: Returns the decoded 32-bit value or throws on malformed input.
std::uint32_t parse_cpio_hex_field(const char* field, const char* name) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        const auto ch = static_cast<unsigned char>(field[i]);
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = ch - '0';
        } else if (ch >= 'A' && ch <= 'F') {
            digit = 10U + ch - 'A';
        } else if (ch >= 'a' && ch <= 'f') {
            digit = 10U + ch - 'a';
        } else {
            throw ArchiveError(std::string("malformed CPIO hex field: ") + name);
        }
        value = (value << 4U) | digit;
    }
    return value;
}

// Purpose: Parse one SVR4 new ASCII CPIO header.
// Inputs: `header` is exactly 110 bytes from the archive.
// Outputs: Returns decoded metadata or throws when the magic/header is unsupported.
CpioHeader parse_cpio_header(const std::array<char, kCpioHeaderBytes>& header) {
    const std::string_view magic(header.data(), 6U);
    if (magic != kCpioNewAsciiMagic && magic != kCpioCrcMagic) {
        throw ArchiveError("unsupported or malformed CPIO magic");
    }
    CpioHeader parsed;
    parsed.crc = magic == kCpioCrcMagic;
    parsed.ino = parse_cpio_hex_field(header.data() + 6, "ino");
    parsed.mode = parse_cpio_hex_field(header.data() + 14, "mode");
    parsed.nlink = parse_cpio_hex_field(header.data() + 38, "nlink");
    parsed.filesize = parse_cpio_hex_field(header.data() + 54, "filesize");
    parsed.namesize = parse_cpio_hex_field(header.data() + 94, "namesize");
    parsed.check = parse_cpio_hex_field(header.data() + 102, "check");
    return parsed;
}

// Purpose: Format one eight-digit uppercase hexadecimal CPIO field.
// Inputs: `output` receives bytes and `value` is a 32-bit header field.
// Outputs: Appends exactly eight hex digits or throws if formatting fails.
void append_cpio_hex_field(std::string& output, std::uint32_t value) {
    std::array<char, 9> encoded{};
    const int written = std::snprintf(encoded.data(), encoded.size(), "%08X", value);
    if (written != 8) {
        throw ArchiveError("failed to encode CPIO header field");
    }
    output.append(encoded.data(), 8U);
}

// Purpose: Build one SVR4 new ASCII CPIO header.
// Inputs: `path` is the entry name, `mode` is the CPIO mode bits, `size` is the payload size, and `ino` is a stable archive ordinal.
// Outputs: Returns the 110-byte header string.
std::string make_cpio_header(std::string_view path, std::uint32_t mode, std::uint32_t size, std::uint32_t ino) {
    if (path.size() + 1U > kMaxCpioNameBytes) {
        throw ArchiveError("CPIO entry name is too long");
    }
    std::string header;
    header.reserve(kCpioHeaderBytes);
    header.append(kCpioNewAsciiMagic);
    append_cpio_hex_field(header, ino);
    append_cpio_hex_field(header, mode);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, 1);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, size);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, 0);
    append_cpio_hex_field(header, static_cast<std::uint32_t>(path.size() + 1U));
    append_cpio_hex_field(header, 0);
    if (header.size() != kCpioHeaderBytes) {
        throw ArchiveError("internal CPIO header size mismatch");
    }
    return header;
}

// Purpose: Write one CPIO entry header and name.
// Inputs: `output` is the archive stream, `path` is the entry name, `mode`/`size`/`ino` are header fields.
// Outputs: Appends header/name/alignment padding or throws on writer failure.
void write_cpio_entry_header(
    std::ostream& output,
    std::string_view path,
    std::uint32_t mode,
    std::uint32_t size,
    std::uint32_t ino) {
    const auto header = make_cpio_header(path, mode, size, ino);
    write_cpio_bytes(output, header.data(), header.size());
    write_cpio_bytes(output, path.data(), path.size());
    const char nul = '\0';
    write_cpio_bytes(output, &nul, 1U);
    write_zero_padding(output, cpio_padding(kCpioHeaderBytes + path.size() + 1U));
}

// Purpose: Copy a bounded file payload into a CPIO output stream.
// Inputs: `source` is a regular file path, `output` is the archive stream, and `expected_size` bounds copied bytes.
// Outputs: Writes exactly `expected_size` bytes or throws on read/write failure.
void copy_file_to_cpio(const std::filesystem::path& source, std::ostream& output, std::uint64_t expected_size) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open file for CPIO archive: " + source.string());
    }
    std::array<char, kCpioIoBufferBytes> buffer{};
    std::uint64_t remaining = expected_size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw ArchiveError("failed to read file for CPIO archive: " + source.string());
        }
        write_cpio_bytes(output, buffer.data(), chunk);
        remaining -= chunk;
    }
}

// Purpose: Copy a validated CPIO file payload into a temporary output file.
// Inputs: `input` is the archive stream, `entry` is trusted scan metadata, `target` is the final path, and `overwrite` controls replacement.
// Outputs: Publishes the verified file or throws without partially publishing target bytes.
void extract_cpio_file_payload(
    std::ifstream& input,
    const CpioEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    if (entry.payload_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError("CPIO payload offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(entry.payload_offset), std::ios::beg);
    if (!input) {
        throw ArchiveError("failed to seek CPIO payload");
    }

    std::filesystem::create_directories(target.parent_path());
    auto temporary_target = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary extracted file: " + target.string());
        }
        std::array<char, kCpioIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            read_exact(input, buffer.data(), chunk, "CPIO file payload");
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

// Purpose: Read and checksum a CPIO CRC payload during validation.
// Inputs: `input` is positioned at the payload, `size` is the expected payload size.
// Outputs: Returns the unsigned byte sum required by the `070702` variant.
std::uint32_t checksum_cpio_payload(std::istream& input, std::uint64_t size) {
    std::array<char, kCpioIoBufferBytes> buffer{};
    std::uint64_t remaining = size;
    std::uint32_t sum = 0;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        read_exact(input, buffer.data(), chunk, "CPIO CRC payload");
        for (std::size_t i = 0; i < chunk; ++i) {
            sum += static_cast<unsigned char>(buffer[i]);
        }
        remaining -= chunk;
    }
    return sum;
}

// Purpose: Validate one CPIO entry type and size combination.
// Inputs: `header` is decoded metadata and `name` is the archive path.
// Outputs: Returns true for directories, false for regular files, or throws for unsupported/special entries.
bool classify_cpio_entry(const CpioHeader& header, const std::string& name) {
    const auto type = header.mode & kCpioModeMask;
    if (type == kCpioDirectoryMode) {
        if (header.filesize != 0) {
            throw ArchiveError("CPIO directory entry has a payload: " + name);
        }
        return true;
    }
    if (type == kCpioRegularMode) {
        if (header.nlink > 1U) {
            throw SecurityError("refusing CPIO hard-link metadata: " + name);
        }
        return false;
    }
    throw SecurityError("refusing unsupported CPIO special entry: " + name);
}

// Purpose: Scan a full CPIO archive and validate metadata before extraction.
// Inputs: `archive_path` is the CPIO file to parse.
// Outputs: Returns trusted extraction metadata; throws on malformed headers, unsafe paths, duplicates, or unsupported entries.
CpioScanResult scan_cpio(const std::filesystem::path& archive_path) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open CPIO archive: " + archive_path.string());
    }

    CpioScanResult result;
    std::vector<ArchivePathValidationEntry> validation_entries;
    while (true) {
        std::array<char, kCpioHeaderBytes> raw_header{};
        read_exact(input, raw_header.data(), raw_header.size(), "CPIO header");
        const auto header = parse_cpio_header(raw_header);
        if (header.namesize == 0 || header.namesize > kMaxCpioNameBytes) {
            throw ArchiveError("CPIO entry name size is invalid");
        }

        std::string raw_name(header.namesize, '\0');
        read_exact(input, raw_name.data(), raw_name.size(), "CPIO entry name");
        if (raw_name.back() != '\0') {
            throw ArchiveError("CPIO entry name is not NUL-terminated");
        }
        raw_name.pop_back();
        skip_file_bytes(input, cpio_padding(kCpioHeaderBytes + header.namesize), "CPIO name padding");

        if (raw_name == kCpioTrailerName) {
            if (header.filesize != 0) {
                throw ArchiveError("CPIO trailer has a payload");
            }
            break;
        }
        if (result.entries.size() >= kMaxArchiveEntries) {
            throw ArchiveError("CPIO archive contains too many entries");
        }

        const bool directory = classify_cpio_entry(header, raw_name);
        const auto payload_position = input.tellg();
        if (payload_position == std::istream::pos_type(-1)) {
            throw ArchiveError("failed to read CPIO payload offset");
        }
        const auto payload_offset = static_cast<std::uint64_t>(payload_position);
        if (header.crc) {
            const auto actual = checksum_cpio_payload(input, header.filesize);
            if (actual != header.check) {
                throw ArchiveError("CPIO CRC entry checksum mismatch: " + raw_name);
            }
        } else {
            skip_file_bytes(input, header.filesize, "CPIO file payload");
        }
        skip_file_bytes(input, cpio_padding(header.filesize), "CPIO file padding");

        validation_entries.push_back(ArchivePathValidationEntry{
            .path = raw_name,
            .directory = directory,
        });
        result.entries.push_back(CpioEntryMetadata{
            .path = raw_name,
            .directory = directory,
            .size = header.filesize,
            .payload_offset = payload_offset,
        });
        if (!directory) {
            result.total_file_bytes = checked_add_cpio_bytes(
                result.total_file_bytes,
                header.filesize,
                "CPIO uncompressed payload byte count overflow");
        }
    }

    validate_archive_path_set(validation_entries);
    return result;
}

}  // namespace

OperationStats compress_cpio(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    std::ofstream output(output_archive, std::ios::binary);
    if (!output) {
        throw ArchiveError("cannot create CPIO archive: " + output_archive.string());
    }

    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.entries.size());
    std::uint32_t ino = 1;
    for (const auto& entry : manifest.entries) {
        if (entry.size > std::numeric_limits<std::uint32_t>::max()) {
            throw ArchiveError("CPIO new ASCII format cannot store file larger than 4 GiB: " + entry.archive_path);
        }
        progress.set_current(entry.archive_path);
        publish_progress(progress, progress_callback);
        const auto name = entry.directory && entry.archive_path.ends_with('/')
            ? std::string_view(entry.archive_path).substr(0, entry.archive_path.size() - 1U)
            : std::string_view(entry.archive_path);
        write_cpio_entry_header(
            output,
            name,
            entry.directory ? (kCpioDirectoryMode | 0755U) : (kCpioRegularMode | 0644U),
            entry.directory ? 0U : static_cast<std::uint32_t>(entry.size),
            ino++);
        if (!entry.directory) {
            copy_file_to_cpio(entry.source_path, output, entry.size);
            write_zero_padding(output, cpio_padding(entry.size));
            progress.add_bytes(entry.size);
        }
        progress.finish_entry();
    }
    write_cpio_entry_header(output, kCpioTrailerName, 0, 0, ino);
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize CPIO archive: " + output_archive.string());
    }

    OperationStats stats;
    stats.input_bytes = manifest.total_file_bytes;
    stats.output_bytes = std::filesystem::file_size(output_archive);
    stats.entries = static_cast<std::uint64_t>(manifest.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_cpio(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto scanned = scan_cpio(archive_path);
    std::filesystem::create_directories(destination);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open CPIO archive: " + archive_path.string());
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
        extract_cpio_file_payload(input, entry, target, overwrite);
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
