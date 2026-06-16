#include "tar/tar_adapter.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"
#include "xz/xz_stream.hpp"
#include "zstd/zstd_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

namespace superzip {
namespace {

constexpr std::size_t kTarBlockSize = 512U;
constexpr std::size_t kTarIoBufferBytes = 64U * 1024U;
constexpr std::uint64_t kMaxTarMetadataPayloadBytes = 1024U * 1024U;

struct TarHeaderName {
    std::string name;
    std::string prefix;
};

struct TarEntryMetadata {
    std::string path;
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t payload_offset = 0;
};

struct TarScanResult {
    std::vector<TarEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
};

struct TarWriteStats {
    std::uint64_t input_bytes = 0;
    std::uint64_t entries = 0;
};

struct PendingTarExtensions {
    std::optional<std::string> path;
};

// Purpose: Add two TAR byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_tar_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Return the number of padding bytes needed after a TAR payload.
// Inputs: `size` is a payload byte count.
// Outputs: Returns a value in `[0, 511]`.
std::uint64_t tar_padding(std::uint64_t size) {
    const auto remainder = size % kTarBlockSize;
    return remainder == 0 ? 0 : kTarBlockSize - remainder;
}

// Purpose: Write exactly one byte range to an output stream.
// Inputs: `output` is the destination stream and `bytes` is the payload to append.
// Outputs: Appends all bytes or throws `ArchiveError` on write failure.
void write_bytes(std::ostream& output, const char* bytes, std::size_t size) {
    output.write(bytes, static_cast<std::streamsize>(size));
    if (!output) {
        throw ArchiveError("failed to write TAR archive");
    }
}

// Purpose: Write zero bytes to align a TAR payload to the next block.
// Inputs: `output` is the destination stream and `size` is the unpadded payload size.
// Outputs: Appends zero padding or throws `ArchiveError` on write failure.
void write_tar_padding(std::ostream& output, std::uint64_t size) {
    const auto padding = tar_padding(size);
    if (padding == 0) {
        return;
    }
    std::array<char, kTarBlockSize> zeros{};
    write_bytes(output, zeros.data(), static_cast<std::size_t>(padding));
}

// Purpose: Consume an exact byte count from a possibly non-seekable input stream.
// Inputs: `input` is the source stream, `size` is the byte count, and `label` names the consumed region.
// Outputs: Reads and discards exactly `size` bytes, or throws on truncation/read failure.
void discard_stream_bytes(std::istream& input, std::uint64_t size, const char* label) {
    std::array<char, kTarIoBufferBytes> buffer{};
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw ArchiveError(std::string(label) + " is truncated");
        }
        remaining -= chunk;
    }
}

// Purpose: Copy a bounded file payload into a TAR output stream.
// Inputs: `source` is an open filesystem path, `output` is the archive stream, and `expected_size` bounds copied bytes.
// Outputs: Writes exactly `expected_size` bytes or throws on read/write failure.
void copy_file_to_tar(const std::filesystem::path& source, std::ostream& output, std::uint64_t expected_size) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open file for TAR archive: " + source.string());
    }
    std::array<char, kTarIoBufferBytes> buffer{};
    std::uint64_t remaining = expected_size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw ArchiveError("failed to read file for TAR archive: " + source.string());
        }
        write_bytes(output, buffer.data(), chunk);
        remaining -= chunk;
    }
}

// Purpose: Fill a fixed-size TAR header string field.
// Inputs: `header` is the mutable header block, `offset`/`length` select the field, and `value` is ASCII metadata.
// Outputs: Copies the value and leaves trailing NUL bytes; throws when the value does not fit.
void put_tar_string(std::array<char, kTarBlockSize>& header, std::size_t offset, std::size_t length, const std::string& value) {
    if (value.size() > length) {
        throw ArchiveError("TAR header field is too long: " + value);
    }
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Write a positive TAR numeric field using octal or POSIX base-256 encoding.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is the numeric value.
// Outputs: Encodes the field in-place; throws when the value cannot fit.
void put_tar_number(std::array<char, kTarBlockSize>& header, std::size_t offset, std::size_t length, std::uint64_t value) {
    const auto octal_digits = length - 1U;
    std::uint64_t octal_limit = 1;
    for (std::size_t i = 0; i < octal_digits; ++i) {
        if (octal_limit > std::numeric_limits<std::uint64_t>::max() / 8U) {
            octal_limit = std::numeric_limits<std::uint64_t>::max();
            break;
        }
        octal_limit *= 8U;
    }
    if (value < octal_limit) {
        std::string encoded(octal_digits, '0');
        auto remaining = value;
        for (std::size_t i = 0; i < octal_digits; ++i) {
            const auto index = octal_digits - i - 1U;
            encoded[index] = static_cast<char>('0' + (remaining & 7U));
            remaining >>= 3U;
        }
        std::copy(encoded.begin(), encoded.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
        header[offset + length - 1U] = '\0';
        return;
    }

    if (length < 2U) {
        throw ArchiveError("TAR numeric field cannot fit value");
    }
    for (std::size_t i = 0; i < length; ++i) {
        header[offset + i] = 0;
    }
    auto remaining = value;
    for (std::size_t i = 0; i < length - 1U; ++i) {
        header[offset + length - 1U - i] = static_cast<char>(remaining & 0xFFU);
        remaining >>= 8U;
    }
    if (remaining != 0) {
        throw ArchiveError("TAR numeric field cannot fit value");
    }
    header[offset] = static_cast<char>(0x80U);
}

// Purpose: Encode the TAR checksum field after all other header fields are populated.
// Inputs: `header` is one mutable 512-byte TAR header.
// Outputs: Writes the checksum field in-place.
void finalize_tar_checksum(std::array<char, kTarBlockSize>& header) {
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    std::uint32_t checksum = 0;
    for (const auto byte : header) {
        checksum += static_cast<unsigned char>(byte);
    }
    std::array<char, 8> encoded{};
    std::snprintf(encoded.data(), encoded.size(), "%06o", checksum);
    std::copy(encoded.begin(), encoded.begin() + 6, header.begin() + 148);
    header[154] = '\0';
    header[155] = ' ';
}

// Purpose: Try to split a path into USTAR name/prefix fields.
// Inputs: `path` is a normalized archive path.
// Outputs: Returns fields that fit USTAR limits, or empty when PAX is required.
std::optional<TarHeaderName> split_ustar_name(const std::string& path) {
    if (path.size() <= 100U) {
        return TarHeaderName{.name = path, .prefix = ""};
    }
    for (std::size_t split = path.rfind('/'); split != std::string::npos; split = path.rfind('/', split - 1U)) {
        const auto prefix = path.substr(0, split);
        const auto name = path.substr(split + 1U);
        if (!prefix.empty() && !name.empty() && prefix.size() <= 155U && name.size() <= 100U) {
            return TarHeaderName{.name = name, .prefix = prefix};
        }
        if (split == 0) {
            break;
        }
    }
    return std::nullopt;
}

// Purpose: Build one TAR header block for a file, directory, or metadata payload.
// Inputs: `path` is the header path, `typeflag` is the TAR entry type, `size` is payload size, and `mode` is the POSIX mode bits.
// Outputs: Returns a complete checksummed 512-byte header block.
std::array<char, kTarBlockSize> make_tar_header(
    const std::string& path,
    char typeflag,
    std::uint64_t size,
    std::uint32_t mode) {
    auto fields = split_ustar_name(path);
    if (!fields) {
        throw ArchiveError("TAR path exceeds USTAR header limits without PAX metadata: " + path);
    }

    std::array<char, kTarBlockSize> header{};
    put_tar_string(header, 0, 100, fields->name);
    put_tar_number(header, 100, 8, mode);
    put_tar_number(header, 108, 8, 0);
    put_tar_number(header, 116, 8, 0);
    put_tar_number(header, 124, 12, size);
    put_tar_number(header, 136, 12, 0);
    header[156] = typeflag;
    put_tar_string(header, 257, 6, "ustar");
    put_tar_string(header, 263, 2, "00");
    put_tar_string(header, 265, 32, "superzip");
    put_tar_string(header, 297, 32, "superzip");
    put_tar_string(header, 345, 155, fields->prefix);
    finalize_tar_checksum(header);
    return header;
}

// Purpose: Create one POSIX PAX key/value record.
// Inputs: `key` and `value` are UTF-8/ASCII metadata strings.
// Outputs: Returns a length-prefixed PAX record.
std::string make_pax_record(std::string_view key, std::string_view value) {
    std::string body;
    body.reserve(key.size() + value.size() + 4U);
    body.push_back(' ');
    body.append(key);
    body.push_back('=');
    body.append(value);
    body.push_back('\n');

    std::size_t digits = 1;
    for (;;) {
        const auto length = body.size() + digits;
        const auto actual_digits = std::to_string(length).size();
        if (actual_digits == digits) {
            return std::to_string(length) + body;
        }
        digits = actual_digits;
    }
}

// Purpose: Emit a PAX path override when a path cannot fit in USTAR name/prefix fields.
// Inputs: `output` is the TAR stream, `path` is the full archive path, and `ordinal` makes the metadata name stable.
// Outputs: Writes a PAX extended header and padded payload when needed.
void write_pax_path_if_needed(std::ostream& output, const std::string& path, std::uint64_t ordinal) {
    if (split_ustar_name(path).has_value()) {
        return;
    }
    const auto payload = make_pax_record("path", path);
    const auto metadata_path = "PaxHeaders.X/superzip-" + std::to_string(ordinal);
    const auto header = make_tar_header(metadata_path, 'x', payload.size(), 0644);
    write_bytes(output, header.data(), header.size());
    write_bytes(output, payload.data(), payload.size());
    write_tar_padding(output, payload.size());
}

// Purpose: Emit a TAR header that may rely on an immediately preceding PAX path override.
// Inputs: `output` is the TAR stream, `path` is the full archive path, `typeflag` and `size` define the entry, and `ordinal` labels optional PAX metadata.
// Outputs: Writes one header and optional PAX metadata.
void write_entry_header(
    std::ostream& output,
    const std::string& path,
    char typeflag,
    std::uint64_t size,
    std::uint64_t ordinal) {
    write_pax_path_if_needed(output, path, ordinal);
    const auto header_path = split_ustar_name(path).has_value() ? path : "superzip-pax-entry";
    const auto header = make_tar_header(header_path, typeflag, size, typeflag == '5' ? 0755 : 0644);
    write_bytes(output, header.data(), header.size());
}

// Purpose: Detect an all-zero TAR end-of-archive block.
// Inputs: `header` is one 512-byte TAR block.
// Outputs: Returns true when all bytes are zero.
bool is_zero_block(const std::array<char, kTarBlockSize>& header) {
    return std::ranges::all_of(header, [](char ch) {
        return ch == '\0';
    });
}

// Purpose: Extract a NUL-terminated string from a fixed TAR header field.
// Inputs: `header` is the source block and `offset`/`length` select the field.
// Outputs: Returns the field bytes before the first NUL.
std::string read_tar_string(const std::array<char, kTarBlockSize>& header, std::size_t offset, std::size_t length) {
    const auto begin = header.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    const auto nul = std::find(begin, end, '\0');
    return std::string(begin, nul);
}

// Purpose: Parse an octal TAR numeric field.
// Inputs: `field` is the fixed header field and `label` names the field for diagnostics.
// Outputs: Returns the decoded value; throws on malformed octal data.
std::uint64_t parse_tar_octal(std::span<const char> field, const char* label) {
    std::uint64_t value = 0;
    bool saw_digit = false;
    for (const char raw : field) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\0' || ch == ' ') {
            if (saw_digit) {
                break;
            }
            continue;
        }
        if (ch < '0' || ch > '7') {
            throw ArchiveError(std::string("malformed TAR octal field: ") + label);
        }
        saw_digit = true;
        if (value > (std::numeric_limits<std::uint64_t>::max() >> 3U)) {
            throw ArchiveError(std::string("TAR octal field overflows: ") + label);
        }
        value = (value << 3U) + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

// Purpose: Parse a positive TAR numeric field encoded as octal or POSIX base-256.
// Inputs: `header` is the source block and `offset`/`length` select the numeric field.
// Outputs: Returns the decoded value; throws on malformed or overflowing data.
std::uint64_t parse_tar_number(
    const std::array<char, kTarBlockSize>& header,
    std::size_t offset,
    std::size_t length,
    const char* label) {
    const auto field = std::span<const char>(header.data() + offset, length);
    if ((static_cast<unsigned char>(field.front()) & 0x80U) == 0) {
        return parse_tar_octal(field, label);
    }
    std::uint64_t value = static_cast<unsigned char>(field.front()) & 0x7FU;
    for (std::size_t i = 1; i < field.size(); ++i) {
        if (value > (std::numeric_limits<std::uint64_t>::max() >> 8U)) {
            throw ArchiveError(std::string("TAR base-256 field overflows: ") + label);
        }
        value = (value << 8U) | static_cast<unsigned char>(field[i]);
    }
    return value;
}

// Purpose: Verify a TAR header checksum before trusting metadata fields.
// Inputs: `header` is one nonzero TAR header block.
// Outputs: Returns normally when checksum matches; throws `ArchiveError` otherwise.
void validate_tar_checksum(const std::array<char, kTarBlockSize>& header) {
    const auto stored = parse_tar_octal(std::span<const char>(header.data() + 148, 8), "checksum");
    std::uint32_t actual = 0;
    for (std::size_t i = 0; i < header.size(); ++i) {
        actual += (i >= 148U && i < 156U) ? static_cast<unsigned char>(' ') : static_cast<unsigned char>(header[i]);
    }
    if (stored != actual) {
        throw ArchiveError("TAR header checksum mismatch");
    }
}

// Purpose: Return a validated TAR entry path from header fields and pending extension metadata.
// Inputs: `header` contains USTAR fields and `pending` may contain a PAX/GNU long path override.
// Outputs: Returns the path used for validation/extraction and clears one-shot path metadata.
std::string tar_entry_path(const std::array<char, kTarBlockSize>& header, PendingTarExtensions& pending) {
    if (pending.path.has_value()) {
        auto path = *pending.path;
        pending.path.reset();
        return path;
    }
    const auto name = read_tar_string(header, 0, 100);
    const auto prefix = read_tar_string(header, 345, 155);
    if (name.empty()) {
        throw ArchiveError("TAR entry path is empty");
    }
    return prefix.empty() ? name : prefix + "/" + name;
}

// Purpose: Advance an input stream past a TAR payload and its padding.
// Inputs: `input` is positioned at payload start, `size` is the payload byte count, and `seekable` selects seek or discard mode.
// Outputs: Moves to the next header or throws on invalid seek.
void skip_tar_payload(std::istream& input, std::uint64_t size, bool seekable) {
    const auto padded = checked_add_tar_bytes(size, tar_padding(size), "TAR payload size overflows");
    if (!seekable) {
        discard_stream_bytes(input, padded, "TAR payload");
        return;
    }
    if (padded > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError("TAR payload is too large to seek");
    }
    input.seekg(static_cast<std::streamoff>(padded), std::ios::cur);
    if (!input) {
        throw ArchiveError("TAR payload is truncated");
    }
}

// Purpose: Read a bounded metadata payload into memory.
// Inputs: `input` is positioned at payload start and `size` is the metadata byte count.
// Outputs: Returns payload bytes as a string and consumes payload padding.
std::string read_tar_metadata_payload(std::istream& input, std::uint64_t size) {
    if (size > kMaxTarMetadataPayloadBytes) {
        throw ArchiveError("TAR metadata payload exceeds SuperZip resource limit");
    }
    std::string payload(static_cast<std::size_t>(size), '\0');
    input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (static_cast<std::uint64_t>(input.gcount()) != size) {
        throw ArchiveError("TAR metadata payload is truncated");
    }
    const auto padding = tar_padding(size);
    if (padding > 0) {
        discard_stream_bytes(input, padding, "TAR metadata padding");
    }
    return payload;
}

// Purpose: Parse POSIX PAX records relevant to secure extraction.
// Inputs: `payload` is a bounded PAX extended-header payload.
// Outputs: Returns pending one-shot path metadata; ignores unrelated keys.
PendingTarExtensions parse_pax_payload(const std::string& payload) {
    PendingTarExtensions parsed;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto space = payload.find(' ', offset);
        if (space == std::string::npos) {
            throw ArchiveError("malformed TAR PAX record");
        }
        std::uint64_t record_length = 0;
        const auto length_begin = payload.data() + offset;
        const auto length_end = payload.data() + space;
        const auto length_result = std::from_chars(length_begin, length_end, record_length, 10);
        if (length_result.ec != std::errc{} || length_result.ptr != length_end || record_length == 0) {
            throw ArchiveError("malformed TAR PAX record length");
        }
        if (record_length > payload.size() - offset) {
            throw ArchiveError("TAR PAX record exceeds metadata payload");
        }
        const auto record_begin = space + 1U;
        const auto record_end = offset + static_cast<std::size_t>(record_length);
        if (record_end == 0 || payload[record_end - 1U] != '\n') {
            throw ArchiveError("TAR PAX record is not newline terminated");
        }
        const auto equals = payload.find('=', record_begin);
        if (equals != std::string::npos && equals < record_end) {
            const auto key = payload.substr(record_begin, equals - record_begin);
            const auto value = payload.substr(equals + 1U, record_end - equals - 2U);
            if (key == "path") {
                parsed.path = value;
            }
        }
        offset = record_end;
    }
    return parsed;
}

// Purpose: Parse a GNU long-name metadata payload.
// Inputs: `payload` contains a NUL-terminated or newline-terminated path.
// Outputs: Returns the path for the next file entry.
std::string parse_gnu_long_name(std::string payload) {
    while (!payload.empty() && (payload.back() == '\0' || payload.back() == '\n')) {
        payload.pop_back();
    }
    if (payload.empty()) {
        throw ArchiveError("TAR long-name metadata is empty");
    }
    return payload;
}

// Purpose: Scan a TAR stream and validate metadata before any extraction writes occur.
// Inputs: `input` is positioned at the TAR start, `source_label` names diagnostics, and `seekable` controls payload skipping strategy.
// Outputs: Returns validated entry metadata and total file bytes; throws on malformed or unsafe TAR records.
TarScanResult scan_tar_stream(std::istream& input, const std::string& source_label, bool seekable) {
    (void)source_label;
    TarScanResult result;
    PendingTarExtensions pending;
    for (;;) {
        std::array<char, kTarBlockSize> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        if (input.gcount() == 0) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            throw ArchiveError("TAR header is truncated");
        }
        if (is_zero_block(header)) {
            break;
        }
        validate_tar_checksum(header);
        const auto size = parse_tar_number(header, 124, 12, "size");
        const auto typeflag = header[156] == '\0' ? '0' : header[156];
        std::uint64_t payload_offset = 0;
        if (seekable) {
            const auto position = input.tellg();
            if (position < 0) {
                throw ArchiveError("TAR payload offset is invalid");
            }
            payload_offset = static_cast<std::uint64_t>(position);
        }

        if (typeflag == 'x') {
            pending = parse_pax_payload(read_tar_metadata_payload(input, size));
            continue;
        }
        if (typeflag == 'g') {
            static_cast<void>(read_tar_metadata_payload(input, size));
            continue;
        }
        if (typeflag == 'L') {
            pending.path = parse_gnu_long_name(read_tar_metadata_payload(input, size));
            continue;
        }
        const auto entry_path = tar_entry_path(header, pending);
        if (typeflag == '0' || typeflag == '5') {
            if (result.entries.size() >= kMaxArchiveEntries) {
                throw ArchiveError("TAR entry count exceeds SuperZip resource limit");
            }
            const bool directory = typeflag == '5';
            if (!directory) {
                result.total_file_bytes = checked_add_tar_bytes(result.total_file_bytes, size, "TAR uncompressed size overflows");
            }
            result.entries.push_back(TarEntryMetadata{
                .path = entry_path,
                .directory = directory,
                .size = directory ? 0 : size,
                .payload_offset = payload_offset,
            });
            skip_tar_payload(input, size, seekable);
            continue;
        }
        if (typeflag == '1' || typeflag == '2') {
            throw SecurityError("refusing to extract TAR link entry: " + entry_path);
        }
        if (typeflag == '3' || typeflag == '4' || typeflag == '6') {
            throw SecurityError("refusing to extract TAR device or FIFO entry: " + entry_path);
        }
        throw ArchiveError("unsupported TAR entry type");
    }

    std::vector<ArchivePathValidationEntry> validation_entries;
    validation_entries.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        validation_entries.push_back(ArchivePathValidationEntry{
            .path = entry.path,
            .directory = entry.directory,
        });
    }
    validate_archive_path_set(validation_entries);
    return result;
}

// Purpose: Scan a seekable TAR file and validate metadata before any extraction writes occur.
// Inputs: `archive_path` is the TAR file to scan.
// Outputs: Returns validated entry metadata and total file bytes; throws on malformed or unsafe TAR records.
TarScanResult scan_tar(const std::filesystem::path& archive_path) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open TAR archive: " + archive_path.string());
    }
    return scan_tar_stream(input, archive_path.string(), true);
}

// Purpose: Copy one already-positioned TAR payload from a stream to a verified temporary file.
// Inputs: `input` is positioned at payload start, `entry` supplies size/path, `target` is the final extraction path, and `overwrite` controls replacement.
// Outputs: Publishes the verified file atomically and consumes payload padding, or throws.
void extract_tar_stream_file_payload(
    std::istream& input,
    const TarEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing TAR extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());
    const auto temporary_target = reserve_file_publish_target(target);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("cannot create TAR extraction target: " + temporary_target.file.string());
        }
        std::array<char, kTarIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (input.gcount() != static_cast<std::streamsize>(chunk)) {
                throw ArchiveError("TAR payload is truncated");
            }
            write_bytes(output, buffer.data(), chunk);
            remaining -= chunk;
        }
        const auto padding = tar_padding(entry.size);
        if (padding > 0) {
            discard_stream_bytes(input, padding, "TAR payload padding");
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to flush TAR extraction target: " + temporary_target.file.string());
        }
        commit_verified_file(temporary_target.file, target, overwrite);
        cleanup_file_publish_target(temporary_target);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary_target);
        }
        throw;
    }
}

// Purpose: Verify that a second-pass TAR record matches the first validated scan.
// Inputs: `actual` is metadata read during extraction and `expected` is first-pass validated metadata.
// Outputs: Returns normally when path, kind, and size match; throws on changed or inconsistent streams.
void require_matching_scanned_entry(const TarEntryMetadata& actual, const TarEntryMetadata& expected) {
    if (actual.path != expected.path || actual.directory != expected.directory || actual.size != expected.size) {
        throw ArchiveError("TAR stream changed between validation and extraction passes");
    }
}

// Purpose: Extract a non-seekable TAR stream after a separate validation pass has succeeded.
// Inputs: `input` is positioned at the TAR start, `scanned` is the validated metadata, `destination` is the extraction root, and `overwrite` controls replacement.
// Outputs: Restores entries into `destination` while re-checking second-pass metadata against `scanned`.
void extract_validated_tar_stream(
    std::istream& input,
    const TarScanResult& scanned,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());
    PendingTarExtensions pending;
    std::size_t entry_index = 0;
    for (;;) {
        std::array<char, kTarBlockSize> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        if (input.gcount() == 0) {
            break;
        }
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            throw ArchiveError("TAR header is truncated");
        }
        if (is_zero_block(header)) {
            break;
        }
        validate_tar_checksum(header);
        const auto size = parse_tar_number(header, 124, 12, "size");
        const auto typeflag = header[156] == '\0' ? '0' : header[156];

        if (typeflag == 'x') {
            pending = parse_pax_payload(read_tar_metadata_payload(input, size));
            continue;
        }
        if (typeflag == 'g') {
            static_cast<void>(read_tar_metadata_payload(input, size));
            continue;
        }
        if (typeflag == 'L') {
            pending.path = parse_gnu_long_name(read_tar_metadata_payload(input, size));
            continue;
        }
        const auto entry_path = tar_entry_path(header, pending);
        if (typeflag == '0' || typeflag == '5') {
            const bool directory = typeflag == '5';
            if (entry_index >= scanned.entries.size()) {
                throw ArchiveError("TAR stream contains entries not present during validation");
            }
            const TarEntryMetadata actual{
                .path = entry_path,
                .directory = directory,
                .size = directory ? 0 : size,
                .payload_offset = 0,
            };
            const auto& expected = scanned.entries[entry_index];
            require_matching_scanned_entry(actual, expected);
            progress.set_current(expected.path);
            publish_progress(progress, progress_callback);
            const auto target = safe_join_archive_path(destination, expected.path);
            if (expected.directory) {
                std::filesystem::create_directories(target);
                skip_tar_payload(input, size, false);
                progress.finish_entry();
            } else {
                extract_tar_stream_file_payload(input, expected, target, overwrite);
                progress.add_bytes(expected.size);
                progress.finish_entry();
            }
            ++entry_index;
            continue;
        }
        if (typeflag == '1' || typeflag == '2') {
            throw SecurityError("refusing to extract TAR link entry: " + entry_path);
        }
        if (typeflag == '3' || typeflag == '4' || typeflag == '6') {
            throw SecurityError("refusing to extract TAR device or FIFO entry: " + entry_path);
        }
        throw ArchiveError("unsupported TAR entry type");
    }
    if (entry_index != scanned.entries.size()) {
        throw ArchiveError("TAR stream ended before all validated entries were extracted");
    }
}

// Purpose: Copy one scanned TAR file payload to a verified temporary file.
// Inputs: `input` is the archive stream, `entry` supplies size and offset, and `target` is the final extraction path.
// Outputs: Publishes the verified file atomically or throws; leaves no known temporary payload on failure.
void extract_tar_file_payload(
    std::ifstream& input,
    const TarEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing TAR extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());
    const auto temporary_target = reserve_file_publish_target(target);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("cannot create TAR extraction target: " + temporary_target.file.string());
        }
        input.seekg(static_cast<std::streamoff>(entry.payload_offset), std::ios::beg);
        if (!input) {
            throw ArchiveError("TAR payload offset is invalid");
        }
        std::array<char, kTarIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), static_cast<std::streamsize>(chunk));
            if (input.gcount() != static_cast<std::streamsize>(chunk)) {
                throw ArchiveError("TAR payload is truncated");
            }
            write_bytes(output, buffer.data(), chunk);
            remaining -= chunk;
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to flush TAR extraction target: " + temporary_target.file.string());
        }
        commit_verified_file(temporary_target.file, target, overwrite);
        cleanup_file_publish_target(temporary_target);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary_target);
        }
        throw;
    }
}

// Purpose: Write a TAR stream to any output stream using shared compatibility semantics.
// Inputs: `sources` are existing files/directories, `output` receives TAR bytes, and `progress_callback` receives synchronous snapshots.
// Outputs: Returns uncompressed input byte/entry counts; throws on source/path/write failures.
TarWriteStats write_tar_stream(
    const std::vector<std::filesystem::path>& sources,
    std::ostream& output,
    const ProgressCallback& progress_callback) {
    const auto manifest = build_manifest(sources);
    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.entries.size());

    std::uint64_t ordinal = 0;
    for (const auto& entry : manifest.entries) {
        progress.set_current(entry.archive_path);
        publish_progress(progress, progress_callback);
        write_entry_header(output, entry.archive_path, entry.directory ? '5' : '0', entry.directory ? 0 : entry.size, ordinal++);
        if (!entry.directory) {
            copy_file_to_tar(entry.source_path, output, entry.size);
            write_tar_padding(output, entry.size);
            progress.add_bytes(entry.size);
        }
        progress.finish_entry();
    }
    std::array<char, kTarBlockSize> zero{};
    write_bytes(output, zero.data(), zero.size());
    write_bytes(output, zero.data(), zero.size());
    return TarWriteStats{
        .input_bytes = manifest.total_file_bytes,
        .entries = static_cast<std::uint64_t>(manifest.entries.size()),
    };
}

}  // namespace

OperationStats compress_tar(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    std::ofstream output(output_archive, std::ios::binary);
    if (!output) {
        throw ArchiveError("cannot create TAR archive: " + output_archive.string());
    }
    const auto write_stats = write_tar_stream(sources, output, progress_callback);
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize TAR archive: " + output_archive.string());
    }

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = std::filesystem::file_size(output_archive);
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats compress_tar_gzip(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    GzipOutputStream output(output_archive);
    const auto write_stats = write_tar_stream(sources, output, progress_callback);
    output.close();

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = output.output_bytes();
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats compress_tar_bzip2(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    Bzip2OutputStream output(output_archive);
    const auto write_stats = write_tar_stream(sources, output, progress_callback);
    output.close();

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = output.output_bytes();
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats compress_tar_zstd(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    ZstdOutputStream output(output_archive);
    const auto write_stats = write_tar_stream(sources, output, progress_callback);
    output.close();

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = output.output_bytes();
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_tar(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto scanned = scan_tar(archive_path);
    std::filesystem::create_directories(destination);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open TAR archive: " + archive_path.string());
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
        extract_tar_file_payload(input, entry, target, overwrite);
        progress.add_bytes(entry.size);
        progress.finish_entry();
    }

    OperationStats stats;
    stats.input_bytes = std::filesystem::file_size(archive_path);
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = scanned.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_tar_gzip(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    GzipInputStream scan_input(archive_path);
    const auto scanned = scan_tar_stream(scan_input, archive_path.string(), false);
    scan_input.finish();
    std::filesystem::create_directories(destination);

    GzipInputStream extract_input(archive_path);
    extract_validated_tar_stream(extract_input, scanned, destination, overwrite, progress_callback);
    extract_input.finish();

    OperationStats stats;
    stats.input_bytes = extract_input.input_bytes();
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = scanned.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_tar_bzip2(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    Bzip2InputStream scan_input(archive_path);
    const auto scanned = scan_tar_stream(scan_input, archive_path.string(), false);
    scan_input.finish();
    std::filesystem::create_directories(destination);

    Bzip2InputStream extract_input(archive_path);
    extract_validated_tar_stream(extract_input, scanned, destination, overwrite, progress_callback);
    extract_input.finish();

    OperationStats stats;
    stats.input_bytes = extract_input.input_bytes();
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = scanned.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_tar_xz(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    XzInputStream scan_input(archive_path);
    const auto scanned = scan_tar_stream(scan_input, archive_path.string(), false);
    scan_input.finish();
    std::filesystem::create_directories(destination);

    XzInputStream extract_input(archive_path);
    extract_validated_tar_stream(extract_input, scanned, destination, overwrite, progress_callback);
    extract_input.finish();

    OperationStats stats;
    stats.input_bytes = extract_input.input_bytes();
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = scanned.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_tar_zstd(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    ZstdInputStream scan_input(archive_path);
    const auto scanned = scan_tar_stream(scan_input, archive_path.string(), false);
    scan_input.finish();
    std::filesystem::create_directories(destination);

    ZstdInputStream extract_input(archive_path);
    extract_validated_tar_stream(extract_input, scanned, destination, overwrite, progress_callback);
    extract_input.finish();

    OperationStats stats;
    stats.input_bytes = extract_input.input_bytes();
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = scanned.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
