#include "tar/tar_adapter.hpp"
#include "tar/tar_reader.hpp"
#include "tar/tar_header_fields.hpp"

#include "bzip2/bzip2_stream.hpp"
#include "core/file_manifest.hpp"
#include "core/file_size.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/path_text.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "gzip/gzip_stream.hpp"
#include "lzip/lzip_stream.hpp"
#include "xz/xz_stream.hpp"
#include "zstd/zstd_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace flitzzip {
namespace {

constexpr std::size_t kTarBlockSize = 512U;
constexpr std::size_t kTarIoBufferBytes = 64U * 1024U;
constexpr std::uint64_t kMaxTarMetadataPayloadBytes = 1024U * 1024U;

struct TarHeaderName {
    std::string name;
    std::string prefix;
};

using detail::extract_validated_tar_stream;
using detail::scan_tar_stream;
using detail::TarEntryMetadata;
using detail::TarScanResult;

struct TarWriteStats {
    std::uint64_t input_bytes = 0;
    std::uint64_t entries = 0;
};

struct TarPathOverride {
    std::string bytes;
    bool pax = true;
};

struct PendingTarExtensions {
    std::optional<TarPathOverride> path;
    std::optional<ArchivePathEncoding> encoding;
    std::optional<std::uint64_t> size;
};

struct TarExtensionState {
    PendingTarExtensions global;
    PendingTarExtensions local;
    bool awaiting_entry = false;
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
        throw ArchiveError("cannot open file for TAR archive: " + path_diagnostic_utf8(source));
    }
    std::array<char, kTarIoBufferBytes> buffer{};
    std::uint64_t remaining = expected_size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw ArchiveError("failed to read file for TAR archive: " + path_diagnostic_utf8(source));
        }
        write_bytes(output, buffer.data(), chunk);
        remaining -= chunk;
    }
}

// Purpose: Encode the TAR checksum field after all other header fields are populated.
// Inputs: `header` is one mutable 512-byte TAR header.
// Outputs: Writes six octal digits, NUL and space; the fixed header bounds the checksum below 8^6.
void finalize_tar_checksum(std::array<char, kTarBlockSize>& header) {
    static_assert(kTarBlockSize * 255U < (1U << 18U));
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    std::uint32_t checksum = 0;
    for (const auto byte : header) {
        checksum += static_cast<unsigned char>(byte);
    }
    tar_detail::put_tar_number(std::span(header).subspan<148, 7>(), checksum);
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
// Inputs: `path` is the header path, `typeflag` is the TAR entry type, `size` is payload size, and `mode` is the POSIX
// mode bits. Outputs: Returns a complete checksummed 512-byte header block.
std::array<char, kTarBlockSize> make_tar_header(const std::string& path, char typeflag, std::uint64_t size,
                                                std::uint32_t mode) {
    auto fields = split_ustar_name(path);
    if (!fields) {
        throw ArchiveError("TAR path exceeds USTAR header limits without PAX metadata: " + path);
    }

    std::array<char, kTarBlockSize> header{};
    tar_detail::put_tar_string(std::span(header).subspan<0, 100>(), fields->name);
    tar_detail::put_tar_number(std::span(header).subspan<100, 8>(), mode);
    tar_detail::put_tar_number(std::span(header).subspan<108, 8>(), 0);
    tar_detail::put_tar_number(std::span(header).subspan<116, 8>(), 0);
    tar_detail::put_tar_number(std::span(header).subspan<124, 12>(), size);
    tar_detail::put_tar_number(std::span(header).subspan<136, 12>(), 0);
    header[156] = typeflag;
    tar_detail::put_tar_string(std::span(header).subspan<257, 6>(), "ustar");
    tar_detail::put_tar_string(std::span(header).subspan<263, 2>(), "00");
    tar_detail::put_tar_string(std::span(header).subspan<265, 32>(), "flitzzip");
    tar_detail::put_tar_string(std::span(header).subspan<297, 32>(), "flitzzip");
    tar_detail::put_tar_string(std::span(header).subspan<345, 155>(), fields->prefix);
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
    const bool ascii = std::ranges::all_of(path, [](unsigned char byte) { return byte < 0x80U; });
    if (ascii && split_ustar_name(path).has_value()) {
        return;
    }
    const auto payload = make_pax_record("path", path);
    const auto metadata_path = "PaxHeaders.X/flitzzip-" + std::to_string(ordinal);
    const auto header = make_tar_header(metadata_path, 'x', payload.size(), 0644);
    write_bytes(output, header.data(), header.size());
    write_bytes(output, payload.data(), payload.size());
    write_tar_padding(output, payload.size());
}

// Purpose: Emit a TAR header that may rely on an immediately preceding PAX path override.
// Inputs: `output` is the TAR stream, `path` is the full archive path, `typeflag` and `size` define the entry, and
// `ordinal` labels optional PAX metadata. Outputs: Writes one header and optional PAX metadata.
void write_entry_header(std::ostream& output, const std::string& path, char typeflag, std::uint64_t size,
                        std::uint64_t ordinal) {
    write_pax_path_if_needed(output, path, ordinal);
    const auto header_path = split_ustar_name(path).has_value() ? path : "flitzzip-pax-entry";
    const auto header = make_tar_header(header_path, typeflag, size, typeflag == '5' ? 0755 : 0644);
    write_bytes(output, header.data(), header.size());
}

// Purpose: Write the canonical pair of TAR end-of-archive records for counting and serialization alike.
// Inputs: A writable destination stream.
// Outputs: Emits exactly two zero header blocks or propagates a write failure.
void write_tar_footer(std::ostream& output) {
    const std::array<char, kTarBlockSize> zero{};
    write_bytes(output, zero.data(), zero.size());
    write_bytes(output, zero.data(), zero.size());
}

class TarByteCounter final : public std::streambuf {
  public:
    // Purpose: Account for payload extents without reading, allocating, or copying file content.
    // Inputs: The number of bytes emitted by the corresponding serialization step.
    // Outputs: Adds the count or throws before unsigned overflow.
    void add(std::uint64_t bytes) {
        bytes_ = checked_add_tar_bytes(bytes_, bytes, "TAR serialized size overflows");
    }

    // Purpose: Return the exact count accumulated by header, payload, padding, and footer accounting.
    // Inputs: None.
    // Outputs: Returns serialized byte count without mutating state.
    [[nodiscard]] std::uint64_t bytes() const {
        return bytes_;
    }

  protected:
    // Purpose: Count a serializer write without retaining its bytes.
    // Inputs: Unused source storage and a nonnegative stream write length.
    // Outputs: Returns the accepted length or throws on count overflow.
    std::streamsize xsputn(const char*, std::streamsize count) override {
        if (count <= 0) {
            return 0;
        }
        add(static_cast<std::uint64_t>(count));
        return count;
    }

    // Purpose: Support single-byte serializer writes without a put buffer.
    // Inputs: A character or the stream EOF sentinel.
    // Outputs: Counts real bytes, accepts the sentinel, and propagates count overflow.
    int_type overflow(int_type value) override {
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            add(1U);
        }
        return traits_type::not_eof(value);
    }

  private:
    std::uint64_t bytes_ = 0;
};

// Purpose: Derive exact TAR length using the production metadata serializer, without a payload-reading prepass.
// Inputs: The same immutable source manifest later used to write the archive.
// Outputs: Returns serialized size including PAX, padding, and footer, or throws on metadata/count errors.
std::uint64_t tar_stream_size(const Manifest& manifest) {
    TarByteCounter counter;
    std::ostream metadata(&counter);
    metadata.exceptions(std::ios::badbit | std::ios::failbit);
    std::uint64_t ordinal = 0;
    for (const auto& entry : manifest.entries) {
        write_entry_header(metadata, entry.archive_path, entry.directory ? '5' : '0', entry.directory ? 0 : entry.size,
                           ordinal++);
        if (!entry.directory) {
            counter.add(entry.size);
            write_tar_padding(metadata, entry.size);
        }
    }
    write_tar_footer(metadata);
    return counter.bytes();
}

// Purpose: Detect an all-zero TAR end-of-archive block.
// Inputs: `header` is one 512-byte TAR block.
// Outputs: Returns true when all bytes are zero.
bool is_zero_block(const std::array<char, kTarBlockSize>& header) {
    return std::ranges::all_of(header, [](char ch) { return ch == '\0'; });
}

// Purpose: Distinguish a complete TAR header, supported clean termination and an input failure in both passes.
// Inputs: A readable borrowed stream, writable header storage and optional independently known absolute extent.
// Outputs: Returns true for a nonzero header or false for a zero block/clean EOF; throws before admitting a short
// header, bad stream state or premature physical EOF within a known extent.
bool read_tar_header(std::istream& input, std::array<char, kTarBlockSize>& header,
                     std::optional<std::uint64_t> archive_size = std::nullopt) {
    if (!input.good()) {
        throw ArchiveError("TAR header input is not readable");
    }
    std::optional<std::uint64_t> remaining;
    if (archive_size) {
        const auto position = input.tellg();
        if (position < 0 || static_cast<std::uint64_t>(position) > *archive_size) {
            throw ArchiveError("TAR header has no valid archive-bounded offset");
        }
        remaining = *archive_size - static_cast<std::uint64_t>(position);
    }
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (input.bad() || (input.fail() && !input.eof())) {
        throw ArchiveError("failed to read TAR header");
    }
    if (input.gcount() == 0) {
        if (!input.eof() || remaining.value_or(0U) != 0U) {
            throw ArchiveError("TAR header is truncated before the known archive extent");
        }
        return false;
    }
    if (input.gcount() != static_cast<std::streamsize>(header.size()) || (remaining && *remaining < header.size())) {
        throw ArchiveError("TAR header is truncated");
    }
    return !is_zero_block(header);
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
    bool saw_terminator = false;
    for (const char raw : field) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch == '\0' || ch == ' ') {
            if (saw_digit) {
                saw_terminator = true;
            }
            continue;
        }
        if (saw_terminator || ch < '0' || ch > '7') {
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
std::uint64_t parse_tar_number(const std::array<char, kTarBlockSize>& header, std::size_t offset, std::size_t length,
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

// Purpose: Read the path when no PAX or GNU extension overrides the USTAR fields.
// Inputs: `header` contains the original name and prefix fields; `typeflag` identifies directories.
// Outputs: Returns the combined path, including USTAR prefix-only directories, or rejects an absent name.
std::string tar_entry_path(const std::array<char, kTarBlockSize>& header, char typeflag) {
    const auto name = read_tar_string(header, 0, 100);
    const auto prefix = read_tar_string(header, 345, 155);
    if (name.empty()) {
        if (typeflag == '5' && !prefix.empty()) {
            return prefix;
        }
        throw ArchiveError("TAR entry path is empty");
    }
    return prefix.empty() ? name : prefix + "/" + name;
}

// Purpose: Advance an input stream past a TAR payload and its padding.
// Inputs: `input` is positioned at payload start, `size` is the payload byte count, and `seekable` selects seek or
// discard mode. Outputs: Moves to the next header or throws on invalid seek.
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
        throw ArchiveError("TAR metadata payload exceeds FlitzZip resource limit");
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

// Purpose: Decode an unsigned decimal PAX length or size without accepting signs or trailing data.
// Inputs: `value` is an untrusted bounded byte view. Outputs: Exact value or an archive-format error.
std::uint64_t parse_pax_decimal(std::string_view value) {
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw ArchiveError("malformed or overflowing TAR PAX decimal value");
    }
    return number;
}

// Purpose: Validate one complete PAX record before deriving its key and value views.
// Inputs: Bounded metadata `payload` and the next record `offset`.
// Outputs: Views into the payload and an advanced offset; throws for malformed framing.
std::pair<std::string_view, std::string_view> read_pax_record(std::string_view payload, std::size_t& offset) {
    const auto space = payload.find(' ', offset);
    if (space == std::string_view::npos) {
        throw ArchiveError("malformed TAR PAX record");
    }
    const auto length = parse_pax_decimal(payload.substr(offset, space - offset));
    const auto prefix_bytes = space - offset + 1U;
    if (length < prefix_bytes || length - prefix_bytes < 3U || length > payload.size() - offset) {
        throw ArchiveError("TAR PAX record length does not bound a key, separator and newline");
    }
    const auto record_end = offset + static_cast<std::size_t>(length);
    if (payload[record_end - 1U] != '\n') {
        throw ArchiveError("TAR PAX record is not newline terminated");
    }
    const auto body = payload.substr(space + 1U, record_end - space - 2U);
    const auto equals = body.find('=');
    if (equals == std::string_view::npos || equals == 0 ||
        body.substr(0, equals).find('\0') != std::string_view::npos) {
        throw ArchiveError("malformed TAR PAX keyword");
    }
    offset = record_end;
    return {body.substr(0, equals), body.substr(equals + 1U)};
}

// Purpose: Apply the extraction-relevant PAX fields without silently ignoring payload transformations.
// Inputs: Bounded metadata, retained local/global attributes and the global-header flag.
// Outputs: Updates attributes; rejects malformed, duplicate, unsupported sparse or empty local fields.
void parse_pax_payload(std::string_view payload, PendingTarExtensions& attributes, bool global) {
    std::array<bool, 3> seen{};
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto [key, value] = read_pax_record(payload, offset);
        if (key == "GNU.sparse" || key.starts_with("GNU.sparse.") || key == "SCHILY.realsize" ||
            key == "SUN.holesdata") {
            throw ArchiveError("sparse TAR PAX payloads are not supported");
        }
        const auto field = key == "path" ? 0U : key == "size" ? 1U : key == "hdrcharset" ? 2U : 3U;
        if (field == seen.size()) {
            continue;
        }
        if (seen[field]) {
            throw ArchiveError("TAR PAX metadata contains duplicate extraction fields");
        }
        seen[field] = true;
        if (value.empty() && !global) {
            throw ArchiveError("empty TAR PAX extraction field is not supported");
        }
        if (field == 0U) {
            if (value.size() > kMaxArchivePathBytes) {
                throw ArchiveError("TAR PAX path exceeds FlitzZip path limits");
            }
            attributes.path = value.empty() ? std::nullopt : std::optional(TarPathOverride{std::string(value), true});
        } else if (field == 1U) {
            attributes.size = value.empty() ? std::nullopt : std::optional(parse_pax_decimal(value));
        } else if (value.empty()) {
            attributes.encoding.reset();
        } else if (value == "BINARY") {
            attributes.encoding = ArchivePathEncoding::HostCodePage;
        } else if (value == "ISO-IR 10646 2000 UTF-8") {
            attributes.encoding = ArchivePathEncoding::Utf8;
        } else {
            throw ArchiveError("unsupported TAR PAX header character set");
        }
    }
}

// Purpose: Parse a GNU long-name metadata payload.
// Inputs: `payload` contains a length-bounded path with optional trailing NUL terminators.
// Outputs: Removes only NUL padding; preserves name bytes for the shared path-safety validation.
std::string parse_gnu_long_name(std::string payload) {
    while (!payload.empty() && payload.back() == '\0') {
        payload.pop_back();
    }
    if (payload.empty()) {
        throw ArchiveError("TAR long-name metadata is empty");
    }
    if (payload.size() > kMaxArchivePathBytes) {
        throw ArchiveError("TAR long-name metadata exceeds FlitzZip path limits");
    }
    return payload;
}

// Purpose: Consume extension records identically during metadata validation and streamed extraction.
// Inputs: Current header/type, payload stream, persistent extension state and cumulative metadata bytes.
// Outputs: Returns whether a bounded extension was consumed; throws before accepting invalid metadata.
bool consume_tar_extension(std::istream& input, const std::array<char, kTarBlockSize>& header, char typeflag,
                           TarExtensionState& extensions, std::uint64_t& metadata_bytes) {
    if (typeflag != 'x' && typeflag != 'g' && typeflag != 'L') {
        return false;
    }
    const auto size = parse_tar_number(header, 124, 12, "metadata size");
    metadata_bytes = checked_add_archive_path_metadata_bytes(metadata_bytes, size, "TAR extension metadata");
    auto payload = read_tar_metadata_payload(input, size);
    if (typeflag == 'L') {
        extensions.local.path = TarPathOverride{parse_gnu_long_name(std::move(payload)), false};
    } else {
        parse_pax_payload(payload, typeflag == 'g' ? extensions.global : extensions.local, typeflag == 'g');
    }
    extensions.awaiting_entry = extensions.awaiting_entry || typeflag != 'g';
    return true;
}

// Purpose: Resolve PAX/GNU precedence before trusting the payload extent or publishing a file.
// Inputs: A checksummed ordinary header, its type and retained local/global extension state.
// Outputs: Effective metadata with the wire payload size; consumes every one-entry override.
TarEntryMetadata resolve_tar_entry(const std::array<char, kTarBlockSize>& header, char typeflag,
                                   TarExtensionState& extensions) {
    TarEntryMetadata entry;
    const auto& path = extensions.local.path ? extensions.local.path : extensions.global.path;
    entry.path = path ? path->bytes : tar_entry_path(header, typeflag);
    if (path && path->pax) {
        entry.encoding =
            extensions.local.encoding.value_or(extensions.global.encoding.value_or(ArchivePathEncoding::Utf8));
    }
    // An overridden USTAR field is ignored, including an unrepresentable original value.
    const auto size = extensions.local.size ? extensions.local.size : extensions.global.size;
    entry.size = size ? *size : parse_tar_number(header, 124, 12, "size");
    entry.directory = typeflag == '5';
    extensions.local = {};
    extensions.awaiting_entry = false;
    return entry;
}

// Purpose: Prove a seekable payload's padded extent lies inside the pinned archive.
// Inputs: Payload-positioned stream, effective byte size and known archive extent.
// Outputs: Absolute payload offset or an error before seeking beyond valid bytes.
std::uint64_t bounded_tar_payload_offset(std::istream& input, std::uint64_t size,
                                         std::optional<std::uint64_t> archive_size) {
    const auto position = input.tellg();
    if (position < 0 || !archive_size.has_value()) {
        throw ArchiveError("seekable TAR payload has no valid offset or archive-size bound");
    }
    const auto offset = static_cast<std::uint64_t>(position);
    const auto padded = checked_add_tar_bytes(size, tar_padding(size), "TAR payload extent overflows");
    if (offset > *archive_size || padded > *archive_size - offset) {
        throw ArchiveError("TAR payload extent extends past the end of the archive");
    }
    return offset;
}

}  // namespace

// Purpose: Scan a TAR stream and validate metadata before any extraction writes occur.
// Inputs: `input` is positioned at the TAR start, `seekable` controls payload
// skipping, and `archive_size` bounds seekable extents. Outputs: Returns validated metadata or throws on malformed,
// unsafe, or resource-exhaustive records.
detail::TarScanResult detail::scan_tar_stream(std::istream& input, bool seekable,
                                              std::optional<std::uint64_t> archive_size) {
    if (seekable && !archive_size) {
        throw ArchiveError("seekable TAR scan requires a known archive extent");
    }
    TarScanResult result;
    TarExtensionState extensions;
    for (;;) {
        std::array<char, kTarBlockSize> header{};
        if (!read_tar_header(input, header, seekable ? archive_size : std::nullopt)) {
            break;
        }
        validate_tar_checksum(header);
        const auto typeflag = header[156] == '\0' ? '0' : header[156];
        if (consume_tar_extension(input, header, typeflag, extensions, result.path_metadata_bytes)) {
            continue;
        }
        auto entry = resolve_tar_entry(header, typeflag, extensions);
        const auto size = entry.size;
        entry.payload_offset = seekable ? bounded_tar_payload_offset(input, size, archive_size) : 0;
        if (typeflag == '0' || typeflag == '5') {
            if (result.entries.size() >= kMaxArchiveEntries) {
                throw ArchiveError("TAR entry count exceeds FlitzZip resource limit");
            }
            result.path_metadata_bytes = checked_add_archive_path_metadata_bytes(
                result.path_metadata_bytes, entry.path.size(), "TAR retained entry paths");
            if (!entry.directory) {
                result.total_file_bytes =
                    checked_add_extracted_output_bytes(result.total_file_bytes, size, "TAR uncompressed payload");
            } else {
                entry.size = 0;
            }
            result.entries.push_back(std::move(entry));
            skip_tar_payload(input, size, seekable);
            continue;
        }
        if (typeflag == '1' || typeflag == '2') {
            throw SecurityError("refusing to extract TAR link entry: " + entry.path);
        }
        if (typeflag == '3' || typeflag == '4' || typeflag == '6') {
            throw SecurityError("refusing to extract TAR device or FIFO entry: " + entry.path);
        }
        throw ArchiveError("unsupported TAR entry type");
    }

    if (extensions.awaiting_entry) {
        throw ArchiveError("TAR stream ends before the entry described by its extension metadata");
    }
    // Compare all retained paths together before any extraction can publish output.
    std::vector<ArchivePathValidationEntry> validation_entries;
    validation_entries.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        result.path_metadata_bytes = checked_add_archive_path_metadata_bytes(
            result.path_metadata_bytes, entry.path.size(), "TAR path validation metadata");
        validation_entries.push_back(ArchivePathValidationEntry{
            .path = entry.path,
            .directory = entry.directory,
            .encoding = entry.encoding,
        });
    }
    validate_archive_path_set(validation_entries);
    return result;
}

namespace {

// Purpose: Scan a seekable TAR file and validate metadata before any extraction writes occur.
// Inputs: `archive_path` is the TAR file to scan.
// Outputs: Returns validated entry metadata and total file bytes; throws on malformed or unsafe TAR records.
TarScanResult scan_tar(const std::filesystem::path& archive_path) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open TAR archive: " + path_diagnostic_utf8(archive_path));
    }
    return scan_tar_stream(input, true, regular_file_size(archive_path));
}

// Purpose: Copy one already-positioned TAR payload from a stream to a verified temporary file.
// Inputs: `input` is positioned at payload start, `entry` supplies size/path, `target` is the final extraction path,
// and `overwrite` controls replacement. Outputs: Publishes the verified file atomically and consumes payload padding,
// or throws.
void extract_tar_stream_file_payload(std::istream& input, const TarEntryMetadata& entry,
                                     const std::filesystem::path& target, bool overwrite) {
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing TAR extraction target: " + path_diagnostic_utf8(target));
    }
    const auto temporary_target = reserve_file_publish_target(target);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("cannot create TAR extraction target: " + path_diagnostic_utf8(temporary_target.file));
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
            throw ArchiveError("failed to flush TAR extraction target: " + path_diagnostic_utf8(temporary_target.file));
        }
        commit_verified_file(temporary_target, target, overwrite);
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
    if (actual.path != expected.path || actual.encoding != expected.encoding ||
        actual.directory != expected.directory || actual.size != expected.size) {
        throw ArchiveError("TAR stream changed between validation and extraction passes");
    }
}

}  // namespace

// Purpose: Extract a non-seekable TAR stream after a separate validation pass has succeeded.
// Inputs: `input` is positioned at the TAR start, `scanned` is the validated metadata, `destination` is the extraction
// root, and `overwrite` controls replacement. Outputs: Restores entries into `destination` while re-checking
// second-pass metadata against `scanned`.
void detail::extract_validated_tar_stream(std::istream& input, const TarScanResult& scanned,
                                          const std::filesystem::path& destination, bool overwrite,
                                          const ProgressCallback& progress_callback) {
    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());
    TarExtensionState extensions;
    std::uint64_t metadata_bytes = 0;
    std::size_t entry_index = 0;
    for (;;) {
        std::array<char, kTarBlockSize> header{};
        if (!read_tar_header(input, header)) {
            break;
        }
        validate_tar_checksum(header);
        const auto typeflag = header[156] == '\0' ? '0' : header[156];
        if (consume_tar_extension(input, header, typeflag, extensions, metadata_bytes)) {
            continue;
        }
        auto actual = resolve_tar_entry(header, typeflag, extensions);
        const auto size = actual.size;
        if (typeflag == '0' || typeflag == '5') {
            if (entry_index >= scanned.entries.size()) {
                throw ArchiveError("TAR stream contains entries not present during validation");
            }
            actual.size = actual.directory ? 0 : size;
            const auto& expected = scanned.entries[entry_index];
            require_matching_scanned_entry(actual, expected);
            progress.set_current(expected.path);
            publish_progress(progress, progress_callback);
            const auto target = safe_join_archive_path(destination, expected.path, expected.encoding);
            if (expected.directory) {
                create_verified_directories(target);
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
            throw SecurityError("refusing to extract TAR link entry: " + actual.path);
        }
        if (typeflag == '3' || typeflag == '4' || typeflag == '6') {
            throw SecurityError("refusing to extract TAR device or FIFO entry: " + actual.path);
        }
        throw ArchiveError("unsupported TAR entry type");
    }
    if (extensions.awaiting_entry || entry_index != scanned.entries.size()) {
        throw ArchiveError("TAR stream ended before all validated entries were extracted");
    }
}

namespace {

// Purpose: Copy one scanned TAR file payload to a verified temporary file.
// Inputs: `input` is the archive stream, `entry` supplies size and offset, and `target` is the final extraction path.
// Outputs: Publishes the verified file atomically or throws; leaves no known temporary payload on failure.
void extract_tar_file_payload(std::ifstream& input, const TarEntryMetadata& entry, const std::filesystem::path& target,
                              bool overwrite) {
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing TAR extraction target: " + path_diagnostic_utf8(target));
    }
    const auto temporary_target = reserve_file_publish_target(target);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("cannot create TAR extraction target: " + path_diagnostic_utf8(temporary_target.file));
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
            throw ArchiveError("failed to flush TAR extraction target: " + path_diagnostic_utf8(temporary_target.file));
        }
        commit_verified_file(temporary_target, target, overwrite);
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
// Inputs: `manifest` fixes source entries, `output` receives TAR bytes, and `progress_callback` receives
// synchronous snapshots. Outputs: Returns uncompressed input byte/entry counts; throws on source/path/write failures.
TarWriteStats write_tar_stream(const Manifest& manifest, std::ostream& output,
                               const ProgressCallback& progress_callback) {
    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.entries.size());

    std::uint64_t ordinal = 0;
    for (const auto& entry : manifest.entries) {
        progress.set_current(entry.archive_path);
        publish_progress(progress, progress_callback);
        write_entry_header(output, entry.archive_path, entry.directory ? '5' : '0', entry.directory ? 0 : entry.size,
                           ordinal++);
        if (!entry.directory) {
            const auto source_lock = lock_manifest_source(entry);
            copy_file_to_tar(entry.source_path, output, entry.size);
            write_tar_padding(output, entry.size);
            progress.add_bytes(entry.size);
        }
        progress.finish_entry();
    }
    write_tar_footer(output);
    return TarWriteStats{
        .input_bytes = manifest.total_file_bytes,
        .entries = static_cast<std::uint64_t>(manifest.entries.size()),
    };
}

}  // namespace

// Purpose: Create a standards-compatible TAR archive from files and directories.
// Inputs: `sources` are existing paths, `output_archive` is the TAR target, and `progress_callback` receives updates.
// Outputs: Snapshots sources before creating output, then returns telemetry or throws on source/path/write failures.
OperationStats compress_tar(const std::vector<std::filesystem::path>& sources,
                            const std::filesystem::path& output_archive, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    FilePublishTransaction publication(output_archive);
    std::ofstream output(publication.staging_path(), std::ios::binary);
    if (!output) {
        throw ArchiveError("cannot create TAR archive: " + path_diagnostic_utf8(output_archive));
    }
    const auto write_stats = write_tar_stream(manifest, output, progress_callback);
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize TAR archive: " + path_diagnostic_utf8(output_archive));
    }
    publication.commit(true);

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = regular_file_size(output_archive);
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Create a TAR.GZ stream from files/directories with bounded in-process Gzip compression.
// Inputs: `sources`, `output_archive`, `compression_level`, and `progress_callback` define the archive run.
// Outputs: Snapshots sources before creating output, then returns telemetry or throws on source/TAR/Gzip failures.
OperationStats compress_tar_gzip(const std::vector<std::filesystem::path>& sources,
                                 const std::filesystem::path& output_archive, int compression_level,
                                 const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    FilePublishTransaction publication(output_archive);
    GzipOutputStream output(publication.staging_path(), compression_level);
    const auto write_stats = write_tar_stream(manifest, output, progress_callback);
    output.close();
    const auto output_bytes = output.output_bytes();
    publication.commit(true);

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = output_bytes;
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Create a TAR.BZ2 stream from files/directories with bounded in-process Bzip2 compression.
// Inputs: `sources`, `output_archive`, `compression_level`, and `progress_callback` define the archive run.
// Outputs: Snapshots sources before creating output, then returns telemetry or throws on source/TAR/Bzip2 failures.
OperationStats compress_tar_bzip2(const std::vector<std::filesystem::path>& sources,
                                  const std::filesystem::path& output_archive, int compression_level,
                                  const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    FilePublishTransaction publication(output_archive);
    Bzip2OutputStream output(publication.staging_path(), compression_level);
    const auto write_stats = write_tar_stream(manifest, output, progress_callback);
    output.close();
    const auto output_bytes = output.output_bytes();
    publication.commit(true);

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = output_bytes;
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Create a TAR.ZST stream from files/directories with bounded app-local Zstandard compression.
// Inputs: `sources`, `output_archive`, `compression_level`, and `progress_callback` define the archive run.
// Outputs: Sizes codec workspace from exact serialized bytes, writes the archive, and returns telemetry or throws.
OperationStats compress_tar_zstd(const std::vector<std::filesystem::path>& sources,
                                 const std::filesystem::path& output_archive, int compression_level,
                                 const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    const auto serialized_size = tar_stream_size(manifest);
    FilePublishTransaction publication(output_archive);
    ZstdOutputStream output(publication.staging_path(), compression_level, serialized_size);
    const auto write_stats = write_tar_stream(manifest, output, progress_callback);
    output.close();
    const auto output_bytes = output.output_bytes();
    publication.commit(true);

    OperationStats stats;
    stats.input_bytes = write_stats.input_bytes;
    stats.output_bytes = output_bytes;
    stats.workers = output.compression_workers();
    stats.entries = write_stats.entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Extract a validated TAR archive into a destination directory.
// Inputs: `archive_path`, `destination`, `overwrite`, and `progress_callback` define the extraction run.
// Outputs: Publishes validated files/directories and returns telemetry, or throws on unsafe or malformed entries.
OperationStats extract_tar(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    const auto scanned = scan_tar(archive_source.path());
    create_verified_directories(destination);

    std::ifstream input(archive_source.path(), std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open TAR archive: " + path_diagnostic_utf8(archive_path));
    }
    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());

    for (const auto& entry : scanned.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path, entry.encoding);
        if (entry.directory) {
            create_verified_directories(target);
            progress.finish_entry();
            continue;
        }
        extract_tar_file_payload(input, entry, target, overwrite);
        progress.add_bytes(entry.size);
        progress.finish_entry();
    }

    OperationStats stats;
    stats.input_bytes = archive_source.size();
    stats.output_bytes = scanned.total_file_bytes;
    stats.entries = scanned.entries.size();
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Extract a validated TAR.GZ stream using the in-process Gzip reader.
// Inputs: `archive_path`, `destination`, `overwrite`, and `progress_callback` define the extraction run.
// Outputs: Publishes validated files/directories and returns telemetry, or throws on Gzip/TAR failures.
OperationStats extract_tar_gzip(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    GzipInputStream scan_input(archive_source.path());
    const auto scanned = scan_tar_stream(scan_input, false);
    scan_input.finish();
    create_verified_directories(destination);

    GzipInputStream extract_input(archive_source.path());
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

// Purpose: Extract a validated TAR.BZ2 stream using the in-process Bzip2 reader.
// Inputs: `archive_path`, `destination`, `overwrite`, and `progress_callback` define the extraction run.
// Outputs: Publishes validated files/directories and returns telemetry, or throws on Bzip2/TAR failures.
OperationStats extract_tar_bzip2(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                 bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    Bzip2InputStream scan_input(archive_source.path());
    const auto scanned = scan_tar_stream(scan_input, false);
    scan_input.finish();
    create_verified_directories(destination);

    Bzip2InputStream extract_input(archive_source.path());
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

// Purpose: Extract an XZ-compressed TAR stream with two-pass TAR metadata validation.
// Inputs: `archive_path` is the compressed TAR source, `destination` is the output root, `overwrite` controls
// replacement, and `progress_callback` receives synchronous progress snapshots. Outputs: Returns extraction telemetry;
// throws for malformed XZ/TAR data, unsafe paths, refused overwrite, or publication failures.
OperationStats extract_tar_xz(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                              bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    XzInputStream scan_input(archive_source.path());
    const auto scanned = scan_tar_stream(scan_input, false);
    scan_input.finish();
    create_verified_directories(destination);

    XzInputStream extract_input(archive_source.path());
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

// Purpose: Extract a lzip-compressed TAR stream with two-pass TAR metadata validation.
// Inputs: `archive_path` is the compressed TAR source, `destination` is the output root, `overwrite` controls
// replacement, and `progress_callback` receives synchronous progress snapshots. Outputs: Returns extraction telemetry;
// throws for malformed lzip/TAR data, unsafe paths, refused overwrite, or publication failures.
OperationStats extract_tar_lzip(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    LzipInputStream scan_input(archive_source.path());
    const auto scanned = scan_tar_stream(scan_input, false);
    scan_input.finish();
    create_verified_directories(destination);

    LzipInputStream extract_input(archive_source.path());
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

// Purpose: Extract a validated TAR.ZST stream using the app-local Zstandard reader.
// Inputs: `archive_path`, `destination`, `overwrite`, and `progress_callback` define the extraction run.
// Outputs: Publishes validated files/directories and returns telemetry, or throws on Zstandard/TAR failures.
OperationStats extract_tar_zstd(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    ZstdInputStream scan_input(archive_source.path());
    const auto scanned = scan_tar_stream(scan_input, false);
    scan_input.finish();
    create_verified_directories(destination);

    ZstdInputStream extract_input(archive_source.path());
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

}  // namespace flitzzip
