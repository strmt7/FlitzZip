#include "base64/base64_adapter.hpp"

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
#include <optional>
#include <string>
#include <string_view>

namespace superzip {
namespace {

constexpr std::size_t kBase64InputBytesPerLine = 57U;
constexpr std::size_t kMaxBase64LineBytes = 4096U;
constexpr std::size_t kMaxBase64PreambleLines = 128U;
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct Base64Header {
    std::optional<std::string> entry_name;
    std::size_t lines_consumed = 0;
    bool wrapped = false;
};

struct Base64DecodeState {
    std::array<char, 4> quad{};
    std::size_t quad_size = 0;
    bool terminal_padding_seen = false;
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

// Purpose: Write one complete line to a Base64 output stream.
// Inputs: `output` is the destination stream and `line` excludes the trailing newline.
// Outputs: Appends `line` plus LF or throws on stream failure.
void write_base64_line(std::ofstream& output, const std::string& line) {
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    if (!output) {
        throw ArchiveError("failed to write Base64 stream");
    }
}

// Purpose: Read one bounded Base64 text line without allowing unbounded allocation.
// Inputs: `input` is the source stream and `line` receives bytes excluding CR/LF.
// Outputs: Returns true for a line, false at clean EOF before any bytes; throws on overlong lines or I/O errors.
bool read_base64_line(std::ifstream& input, std::string& line) {
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
        if (line.size() >= kMaxBase64LineBytes) {
            throw ArchiveError("Base64 line exceeds SuperZip metadata limit");
        }
        line.push_back(static_cast<char>(byte));
    }
    if (input.bad()) {
        throw ArchiveError("failed to read Base64 stream");
    }
    if (!line.empty()) {
        if (line.back() == '\r') {
            line.pop_back();
        }
        return true;
    }
    return false;
}

// Purpose: Test whether a line contains only ignorable whitespace after a Base64 trailer.
// Inputs: `line` is a bounded text line.
// Outputs: Returns true when all characters are ASCII whitespace.
bool is_blank_or_whitespace(const std::string& line) {
    return std::ranges::all_of(line, [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
}

// Purpose: Encode up to three bytes into one Base64 quantum.
// Inputs: `bytes` points to `size` decoded bytes, where `size` is 1 through 3.
// Outputs: Returns four Base64 characters with RFC padding.
std::array<char, 4> encode_base64_quantum(const unsigned char* bytes, std::size_t size) {
    if (size == 0U || size > 3U) {
        throw ArchiveError("Base64 encoder received an invalid quantum size");
    }
    const auto a = bytes[0];
    const auto b = size > 1U ? bytes[1] : 0U;
    const auto c = size > 2U ? bytes[2] : 0U;
    return {
        kBase64Alphabet[(a >> 2U) & 0x3FU],
        kBase64Alphabet[((a << 4U) | (b >> 4U)) & 0x3FU],
        size > 1U ? kBase64Alphabet[((b << 2U) | (c >> 6U)) & 0x3FU] : '=',
        size > 2U ? kBase64Alphabet[c & 0x3FU] : '=',
    };
}

// Purpose: Build one RFC-style Base64 payload line from up to 57 input bytes.
// Inputs: `bytes` points to `size` decoded bytes from the source file.
// Outputs: Returns a complete Base64 line excluding the trailing newline.
std::string encode_base64_payload_line(const unsigned char* bytes, std::size_t size) {
    if (size > kBase64InputBytesPerLine) {
        throw ArchiveError("Base64 encoder received an oversized line");
    }
    std::string line;
    line.reserve(((size + 2U) / 3U) * 4U);
    for (std::size_t i = 0; i < size; i += 3U) {
        const auto take = std::min<std::size_t>(3U, size - i);
        const auto quantum = encode_base64_quantum(bytes + i, take);
        line.append(quantum.data(), quantum.size());
    }
    return line;
}

// Purpose: Convert one Base64 alphabet character into its six-bit value.
// Inputs: `ch` is a payload character and may not be padding.
// Outputs: Returns the decoded six-bit value or throws on invalid input.
std::uint8_t decode_base64_value(char ch) {
    const auto found = kBase64Alphabet.find(ch);
    if (found == std::string_view::npos) {
        throw ArchiveError("Base64 stream contains an invalid character");
    }
    return static_cast<std::uint8_t>(found);
}

// Purpose: Decode one complete four-character Base64 quantum.
// Inputs: `quad` is the encoded group and `decoded` receives one to three bytes.
// Outputs: Appends decoded bytes and returns true when padding terminates the payload.
bool decode_base64_quantum(const std::array<char, 4>& quad, std::vector<unsigned char>& decoded) {
    if (quad[0] == '=' || quad[1] == '=') {
        throw ArchiveError("Base64 padding appears before enough payload data");
    }
    const bool pad2 = quad[2] == '=';
    const bool pad3 = quad[3] == '=';
    if (pad2 && !pad3) {
        throw ArchiveError("Base64 padding sequence is malformed");
    }
    const auto a = decode_base64_value(quad[0]);
    const auto b = decode_base64_value(quad[1]);
    const auto c = pad2 ? 0U : decode_base64_value(quad[2]);
    const auto d = pad3 ? 0U : decode_base64_value(quad[3]);
    decoded.push_back(static_cast<unsigned char>((a << 2U) | (b >> 4U)));
    if (!pad2) {
        decoded.push_back(static_cast<unsigned char>((b << 4U) | (c >> 2U)));
    }
    if (!pad3) {
        decoded.push_back(static_cast<unsigned char>((c << 6U) | d));
    }
    return pad2 || pad3;
}

// Purpose: Feed one Base64 payload character into the streaming decoder.
// Inputs: `state` tracks the current quantum, `ch` is an encoded or padding character, and `decoded` receives completed bytes.
// Outputs: Mutates decoder state and decoded output; throws on non-canonical data after padding.
void feed_base64_char(Base64DecodeState& state, char ch, std::vector<unsigned char>& decoded) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
        return;
    }
    if (state.terminal_padding_seen) {
        throw ArchiveError("Base64 stream contains payload data after padding");
    }
    if (ch != '=' && kBase64Alphabet.find(ch) == std::string_view::npos) {
        throw ArchiveError("Base64 stream contains an invalid character");
    }
    state.quad[state.quad_size++] = ch;
    if (state.quad_size == state.quad.size()) {
        state.terminal_padding_seen = decode_base64_quantum(state.quad, decoded);
        state.quad_size = 0;
    }
}

// Purpose: Finish a Base64 decode stream and validate the final quantum boundary.
// Inputs: `state` is the streaming decoder state.
// Outputs: Returns normally only when no partial quantum remains.
void finish_base64_decode(const Base64DecodeState& state) {
    if (state.quad_size != 0U) {
        throw ArchiveError("Base64 stream ended with a partial quantum");
    }
}

// Purpose: Decode a compact Base64 token into a string for GNU-style encoded names.
// Inputs: `token` is a single Base64 token without embedded whitespace.
// Outputs: Returns decoded bytes as a string or throws on malformed Base64.
std::string decode_base64_token(std::string_view token) {
    Base64DecodeState state;
    std::vector<unsigned char> decoded;
    for (char ch : token) {
        feed_base64_char(state, ch, decoded);
    }
    finish_base64_decode(state);
    return std::string(decoded.begin(), decoded.end());
}

// Purpose: Parse a Base64 wrapper begin line and validate the embedded filename.
// Inputs: `line` is a candidate `begin-base64 <mode> <name>` line from untrusted input.
// Outputs: Returns the safe archive entry name or throws on malformed mode/name data.
std::string parse_begin_base64_line(const std::string& line) {
    constexpr std::string_view raw_prefix = "begin-base64 ";
    constexpr std::string_view encoded_prefix = "begin-base64-encoded ";
    const bool encoded_name = line.starts_with(encoded_prefix);
    const auto prefix_size = encoded_name ? encoded_prefix.size() : raw_prefix.size();
    if (!encoded_name && !line.starts_with(raw_prefix)) {
        throw ArchiveError("Base64 wrapper is missing a begin-base64 line");
    }
    const auto mode_start = prefix_size;
    const auto mode_end = line.find(' ', mode_start);
    if (mode_end == std::string::npos || mode_end == mode_start || mode_end + 1U >= line.size()) {
        throw ArchiveError("Base64 begin line is malformed");
    }
    for (std::size_t i = mode_start; i < mode_end; ++i) {
        const auto ch = static_cast<unsigned char>(line[i]);
        if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('7')) {
            throw ArchiveError("Base64 begin line has a non-octal mode");
        }
    }
    const auto name = encoded_name ? decode_base64_token(line.substr(mode_end + 1U)) : line.substr(mode_end + 1U);
    return normalize_archive_path_key(name);
}

// Purpose: Derive a safe raw Base64 output filename when no wrapper header exists.
// Inputs: `archive_path` is the source `.b64` path.
// Outputs: Returns a normalized single-file output name.
std::string raw_base64_output_name(const std::filesystem::path& archive_path) {
    auto name = archive_path.stem().string();
    if (name.empty()) {
        name = "decoded.bin";
    }
    return normalize_archive_path_key(name);
}

// Purpose: Scan to an optional Base64 wrapper begin line while bounding preamble tolerance.
// Inputs: `input` is positioned at stream start and `archive_path` supplies raw-mode filename fallback.
// Outputs: Returns wrapper metadata; resets `input` to the start when no wrapper is found.
Base64Header read_base64_header_or_reset(std::ifstream& input, const std::filesystem::path& archive_path) {
    std::string line;
    for (std::size_t line_index = 1; line_index <= kMaxBase64PreambleLines; ++line_index) {
        const auto position = input.tellg();
        if (!read_base64_line(input, line)) {
            input.clear();
            input.seekg(0, std::ios::beg);
            return Base64Header{.entry_name = raw_base64_output_name(archive_path), .lines_consumed = 0, .wrapped = false};
        }
        if (line.starts_with("begin-base64 ")) {
            return Base64Header{.entry_name = parse_begin_base64_line(line), .lines_consumed = line_index, .wrapped = true};
        }
        if (line.starts_with("begin-base64-encoded ")) {
            return Base64Header{.entry_name = parse_begin_base64_line(line), .lines_consumed = line_index, .wrapped = true};
        }
        if (line_index == 1U && position == std::ifstream::pos_type(0) && !line.empty()) {
            input.clear();
            input.seekg(0, std::ios::beg);
            return Base64Header{.entry_name = raw_base64_output_name(archive_path), .lines_consumed = 0, .wrapped = false};
        }
    }
    throw ArchiveError("Base64 preamble exceeds SuperZip metadata limit");
}

// Purpose: Decode a Base64 stream body to an already-open temporary file.
// Inputs: `input` is positioned at payload start, `header` defines raw/wrapper mode, and `output` is the destination temp stream.
// Outputs: Returns decoded byte count; throws on malformed payload, missing trailer, or output failure.
std::uint64_t decode_base64_body(std::ifstream& input, const Base64Header& header, std::ofstream& output) {
    std::string line;
    std::vector<unsigned char> decoded;
    Base64DecodeState state;
    std::uint64_t output_size = 0;
    bool saw_wrapper_trailer = false;
    while (read_base64_line(input, line)) {
        if (header.wrapped && line == "====") {
            saw_wrapper_trailer = true;
            break;
        }
        decoded.clear();
        for (char ch : line) {
            feed_base64_char(state, ch, decoded);
        }
        if (!decoded.empty()) {
            output.write(reinterpret_cast<const char*>(decoded.data()), static_cast<std::streamsize>(decoded.size()));
            if (!output) {
                throw ArchiveError("failed to write Base64 extraction target");
            }
            checked_add_bytes(output_size, decoded.size(), "Base64 output");
        }
    }
    finish_base64_decode(state);
    if (header.wrapped) {
        if (!saw_wrapper_trailer) {
            throw ArchiveError("Base64 wrapper ended before the trailer line");
        }
        while (read_base64_line(input, line)) {
            if (!is_blank_or_whitespace(line)) {
                throw ArchiveError("Base64 wrapper contains trailing data after trailer");
            }
        }
    }
    return output_size;
}

}  // namespace

OperationStats compress_base64_file(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("Base64 compression requires one regular file: " + source_file.string());
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) &&
        !equivalent_error) {
        throw SecurityError("refusing to overwrite the Base64 source file: " + output_archive.string());
    }

    const auto input_size = regular_file_size(source_file);
    const auto entry_name = normalize_archive_path_key(source_file.filename().string());
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(entry_name);
    publish_progress(progress, progress_callback);

    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Base64 source file: " + source_file.string());
    }
    const auto temporary = reserve_file_publish_target(output_archive);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Base64 archive: " + output_archive.string());
        }
        write_base64_line(output, "begin-base64 644 " + entry_name);
        std::array<unsigned char, kBase64InputBytesPerLine> buffer{};
        for (;;) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            if (bytes_read > 0U) {
                write_base64_line(output, encode_base64_payload_line(buffer.data(), bytes_read));
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad()) {
                throw ArchiveError("failed to read Base64 source file: " + source_file.string());
            }
            if (input.eof()) {
                break;
            }
        }
        write_base64_line(output, "====");
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Base64 archive: " + output_archive.string());
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

OperationStats compress_base64(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    if (sources.size() != 1U) {
        throw ArchiveError("Base64 compatibility requires exactly one regular-file source");
    }
    return compress_base64_file(sources.front(), output_archive, progress_callback);
}

OperationStats extract_base64_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Base64 archive: " + archive_path.string());
    }
    const auto header = read_base64_header_or_reset(input, archive_path);
    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, *header.entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing Base64 extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current(*header.entry_name);
    publish_progress(progress, progress_callback);

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Base64 extraction target: " + target.string());
        }
        output_size = decode_base64_body(input, header, output);
        progress.add_bytes(archive_size);
        publish_progress(progress, progress_callback);
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Base64 extraction target: " + target.string());
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
