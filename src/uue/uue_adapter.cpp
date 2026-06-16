#include "uue/uue_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace superzip {
namespace {

constexpr std::size_t kUuePayloadBytesPerLine = 45U;
constexpr std::size_t kMaxUueLineBytes = 1024U;
constexpr std::size_t kMaxUuePreambleLines = 128U;

struct UueHeader {
    std::string entry_name;
    std::size_t lines_consumed = 0;
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
// Inputs: `total` is mutated by adding `bytes`; `context` identifies the counter for diagnostics.
// Outputs: Updates `total`, or throws before unsigned wraparound.
void checked_add_bytes(std::uint64_t& total, std::uint64_t bytes, const char* context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    total += bytes;
}

// Purpose: Encode one six-bit UUencode value.
// Inputs: `value` is a six-bit payload value or decoded line length.
// Outputs: Returns the printable UUE character, using backtick for zero.
char encode_uue_six(std::uint8_t value) {
    value &= 0x3FU;
    return value == 0U ? '`' : static_cast<char>(value + 0x20U);
}

// Purpose: Decode one UUencode character into a six-bit value.
// Inputs: `ch` is an encoded payload or length character.
// Outputs: Returns the decoded value; throws for non-UUE bytes.
std::uint8_t decode_uue_six(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    if (value == static_cast<unsigned char>('`')) {
        return 0U;
    }
    if (value >= 0x20U && value <= 0x5FU) {
        return static_cast<std::uint8_t>((value - 0x20U) & 0x3FU);
    }
    throw ArchiveError("UUencoded stream contains an invalid character");
}

// Purpose: Write one complete line to a text-mode UUE output stream.
// Inputs: `output` is the destination stream and `line` excludes the trailing newline.
// Outputs: Appends `line` plus LF or throws on stream failure.
void write_uue_line(std::ofstream& output, const std::string& line) {
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    if (!output) {
        throw ArchiveError("failed to write UUencoded stream");
    }
}

// Purpose: Read one bounded UUE text line without allowing unbounded allocation.
// Inputs: `input` is the source stream and `line` receives bytes excluding CR/LF.
// Outputs: Returns true for a line, false at clean EOF before any bytes; throws on overlong lines or I/O errors.
bool read_uue_line(std::ifstream& input, std::string& line) {
    line.clear();
    std::istream::int_type next = std::char_traits<char>::eof();
    while (!std::char_traits<char>::eq_int_type(
        (next = input.get()),
        std::char_traits<char>::eof())) {
        const auto byte = static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        if (byte == static_cast<unsigned char>('\n')) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        if (line.size() >= kMaxUueLineBytes) {
            throw ArchiveError("UUencoded line exceeds SuperZip metadata limit");
        }
        line.push_back(static_cast<char>(byte));
    }
    if (input.bad()) {
        throw ArchiveError("failed to read UUencoded stream");
    }
    if (!line.empty()) {
        if (line.back() == '\r') {
            line.pop_back();
        }
        return true;
    }
    return false;
}

// Purpose: Test whether a line contains only ignorable whitespace after a UUE end marker.
// Inputs: `line` is a bounded text line.
// Outputs: Returns true when all characters are ASCII whitespace.
bool is_blank_or_whitespace(const std::string& line) {
    return std::ranges::all_of(line, [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

// Purpose: Parse a strict UUE begin line and validate the embedded filename.
// Inputs: `line` is a candidate `begin <mode> <name>` line from untrusted input.
// Outputs: Returns the safe archive entry name or throws on malformed mode/name data.
std::string parse_begin_line(const std::string& line) {
    constexpr std::string_view prefix = "begin ";
    if (!line.starts_with(prefix)) {
        throw ArchiveError("UUencoded stream is missing a begin line");
    }
    const auto mode_start = prefix.size();
    const auto mode_end = line.find(' ', mode_start);
    if (mode_end == std::string::npos || mode_end == mode_start || mode_end + 1U >= line.size()) {
        throw ArchiveError("UUencoded begin line is malformed");
    }
    for (std::size_t i = mode_start; i < mode_end; ++i) {
        const auto ch = static_cast<unsigned char>(line[i]);
        if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('7')) {
            throw ArchiveError("UUencoded begin line has a non-octal mode");
        }
    }
    return normalize_archive_path_key(line.substr(mode_end + 1U));
}

// Purpose: Scan to the first UUE begin line while bounding mail-style preamble tolerance.
// Inputs: `input` is positioned at stream start.
// Outputs: Returns the parsed header and consumed line count; throws when no valid begin line is found.
UueHeader read_uue_header(std::ifstream& input) {
    std::string line;
    for (std::size_t line_index = 1; line_index <= kMaxUuePreambleLines; ++line_index) {
        if (!read_uue_line(input, line)) {
            throw ArchiveError("UUencoded stream ended before begin line");
        }
        if (line.starts_with("begin ")) {
            return UueHeader{
                .entry_name = parse_begin_line(line),
                .lines_consumed = line_index,
            };
        }
    }
    throw ArchiveError("UUencoded stream preamble exceeds SuperZip metadata limit");
}

// Purpose: Build one encoded UUE payload line from up to 45 input bytes.
// Inputs: `bytes` points to `size` decoded bytes from the source file.
// Outputs: Returns a complete UUE line excluding the trailing newline.
std::string encode_uue_payload_line(const unsigned char* bytes, std::size_t size) {
    if (size > kUuePayloadBytesPerLine) {
        throw ArchiveError("UUencoded encoder received an oversized line");
    }
    std::string line;
    line.reserve(1U + ((size + 2U) / 3U) * 4U);
    line.push_back(encode_uue_six(static_cast<std::uint8_t>(size)));
    for (std::size_t i = 0; i < size; i += 3U) {
        const auto a = bytes[i];
        const auto b = i + 1U < size ? bytes[i + 1U] : 0U;
        const auto c = i + 2U < size ? bytes[i + 2U] : 0U;
        line.push_back(encode_uue_six(static_cast<std::uint8_t>(a >> 2U)));
        line.push_back(encode_uue_six(static_cast<std::uint8_t>(((a << 4U) | (b >> 4U)) & 0x3FU)));
        line.push_back(encode_uue_six(static_cast<std::uint8_t>(((b << 2U) | (c >> 6U)) & 0x3FU)));
        line.push_back(encode_uue_six(static_cast<std::uint8_t>(c & 0x3FU)));
    }
    return line;
}

// Purpose: Decode one UUE payload line into raw bytes.
// Inputs: `line` is a bounded encoded line and `decoded` receives exactly the declared byte count.
// Outputs: Mutates `decoded`; returns false for the zero-length terminator line and throws on malformed payload data.
bool decode_uue_payload_line(const std::string& line, std::vector<unsigned char>& decoded) {
    if (line.empty()) {
        throw ArchiveError("UUencoded payload line is empty");
    }
    const auto declared = static_cast<std::size_t>(decode_uue_six(line.front()));
    if (declared == 0U) {
        decoded.clear();
        return false;
    }
    if (declared > kUuePayloadBytesPerLine) {
        throw ArchiveError("UUencoded payload line declares too many bytes");
    }
    const auto encoded_chars = ((declared + 2U) / 3U) * 4U;
    if (line.size() < 1U + encoded_chars) {
        throw ArchiveError("UUencoded payload line is truncated");
    }
    for (std::size_t i = 1U + encoded_chars; i < line.size(); ++i) {
        if (line[i] != ' ' && line[i] != '\t') {
            throw ArchiveError("UUencoded payload line contains trailing data");
        }
    }
    decoded.clear();
    decoded.reserve(declared);
    for (std::size_t offset = 1U; offset < 1U + encoded_chars; offset += 4U) {
        const auto a = decode_uue_six(line[offset]);
        const auto b = decode_uue_six(line[offset + 1U]);
        const auto c = decode_uue_six(line[offset + 2U]);
        const auto d = decode_uue_six(line[offset + 3U]);
        const std::array<unsigned char, 3> group{
            static_cast<unsigned char>((a << 2U) | (b >> 4U)),
            static_cast<unsigned char>((b << 4U) | (c >> 2U)),
            static_cast<unsigned char>((c << 6U) | d),
        };
        const auto remaining = declared - decoded.size();
        const auto take = std::min<std::size_t>(remaining, group.size());
        decoded.insert(decoded.end(), group.begin(), group.begin() + static_cast<std::ptrdiff_t>(take));
    }
    if (decoded.size() != declared) {
        throw ArchiveError("UUencoded payload length mismatch");
    }
    return true;
}

}  // namespace

OperationStats compress_uue_file(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("UUE compression requires one regular file: " + source_file.string());
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) &&
        !equivalent_error) {
        throw SecurityError("refusing to overwrite the UUE source file: " + output_archive.string());
    }

    const auto input_size = regular_file_size(source_file);
    const auto entry_name = normalize_archive_path_key(source_file.filename().string());
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open UUE source file: " + source_file.string());
    }
    const auto temporary = reserve_file_publish_target(output_archive);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create UUE archive: " + output_archive.string());
        }
        write_uue_line(output, "begin 644 " + entry_name);
        std::array<unsigned char, kUuePayloadBytesPerLine> buffer{};
        for (;;) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                write_uue_line(output, encode_uue_payload_line(buffer.data(), bytes_read));
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad()) {
                throw ArchiveError("failed to read UUE source file: " + source_file.string());
            }
            if (input.eof()) {
                break;
            }
        }
        write_uue_line(output, "`");
        write_uue_line(output, "end");
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize UUE archive: " + output_archive.string());
        }
        commit_verified_file(temporary.file, output_archive, true);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }

    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = input_size;
    stats.output_bytes = regular_file_size(output_archive);
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

OperationStats compress_uue(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    if (sources.size() != 1U) {
        throw ArchiveError("UUE compatibility requires exactly one regular-file source");
    }
    return compress_uue_file(sources.front(), output_archive, progress_callback);
}

OperationStats extract_uue_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open UUE archive: " + archive_path.string());
    }
    const auto header = read_uue_header(input);
    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, header.entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing UUE extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(header.entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create UUE extraction target: " + target.string());
        }

        std::string line;
        std::vector<unsigned char> decoded;
        bool saw_zero_line = false;
        std::size_t line_count = header.lines_consumed;
        while (read_uue_line(input, line)) {
            ++line_count;
            if (!saw_zero_line) {
                if (!decode_uue_payload_line(line, decoded)) {
                    saw_zero_line = true;
                    continue;
                }
                output.write(reinterpret_cast<const char*>(decoded.data()), static_cast<std::streamsize>(decoded.size()));
                if (!output) {
                    throw ArchiveError("failed to write UUE extraction target: " + target.string());
                }
                checked_add_bytes(output_size, decoded.size(), "UUE output");
                progress.add_bytes(line.size() + 1U);
                publish_progress(progress, progress_callback);
                continue;
            }
            if (line == "end") {
                break;
            }
            throw ArchiveError("UUencoded stream is missing end marker after zero-length line");
        }
        if (!saw_zero_line || line != "end") {
            throw ArchiveError("UUencoded stream ended before the end marker");
        }
        while (read_uue_line(input, line)) {
            ++line_count;
            if (!is_blank_or_whitespace(line)) {
                throw ArchiveError("UUencoded stream contains trailing data after end marker");
            }
        }
        if (line_count < 3U) {
            throw ArchiveError("UUencoded stream is incomplete");
        }
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize UUE extraction target: " + target.string());
        }
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
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
