#include "macbinary/macbinary_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace superzip {
namespace {

constexpr std::uint64_t kMacBinaryHeaderBytes = 128U;
constexpr std::uint64_t kMacBinaryBlockBytes = 128U;
constexpr std::size_t kCopyBufferBytes = 64U * 1024U;

struct MacBinaryHeader {
    std::string entry_name;
    std::uint64_t data_offset = 0;
    std::uint64_t data_length = 0;
};

// Purpose: Read a filesystem file size into the archive telemetry type.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried or represented.
std::uint64_t regular_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("file size exceeds SuperZip limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

// Purpose: Add byte counts while detecting telemetry overflow.
// Inputs: `lhs` and `rhs` are byte counts and `context` identifies the counter for diagnostics.
// Outputs: Returns the sum or throws before unsigned wraparound.
std::uint64_t checked_sum(std::uint64_t lhs, std::uint64_t rhs, const char* context) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    return lhs + rhs;
}

// Purpose: Round a byte count up to a MacBinary 128-byte packet boundary.
// Inputs: `value` is a fork or secondary-header length.
// Outputs: Returns the padded byte count or throws on overflow.
std::uint64_t round_up_macbinary_block(std::uint64_t value) {
    if (value == 0U) {
        return 0U;
    }
    const auto with_padding = checked_sum(value, kMacBinaryBlockBytes - 1U, "MacBinary padding");
    return (with_padding / kMacBinaryBlockBytes) * kMacBinaryBlockBytes;
}

// Purpose: Read a big-endian 16-bit integer from the fixed MacBinary header.
// Inputs: `header` is the 128-byte header and `offset` is the first of two bytes.
// Outputs: Returns the decoded integer.
std::uint16_t read_be_u16(const std::array<unsigned char, 128>& header, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(header[offset]) << 8U) |
        static_cast<std::uint16_t>(header[offset + 1U]));
}

// Purpose: Read a big-endian 32-bit integer from the fixed MacBinary header.
// Inputs: `header` is the 128-byte header and `offset` is the first of four bytes.
// Outputs: Returns the decoded integer.
std::uint32_t read_be_u32(const std::array<unsigned char, 128>& header, std::size_t offset) {
    return (static_cast<std::uint32_t>(header[offset]) << 24U) |
        (static_cast<std::uint32_t>(header[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(header[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(header[offset + 3U]);
}

// Purpose: Update a CRC-16/XMODEM register with one byte.
// Inputs: `crc` is the mutable register and `byte` is the next header byte.
// Outputs: Mutates `crc` without allocation or I/O.
void update_crc16_xmodem(std::uint16_t& crc, std::uint8_t byte) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8U));
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
        if ((crc & 0x8000U) != 0U) {
            crc = static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U);
        } else {
            crc = static_cast<std::uint16_t>(crc << 1U);
        }
    }
}

// Purpose: Compute the MacBinary II/III header CRC over bytes 0 through 123.
// Inputs: `header` is the untrusted 128-byte MacBinary header.
// Outputs: Returns the CRC-16/XMODEM value expected at bytes 124 through 125.
std::uint16_t macbinary_header_crc(const std::array<unsigned char, 128>& header) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < 124U; ++i) {
        update_crc16_xmodem(crc, static_cast<std::uint8_t>(header[i]));
    }
    return crc;
}

// Purpose: Classify whether the header carries a MacBinary II/III CRC marker.
// Inputs: `header` is the untrusted 128-byte MacBinary header.
// Outputs: Returns true when version/signature fields make the stored CRC authoritative.
bool has_macbinary_crc_marker(const std::array<unsigned char, 128>& header) {
    const bool versioned = (header[122] == 0x81U || header[122] == 0x82U) &&
        (header[123] == 0x81U || header[123] == 0x82U);
    const bool signed_v3 = header[102] == static_cast<unsigned char>('m') &&
        header[103] == static_cast<unsigned char>('B') &&
        header[104] == static_cast<unsigned char>('I') &&
        header[105] == static_cast<unsigned char>('N');
    return versioned || signed_v3;
}

// Purpose: Decode a path-safe ASCII filename from the MacBinary Pascal string.
// Inputs: `header` contains the raw MacBinary filename bytes.
// Outputs: Returns a normalized SuperZip archive path key or throws on unsafe or unsupported names.
std::string parse_macbinary_name(const std::array<unsigned char, 128>& header) {
    const auto name_length = static_cast<std::size_t>(header[1]);
    if (name_length == 0U || name_length > 63U) {
        throw SecurityError("MacBinary filename length is outside 1..63 bytes");
    }
    std::string name;
    name.reserve(name_length);
    for (std::size_t i = 0; i < name_length; ++i) {
        const auto byte = header[2U + i];
        if (byte < 0x20U || byte > 0x7EU) {
            throw SecurityError("MacBinary filename uses unsupported non-ASCII or control bytes");
        }
        if (byte == static_cast<unsigned char>(':')) {
            throw SecurityError("MacBinary filename contains a classic Mac path separator");
        }
        name.push_back(static_cast<char>(byte));
    }
    return normalize_archive_path_key(name);
}

// Purpose: Validate MacBinary fixed-header fields before fork offsets are trusted.
// Inputs: `header` is the untrusted 128-byte MacBinary header.
// Outputs: Throws when required zero fields, signed lengths, or header CRC checks fail.
void validate_macbinary_header_fields(const std::array<unsigned char, 128>& header) {
    if (header[0] != 0U || header[74] != 0U || header[82] != 0U) {
        throw ArchiveError("MacBinary header required zero fields are invalid");
    }
    if ((read_be_u32(header, 83U) & 0x80000000U) != 0U ||
        (read_be_u32(header, 87U) & 0x80000000U) != 0U) {
        throw ArchiveError("MacBinary fork length uses an unsupported signed value");
    }
    if (has_macbinary_crc_marker(header)) {
        const auto expected = macbinary_header_crc(header);
        const auto actual = read_be_u16(header, 124U);
        if (expected != actual) {
            throw ArchiveError("MacBinary header CRC mismatch");
        }
    }
}

// Purpose: Ensure declared MacBinary fork extents are inside the input stream.
// Inputs: `header` carries declared lengths and `file_size` is the archive byte count.
// Outputs: Returns parsed fork offsets and lengths, or throws on truncation/overflow.
MacBinaryHeader parse_macbinary_header(
    const std::array<unsigned char, 128>& header,
    std::uint64_t file_size) {
    validate_macbinary_header_fields(header);

    const auto secondary_length = static_cast<std::uint64_t>(read_be_u16(header, 120U));
    const auto data_length = static_cast<std::uint64_t>(read_be_u32(header, 83U));
    const auto resource_length = static_cast<std::uint64_t>(read_be_u32(header, 87U));
    const auto comment_length = static_cast<std::uint64_t>(read_be_u16(header, 99U));
    const auto data_offset = checked_sum(
        kMacBinaryHeaderBytes,
        round_up_macbinary_block(secondary_length),
        "MacBinary data offset");
    const auto data_end = checked_sum(data_offset, data_length, "MacBinary data fork");
    if (data_end > file_size) {
        throw ArchiveError("MacBinary data fork extends past end of file");
    }

    const auto resource_offset = checked_sum(
        data_offset,
        round_up_macbinary_block(data_length),
        "MacBinary resource offset");
    if (resource_length > 0U) {
        const auto resource_end = checked_sum(resource_offset, resource_length, "MacBinary resource fork");
        if (resource_end > file_size) {
            throw ArchiveError("MacBinary resource fork extends past end of file");
        }
    }
    if (comment_length > 0U) {
        const auto comment_offset = checked_sum(
            resource_offset,
            round_up_macbinary_block(resource_length),
            "MacBinary comment offset");
        const auto comment_end = checked_sum(comment_offset, comment_length, "MacBinary comment");
        if (comment_end > file_size) {
            throw ArchiveError("MacBinary Get Info comment extends past end of file");
        }
    }

    return MacBinaryHeader{
        .entry_name = parse_macbinary_name(header),
        .data_offset = data_offset,
        .data_length = data_length,
    };
}

// Purpose: Read the fixed MacBinary header from a stream positioned at byte zero.
// Inputs: `input` is an open binary archive stream.
// Outputs: Returns exactly 128 header bytes or throws on short reads.
std::array<unsigned char, 128> read_macbinary_header(std::ifstream& input) {
    std::array<unsigned char, 128> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        throw ArchiveError("MacBinary archive is shorter than its fixed header");
    }
    if (!input && !input.eof()) {
        throw ArchiveError("failed to read MacBinary header");
    }
    input.clear();
    return header;
}

// Purpose: Copy exactly the declared data fork into a verified temporary file.
// Inputs: `input` is the archive, `header` defines data location/length, `output` is the private temp stream, and `progress` reports copied bytes.
// Outputs: Returns bytes copied, or throws on truncated input or output failure.
std::uint64_t copy_data_fork(
    std::ifstream& input,
    const MacBinaryHeader& header,
    std::ofstream& output,
    ProgressState& progress,
    const ProgressCallback& progress_callback) {
    input.clear();
    input.seekg(static_cast<std::streamoff>(header.data_offset), std::ios::beg);
    if (!input) {
        throw ArchiveError("failed to seek to MacBinary data fork");
    }
    std::array<char, kCopyBufferBytes> buffer{};
    std::uint64_t copied = 0;
    while (copied < header.data_length) {
        const auto remaining = header.data_length - copied;
        const auto chunk = static_cast<std::streamsize>(
            remaining < buffer.size() ? remaining : buffer.size());
        input.read(buffer.data(), chunk);
        const auto bytes_read = input.gcount();
        if (bytes_read != chunk) {
            throw ArchiveError("MacBinary data fork is truncated");
        }
        output.write(buffer.data(), bytes_read);
        if (!output) {
            throw ArchiveError("failed to write MacBinary data fork");
        }
        copied += static_cast<std::uint64_t>(bytes_read);
        progress.add_bytes(static_cast<std::uint64_t>(bytes_read));
        publish_progress(progress, progress_callback);
    }
    return copied;
}

}  // namespace

// Purpose: Extract the data fork from one MacBinary stream with path-safe publication.
// Inputs: `archive_path` is the MacBinary stream, `destination` is the extraction root, `overwrite` controls replacement, and `progress_callback` receives synchronous progress snapshots.
// Outputs: Returns operation statistics; throws on malformed headers, unsafe names, truncated forks, refused overwrite, or verified-file publication failure.
OperationStats extract_macbinary_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open MacBinary archive: " + archive_path.string());
    }
    const auto raw_header = read_macbinary_header(input);
    const auto header = parse_macbinary_header(raw_header, archive_size);

    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, header.entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing MacBinary extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(header.entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    std::uint64_t output_size = 0;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create MacBinary extraction target: " + target.string());
        }
        output_size = copy_data_fork(input, header, output, progress, progress_callback);
        if (archive_size >= output_size) {
            progress.add_bytes(archive_size - output_size);
            publish_progress(progress, progress_callback);
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize MacBinary extraction target: " + target.string());
        }
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
    } catch (...) {
        cleanup_file_publish_target(temporary);
        throw;
    }

    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
