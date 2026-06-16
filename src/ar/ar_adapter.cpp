#include "ar/ar_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace superzip {
namespace {

constexpr std::string_view kArGlobalMagic = "!<arch>\n";
constexpr std::string_view kArMemberMagic = "`\n";
constexpr std::size_t kArHeaderBytes = 60U;
constexpr std::size_t kArNameBytes = 16U;
constexpr std::size_t kArIoBufferBytes = 64U * 1024U;
constexpr std::uint64_t kMaxArNameBytes = 64U * 1024U;
constexpr std::uint64_t kMaxArStringTableBytes = 16U * 1024U * 1024U;
constexpr std::uint64_t kMaxArDecimalField = 9'999'999'999ULL;

struct ArRawHeader {
    std::string name;
    std::uint64_t size = 0;
};

struct ArEntryMetadata {
    std::string path;
    std::uint64_t size = 0;
    std::uint64_t payload_offset = 0;
};

struct ArScanResult {
    std::vector<ArEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
};

// Purpose: Add two AR byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_ar_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Return the single byte of AR padding required after odd-sized members.
// Inputs: `size` is an AR member byte count.
// Outputs: Returns zero or one.
std::uint64_t ar_padding(std::uint64_t size) {
    return size % 2U;
}

// Purpose: Write exactly one byte range to an AR output stream.
// Inputs: `output` is the destination stream and `bytes` is the payload to append.
// Outputs: Appends all bytes or throws `ArchiveError` on write failure.
void write_ar_bytes(std::ostream& output, const char* bytes, std::size_t size) {
    output.write(bytes, static_cast<std::streamsize>(size));
    if (!output) {
        throw ArchiveError("failed to write AR archive");
    }
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

// Purpose: Seek forward by an exact byte count in an AR file stream.
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

// Purpose: Verify that a declared AR member range is fully contained in the archive file.
// Inputs: `member_start` is the current payload offset, `member_size` is the declared bytes, `archive_size` is the file length, and `label` names diagnostics.
// Outputs: Throws before a later seek/read can move past EOF.
void validate_member_extent(
    std::uint64_t member_start,
    std::uint64_t member_size,
    std::uint64_t archive_size,
    const char* label) {
    const auto payload_end = checked_add_ar_bytes(member_start, member_size, "AR member extent overflow");
    const auto padded_end = checked_add_ar_bytes(payload_end, ar_padding(member_size), "AR padded member extent overflow");
    if (padded_end > archive_size) {
        throw ArchiveError(std::string(label) + " extends past the end of the AR archive");
    }
}

// Purpose: Trim right-side spaces from a fixed-width AR field.
// Inputs: `value` is a fixed-width ASCII field.
// Outputs: Returns the field without trailing spaces.
std::string trim_right_spaces(std::string value) {
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

// Purpose: Parse a decimal AR header field.
// Inputs: `field` is a fixed-width ASCII decimal field and `name` labels diagnostics.
// Outputs: Returns the decoded value or throws on malformed input.
std::uint64_t parse_decimal_field(std::string_view field, const char* name) {
    std::uint64_t value = 0;
    bool saw_digit = false;
    for (const unsigned char ch : field) {
        if (ch == ' ') {
            continue;
        }
        if (!std::isdigit(ch)) {
            throw ArchiveError(std::string("malformed AR decimal field: ") + name);
        }
        saw_digit = true;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw ArchiveError(std::string("AR decimal field overflows: ") + name);
        }
        value = value * 10U + digit;
    }
    if (!saw_digit) {
        throw ArchiveError(std::string("empty AR decimal field: ") + name);
    }
    return value;
}

// Purpose: Write one fixed-width AR header field.
// Inputs: `header` is the mutable header, `offset`/`width` select the field, and `value` is ASCII metadata.
// Outputs: Copies the value and leaves remaining bytes as spaces; throws when the value does not fit.
void put_field(std::array<char, kArHeaderBytes>& header, std::size_t offset, std::size_t width, std::string_view value) {
    if (value.size() > width) {
        throw ArchiveError("AR header field does not fit");
    }
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Parse one AR member header.
// Inputs: `input` is positioned at a member header.
// Outputs: Returns raw header fields, or throws on malformed/truncated headers.
ArRawHeader read_ar_header(std::istream& input) {
    std::array<char, kArHeaderBytes> header{};
    read_exact(input, header.data(), header.size(), "AR member header");
    if (std::string_view(header.data() + 58, 2U) != kArMemberMagic) {
        throw ArchiveError("malformed AR member terminator");
    }
    return ArRawHeader{
        .name = trim_right_spaces(std::string(header.data(), kArNameBytes)),
        .size = parse_decimal_field(std::string_view(header.data() + 48, 10U), "size"),
    };
}

// Purpose: Create one AR member header using a BSD long name payload prefix.
// Inputs: `path` is the archive entry path and `payload_size` is the source file size.
// Outputs: Returns a 60-byte header; throws if the AR decimal size field cannot represent the member.
std::array<char, kArHeaderBytes> make_bsd_ar_header(std::string_view path, std::uint64_t payload_size) {
    if (path.size() > kMaxArNameBytes) {
        throw ArchiveError("AR entry name is too long");
    }
    const auto stored_size = checked_add_ar_bytes(
        payload_size,
        static_cast<std::uint64_t>(path.size()),
        "AR member size overflow");
    if (stored_size > kMaxArDecimalField) {
        throw ArchiveError("AR member is too large for the portable decimal header");
    }
    std::array<char, kArHeaderBytes> header{};
    std::fill(header.begin(), header.end(), ' ');
    put_field(header, 0, 16, "#1/" + std::to_string(path.size()));
    put_field(header, 16, 12, "0");
    put_field(header, 28, 6, "0");
    put_field(header, 34, 6, "0");
    put_field(header, 40, 8, "100644");
    put_field(header, 48, 10, std::to_string(stored_size));
    header[58] = '`';
    header[59] = '\n';
    return header;
}

// Purpose: Copy a bounded file payload into an AR output stream.
// Inputs: `source` is a regular file path, `output` is the archive stream, and `expected_size` bounds copied bytes.
// Outputs: Writes exactly `expected_size` bytes or throws on read/write failure.
void copy_file_to_ar(const std::filesystem::path& source, std::ostream& output, std::uint64_t expected_size) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open file for AR archive: " + source.string());
    }
    std::array<char, kArIoBufferBytes> buffer{};
    std::uint64_t remaining = expected_size;
    while (remaining > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk)) {
            throw ArchiveError("failed to read file for AR archive: " + source.string());
        }
        write_ar_bytes(output, buffer.data(), chunk);
        remaining -= chunk;
    }
}

// Purpose: Return whether an AR member name is archive metadata rather than an extractable file.
// Inputs: `raw_name` is the header name after trimming spaces.
// Outputs: Returns true for symbol tables and GNU/BSD metadata members.
bool is_metadata_member(std::string_view raw_name) {
    return raw_name == "/" ||
        raw_name == "/SYM64/" ||
        raw_name == "__.SYMDEF" ||
        raw_name == "__.SYMDEF SORTED";
}

// Purpose: Parse an unsigned decimal suffix after a prefix.
// Inputs: `value` is the complete string, `prefix` is the expected leading text, and `label` names diagnostics.
// Outputs: Returns the suffix value or throws when the suffix is malformed.
std::uint64_t parse_prefixed_decimal(std::string_view value, std::string_view prefix, const char* label) {
    if (!value.starts_with(prefix) || value.size() == prefix.size()) {
        throw ArchiveError(std::string("malformed AR ") + label);
    }
    std::uint64_t result = 0;
    for (std::size_t i = prefix.size(); i < value.size(); ++i) {
        const auto ch = static_cast<unsigned char>(value[i]);
        if (!std::isdigit(ch)) {
            throw ArchiveError(std::string("malformed AR ") + label);
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw ArchiveError(std::string("AR ") + label + " overflows");
        }
        result = result * 10U + digit;
    }
    return result;
}

// Purpose: Resolve a GNU string-table name reference.
// Inputs: `string_table` contains the GNU `//` member and `offset` is the `/123` reference.
// Outputs: Returns the referenced filename or throws on invalid table data.
std::string resolve_gnu_long_name(const std::string& string_table, std::uint64_t offset) {
    if (offset >= string_table.size()) {
        throw ArchiveError("AR long-name offset is outside the GNU string table");
    }
    const auto start = static_cast<std::size_t>(offset);
    auto end = string_table.find('\n', start);
    if (end == std::string::npos) {
        end = string_table.size();
    }
    auto name = string_table.substr(start, end - start);
    if (!name.empty() && name.back() == '/') {
        name.pop_back();
    }
    if (name.empty()) {
        throw ArchiveError("AR GNU long-name entry is empty");
    }
    return name;
}

// Purpose: Parse one AR member name and adjust payload metadata for BSD long names.
// Inputs: `input` is positioned after the fixed header, `raw` is the parsed header, and `string_table` contains optional GNU long names.
// Outputs: Returns an archive path and payload size/offset, or empty when the member is metadata.
std::optional<ArEntryMetadata> parse_member_name_and_payload(
    std::ifstream& input,
    const ArRawHeader& raw,
    const std::string& string_table) {
    const auto payload_start_pos = input.tellg();
    if (payload_start_pos == std::istream::pos_type(-1)) {
        throw ArchiveError("failed to read AR payload offset");
    }
    auto payload_offset = static_cast<std::uint64_t>(payload_start_pos);
    auto payload_size = raw.size;

    if (raw.name == "//" || is_metadata_member(raw.name)) {
        return std::nullopt;
    }

    std::string path;
    if (raw.name.starts_with("#1/")) {
        const auto name_size = parse_prefixed_decimal(raw.name, "#1/", "BSD name length");
        if (name_size == 0 || name_size > kMaxArNameBytes || name_size > payload_size) {
            throw ArchiveError("AR BSD long-name size is invalid");
        }
        std::string name(static_cast<std::size_t>(name_size), '\0');
        read_exact(input, name.data(), name.size(), "AR BSD long name");
        payload_offset = checked_add_ar_bytes(payload_offset, name_size, "AR BSD payload offset overflow");
        payload_size -= name_size;
        path = std::move(name);
    } else if (raw.name.starts_with("/") && raw.name.size() > 1U) {
        const auto offset = parse_prefixed_decimal(raw.name, "/", "GNU long-name offset");
        path = resolve_gnu_long_name(string_table, offset);
    } else {
        path = raw.name;
        if (!path.empty() && path.back() == '/') {
            path.pop_back();
        }
    }

    if (path.empty()) {
        throw ArchiveError("AR member name is empty");
    }
    return ArEntryMetadata{
        .path = std::move(path),
        .size = payload_size,
        .payload_offset = payload_offset,
    };
}

// Purpose: Read a bounded GNU string-table member.
// Inputs: `input` is positioned at the table payload and `size` is the table byte count.
// Outputs: Returns table bytes or throws when the metadata is unreasonably large/truncated.
std::string read_string_table(std::ifstream& input, std::uint64_t size) {
    if (size > kMaxArStringTableBytes) {
        throw ArchiveError("AR GNU string table exceeds SuperZip resource limits");
    }
    std::string table(static_cast<std::size_t>(size), '\0');
    if (!table.empty()) {
        read_exact(input, table.data(), table.size(), "AR GNU string table");
    }
    return table;
}

// Purpose: Scan a full AR archive and validate metadata before extraction.
// Inputs: `archive_path` is the AR file to parse.
// Outputs: Returns trusted extraction metadata; throws on malformed headers, unsafe paths, duplicates, or unsupported layout.
ArScanResult scan_ar(const std::filesystem::path& archive_path) {
    const auto archive_size = std::filesystem::file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open AR archive: " + archive_path.string());
    }
    std::array<char, 8> magic{};
    read_exact(input, magic.data(), magic.size(), "AR global header");
    if (std::string_view(magic.data(), magic.size()) != kArGlobalMagic) {
        throw ArchiveError("unsupported or malformed AR global header");
    }

    ArScanResult result;
    std::string string_table;
    std::vector<ArchivePathValidationEntry> validation_entries;
    while (input.peek() != std::char_traits<char>::eof()) {
        if (result.entries.size() >= kMaxArchiveEntries) {
            throw ArchiveError("AR archive contains too many entries");
        }
        const auto raw = read_ar_header(input);
        const auto member_start_pos = input.tellg();
        if (member_start_pos == std::istream::pos_type(-1)) {
            throw ArchiveError("failed to read AR member offset");
        }
        validate_member_extent(
            static_cast<std::uint64_t>(member_start_pos),
            raw.size,
            archive_size,
            "AR member");
        if (raw.name == "//") {
            string_table = read_string_table(input, raw.size);
        } else {
            const auto entry = parse_member_name_and_payload(input, raw, string_table);
            const auto consumed = entry.has_value() && raw.name.starts_with("#1/")
                ? static_cast<std::uint64_t>(entry->path.size())
                : 0U;
            if (entry.has_value()) {
                validation_entries.push_back(ArchivePathValidationEntry{
                    .path = entry->path,
                    .directory = false,
                });
                result.total_file_bytes = checked_add_ar_bytes(
                    result.total_file_bytes,
                    entry->size,
                    "AR extracted payload byte count overflow");
                result.entries.push_back(*entry);
            }
            if (consumed > raw.size) {
                throw ArchiveError("AR member consumed more bytes than declared");
            }
            skip_file_bytes(input, raw.size - consumed, "AR member payload");
        }
        skip_file_bytes(input, ar_padding(raw.size), "AR member padding");
    }

    validate_archive_path_set(validation_entries);
    return result;
}

// Purpose: Copy a validated AR file payload into a temporary output file.
// Inputs: `input` is the archive stream, `entry` is trusted scan metadata, `target` is the final path, and `overwrite` controls replacement.
// Outputs: Publishes the verified file or throws without partially publishing target bytes.
void extract_ar_file_payload(
    std::ifstream& input,
    const ArEntryMetadata& entry,
    const std::filesystem::path& target,
    bool overwrite) {
    if (entry.payload_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError("AR payload offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(entry.payload_offset), std::ios::beg);
    if (!input) {
        throw ArchiveError("failed to seek AR payload");
    }

    std::filesystem::create_directories(target.parent_path());
    auto temporary_target = reserve_file_publish_target(target);
    try {
        std::ofstream output(temporary_target.file, std::ios::binary);
        if (!output) {
            throw ArchiveError("failed to create temporary extracted file: " + target.string());
        }
        std::array<char, kArIoBufferBytes> buffer{};
        std::uint64_t remaining = entry.size;
        while (remaining > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            read_exact(input, buffer.data(), chunk, "AR file payload");
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

OperationStats compress_ar(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    std::ofstream output(output_archive, std::ios::binary);
    if (!output) {
        throw ArchiveError("cannot create AR archive: " + output_archive.string());
    }
    write_ar_bytes(output, kArGlobalMagic.data(), kArGlobalMagic.size());

    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.file_count);
    std::uint64_t entries = 0;
    for (const auto& entry : manifest.entries) {
        if (entry.directory) {
            continue;
        }
        progress.set_current(entry.archive_path);
        publish_progress(progress, progress_callback);
        const auto header = make_bsd_ar_header(entry.archive_path, entry.size);
        write_ar_bytes(output, header.data(), header.size());
        write_ar_bytes(output, entry.archive_path.data(), entry.archive_path.size());
        copy_file_to_ar(entry.source_path, output, entry.size);
        write_ar_bytes(output, "\n", static_cast<std::size_t>(ar_padding(entry.archive_path.size() + entry.size)));
        progress.add_bytes(entry.size);
        progress.finish_entry();
        ++entries;
    }
    output.close();
    if (!output) {
        throw ArchiveError("failed to finalize AR archive: " + output_archive.string());
    }

    OperationStats stats;
    stats.input_bytes = manifest.total_file_bytes;
    stats.output_bytes = std::filesystem::file_size(output_archive);
    stats.entries = entries;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats extract_ar(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto scanned = scan_ar(archive_path);
    std::filesystem::create_directories(destination);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open AR archive: " + archive_path.string());
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, scanned.total_file_bytes, scanned.entries.size());
    for (const auto& entry : scanned.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        extract_ar_file_payload(input, entry, target, overwrite);
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
