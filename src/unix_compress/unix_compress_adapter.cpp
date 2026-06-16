#include "unix_compress/unix_compress_adapter.hpp"

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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kIoBufferBytes = 64U * 1024U;
constexpr unsigned char kMagic0 = 0x1FU;
constexpr unsigned char kMagic1 = 0x9DU;
constexpr unsigned char kBlockModeFlag = 0x80U;
constexpr unsigned char kReservedFlags = 0x60U;
constexpr unsigned char kMaxBitsMask = 0x1FU;
constexpr int kInitialBits = 9;
constexpr int kMaxBits = 16;
constexpr std::uint32_t kClearCode = 256U;
constexpr std::uint32_t kFirstBlockCode = 257U;
constexpr std::uint32_t kFirstNonBlockCode = 256U;
constexpr std::uint32_t kMaxDictionaryEntries = 1U << kMaxBits;

// Purpose: Read a filesystem file size into the archive telemetry type.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
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

// Purpose: Write all bytes in a buffer to a binary stream.
// Inputs: `output` is the destination stream and `bytes` is the payload to append.
// Outputs: Appends bytes or throws on stream failure.
void write_exact(std::ofstream& output, std::span<const unsigned char> bytes) {
    if (bytes.empty()) {
        return;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw ArchiveError("failed to write Unix Compress stream");
    }
}

// Purpose: Return the highest code representable by an LZW width.
// Inputs: `bits` is the active code width.
// Outputs: Returns `(1 << bits) - 1`, or throws for invalid widths.
std::uint32_t max_code_for_width(int bits) {
    if (bits < kInitialBits || bits > kMaxBits) {
        throw ArchiveError("invalid Unix Compress code width");
    }
    return (1U << static_cast<unsigned int>(bits)) - 1U;
}

// Purpose: Return the Unix Compress encoder threshold for increasing code width.
// Inputs: `bits` is the active code width.
// Outputs: Returns the reference `extcode` threshold used before reading the next byte.
std::uint32_t compressor_extcode_for_width(int bits) {
    if (bits < kInitialBits || bits > kMaxBits) {
        throw ArchiveError("invalid Unix Compress compressor code width");
    }
    const auto base = 1U << static_cast<unsigned int>(bits);
    return bits < kMaxBits ? base + 1U : base;
}

// Purpose: Return the Unix Compress decoder threshold for increasing code width.
// Inputs: `bits` is the active width and `max_bits` is the stream-declared dictionary width.
// Outputs: Returns the reference `maxcode` value used before reading each code.
std::uint32_t decoder_max_code_for_width(int bits, int max_bits) {
    if (bits < kInitialBits || bits > max_bits || max_bits > kMaxBits) {
        throw ArchiveError("invalid Unix Compress decoder code width");
    }
    if (bits == max_bits) {
        return 1U << static_cast<unsigned int>(max_bits);
    }
    return max_code_for_width(bits);
}

// Purpose: Build a compact dictionary key from a prefix code and next byte.
// Inputs: `prefix` is an existing LZW code and `suffix` is the next byte.
// Outputs: Returns a stable 24-bit key for the compressor dictionary.
std::uint32_t dictionary_key(std::uint32_t prefix, unsigned char suffix) {
    return (prefix << 8U) | static_cast<std::uint32_t>(suffix);
}

// Purpose: Convert compressed payload bytes to bits while detecting overflow.
// Inputs: `payload_bytes` is the bounded compressed byte count after the `.Z` header.
// Outputs: Returns bit count or throws before unsigned wraparound.
std::uint64_t payload_bits_from_bytes(std::uint64_t payload_bytes) {
    if (payload_bytes > std::numeric_limits<std::uint64_t>::max() / 8ULL) {
        throw ArchiveError("Unix Compress payload bit count overflows");
    }
    return payload_bytes * 8ULL;
}

class UnixCompressCodeWriter {
public:
    // Purpose: Create a continuous `.Z` code writer over an output stream.
    // Inputs: `output` is the destination stream.
    // Outputs: Initializes a 9-bit writer.
    explicit UnixCompressCodeWriter(std::ofstream& output) : output_(output) {}

    // Purpose: Change the active LZW code width at the reference `.Z` alignment boundary.
    // Inputs: `bits` is the new width.
    // Outputs: Pads to the next old-width code group and uses `bits` for later codes.
    void set_width(int bits) {
        if (bits == width_) {
            return;
        }
        align_to_width_boundary();
        width_ = bits;
    }

    // Purpose: Write one LZW code using the current `.Z` bit packing.
    // Inputs: `code` is the dictionary code to encode.
    // Outputs: Appends bits least-significant first, or throws on invalid codes/write failure.
    void write_code(std::uint32_t code) {
        if (code > max_code_for_width(width_)) {
            throw ArchiveError("Unix Compress code exceeds active width");
        }
        for (int bit = 0; bit < width_; ++bit) {
            write_bit((code & (1U << static_cast<unsigned int>(bit))) != 0U);
        }
    }

    // Purpose: Finish the final partial `.Z` byte.
    // Inputs: None.
    // Outputs: Writes the final byte with zero padding when needed.
    void finish() {
        flush_partial_byte();
    }

private:
    // Purpose: Append one packed bit to the output stream.
    // Inputs: `set` is true when the bit value is one.
    // Outputs: Writes completed bytes immediately.
    void write_bit(bool set) {
        if (set) {
            current_byte_ |= static_cast<unsigned char>(1U << static_cast<unsigned int>(bits_in_byte_));
        }
        ++bits_in_byte_;
        ++logical_bits_;
        if (bits_in_byte_ == 8) {
            const std::array<unsigned char, 1> byte{current_byte_};
            write_exact(output_, byte);
            current_byte_ = 0;
            bits_in_byte_ = 0;
        }
    }

    // Purpose: Pad output to the next old-width `.Z` code group boundary.
    // Inputs: None; uses the current width before it changes.
    // Outputs: Writes zero bits until the reference header-aware boundary is reached.
    void align_to_width_boundary() {
        const auto boundary = static_cast<std::uint64_t>(width_) * 8ULL;
        const auto aligned = logical_bits_ - 1ULL + (boundary - ((logical_bits_ - 1ULL + boundary) % boundary));
        while (logical_bits_ < aligned) {
            write_bit(false);
        }
    }

    // Purpose: Flush the final partially filled byte.
    // Inputs: None.
    // Outputs: Writes one byte only when it contains pending bits.
    void flush_partial_byte() {
        if (bits_in_byte_ == 0) {
            return;
        }
        const std::array<unsigned char, 1> byte{current_byte_};
        write_exact(output_, byte);
        current_byte_ = 0;
        bits_in_byte_ = 0;
    }

    std::ofstream& output_;
    std::uint64_t logical_bits_ = 3ULL * 8ULL;
    unsigned char current_byte_ = 0;
    int width_ = kInitialBits;
    int bits_in_byte_ = 0;
};

class UnixCompressCodeReader {
public:
    // Purpose: Create a continuous `.Z` code reader over a bounded compressed payload.
    // Inputs: `input` is positioned at the payload and `payload_bytes` is the available byte count.
    // Outputs: Initializes a 9-bit reader.
    UnixCompressCodeReader(std::ifstream& input, std::uint64_t payload_bytes)
        : input_(input), payload_limit_bits_(3ULL * 8ULL + payload_bits_from_bytes(payload_bytes)) {}

    // Purpose: Change the active LZW code width at the reference `.Z` alignment boundary.
    // Inputs: `bits` is the new width.
    // Outputs: Discards old-width padding bits and uses `bits` for later codes.
    void set_width(int bits) {
        if (bits == width_) {
            return;
        }
        align_to_width_boundary();
        width_ = bits;
    }

    // Purpose: Reset width after a CLEAR code while still consuming reference padding.
    // Inputs: `bits` is the reset width, normally 9.
    // Outputs: Discards old-width padding and then switches to `bits`.
    void reset_width(int bits) {
        align_to_width_boundary();
        width_ = bits;
    }

    // Purpose: Return how many compressed payload bytes have been read from the host stream.
    // Inputs: None.
    // Outputs: Returns the payload byte counter for progress reporting.
    [[nodiscard]] std::uint64_t consumed_bytes() const {
        return consumed_bytes_;
    }

    // Purpose: Read one LZW code from the current grouped `.Z` bit stream.
    // Inputs: None.
    // Outputs: Returns a code, empty at clean EOF, or throws on stream truncation.
    std::optional<std::uint32_t> read_code() {
        if (payload_limit_bits_ - logical_bits_ < static_cast<std::uint64_t>(width_)) {
            return std::nullopt;
        }

        std::uint32_t code = 0;
        for (int bit = 0; bit < width_; ++bit) {
            if (read_bit()) {
                code |= 1U << static_cast<unsigned int>(bit);
            }
        }
        return code;
    }

private:
    // Purpose: Read one packed bit from the stream.
    // Inputs: None.
    // Outputs: Returns the bit value and advances the absolute bit position.
    bool read_bit() {
        if (logical_bits_ >= payload_limit_bits_) {
            throw ArchiveError("Unix Compress payload is truncated");
        }
        if (bits_in_byte_ == 8) {
            char byte = 0;
            input_.read(&byte, 1);
            if (input_.gcount() != 1) {
                throw ArchiveError("Unix Compress payload is truncated");
            }
            current_byte_ = static_cast<unsigned char>(byte);
            bits_in_byte_ = 0;
            ++consumed_bytes_;
        }
        const bool set = (current_byte_ & static_cast<unsigned char>(1U << static_cast<unsigned int>(bits_in_byte_))) != 0U;
        ++bits_in_byte_;
        ++logical_bits_;
        return set;
    }

    // Purpose: Discard bits up to the next old-width `.Z` code group boundary.
    // Inputs: None; uses the current width before it changes.
    // Outputs: Advances through padding bits without returning them as codes.
    void align_to_width_boundary() {
        const auto boundary = static_cast<std::uint64_t>(width_) * 8ULL;
        const auto aligned = logical_bits_ - 1ULL + (boundary - ((logical_bits_ - 1ULL + boundary) % boundary));
        while (logical_bits_ < aligned && logical_bits_ < payload_limit_bits_) {
            (void)read_bit();
        }
    }

    std::ifstream& input_;
    std::uint64_t payload_limit_bits_ = 0;
    std::uint64_t consumed_bytes_ = 0;
    std::uint64_t logical_bits_ = 3ULL * 8ULL;
    unsigned char current_byte_ = 0;
    int width_ = kInitialBits;
    int bits_in_byte_ = 8;
};

// Purpose: Derive a safe single output entry name from the archive filename.
// Inputs: `archive_path` is the host path to the `.Z` stream.
// Outputs: Returns a relative archive entry name that can pass path-safety checks.
std::string unix_compress_output_entry_name(const std::filesystem::path& archive_path) {
    auto filename = archive_path.filename().string();
    auto lower = filename;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.size() > 2U && lower.ends_with(".z")) {
        filename.resize(filename.size() - 2U);
    } else {
        filename = archive_path.stem().string();
    }
    if (filename.empty()) {
        filename = "payload";
    }
    return normalize_archive_path_key(filename);
}

// Purpose: Expand one LZW code into a byte sequence.
// Inputs: `code` is the requested code, `next_code` bounds valid dictionary entries, and `prefix`/`suffix` store the dictionary.
// Outputs: Returns decoded bytes in forward order, or throws on malformed/cyclic dictionary references.
std::vector<unsigned char> expand_code(
    std::uint32_t code,
    std::uint32_t next_code,
    std::span<const std::uint32_t> prefix,
    std::span<const unsigned char> suffix) {
    std::vector<unsigned char> reversed;
    reversed.reserve(256U);
    auto current = code;
    for (std::uint32_t depth = 0; depth < kMaxDictionaryEntries; ++depth) {
        if (current < 256U) {
            reversed.push_back(static_cast<unsigned char>(current));
            std::reverse(reversed.begin(), reversed.end());
            return reversed;
        }
        if (current >= next_code) {
            throw ArchiveError("Unix Compress stream references an undefined code");
        }
        reversed.push_back(suffix[current]);
        current = prefix[current];
    }
    throw ArchiveError("Unix Compress dictionary chain is too deep");
}

// Purpose: Append decoded bytes to the verified temporary output file.
// Inputs: `output` is the temporary file, `bytes` is the decoded sequence, and `output_size` is updated.
// Outputs: Writes all bytes or throws on output failure/overflow.
void write_output_sequence(std::ofstream& output, std::span<const unsigned char> bytes, std::uint64_t& output_size) {
    checked_add_bytes(output_size, static_cast<std::uint64_t>(bytes.size()), "Unix Compress output");
    write_exact(output, bytes);
}

}  // namespace

OperationStats compress_unix_compress_file(
    const std::filesystem::path& source_file,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("Unix Compress requires one regular file: " + source_file.string());
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) &&
        !equivalent_error) {
        throw SecurityError("refusing to overwrite the Unix Compress source file: " + output_archive.string());
    }

    const auto input_size = regular_file_size(source_file);
    ProgressState progress;
    progress.start(OperationKind::Compress, input_size, 1);
    progress.set_current(source_file.filename().string());
    publish_progress(progress, progress_callback);

    // Write to a verified temporary path first; a malformed stream or I/O error
    // must not replace an existing archive target.
    std::ifstream input(source_file, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Unix Compress source file: " + source_file.string());
    }
    const auto temporary = reserve_file_publish_target(output_archive);
    bool temporary_active = true;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Unix Compress archive: " + output_archive.string());
        }
        // SuperZip emits block-mode 16-bit streams for broad `.Z` compatibility.
        const std::array<unsigned char, 3> header{kMagic0, kMagic1, static_cast<unsigned char>(kBlockModeFlag | kMaxBits)};
        write_exact(output, header);

        UnixCompressCodeWriter writer(output);
        std::unordered_map<std::uint32_t, std::uint32_t> dictionary;
        dictionary.reserve(kMaxDictionaryEntries);
        std::uint32_t next_code = kFirstBlockCode;
        int code_width = kInitialBits;
        std::uint32_t extcode = compressor_extcode_for_width(code_width);
        bool have_current = false;
        std::uint32_t current_code = 0;

        // The encoder mirrors Unix Compress width growth: the width is advanced
        // before reading the next byte and only at the format's packed boundary.
        std::array<unsigned char, kIoBufferBytes> buffer{};
        for (;;) {
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(input.gcount());
            for (std::size_t i = 0; i < bytes_read; ++i) {
                const auto value = buffer[i];
                if (!have_current) {
                    current_code = value;
                    have_current = true;
                    continue;
                }
                if (next_code >= extcode && current_code < kFirstBlockCode) {
                    if (code_width < kMaxBits) {
                        writer.set_width(code_width + 1);
                        ++code_width;
                        extcode = compressor_extcode_for_width(code_width);
                    } else {
                        extcode = kMaxDictionaryEntries + static_cast<std::uint32_t>(kIoBufferBytes);
                    }
                }
                const auto key = dictionary_key(current_code, value);
                const auto found = dictionary.find(key);
                if (found != dictionary.end()) {
                    current_code = found->second;
                    continue;
                }

                writer.write_code(current_code);
                if (next_code < kMaxDictionaryEntries) {
                    dictionary.emplace(key, next_code);
                    ++next_code;
                }
                current_code = value;
            }
            if (bytes_read > 0U) {
                progress.add_bytes(bytes_read);
                publish_progress(progress, progress_callback);
            }
            if (input.bad()) {
                throw ArchiveError("failed to read Unix Compress source file: " + source_file.string());
            }
            if (input.eof()) {
                break;
            }
        }

        if (have_current) {
            writer.write_code(current_code);
        }
        // The final partial byte is zero-padded, then the archive is atomically
        // published only after the stream has flushed successfully.
        writer.finish();
        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Unix Compress archive: " + output_archive.string());
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

OperationStats compress_unix_compress(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& output_archive,
    const ProgressCallback& progress_callback) {
    if (sources.size() != 1U) {
        throw ArchiveError("Unix Compress compatibility requires exactly one regular-file source");
    }
    return compress_unix_compress_file(sources.front(), output_archive, progress_callback);
}

OperationStats extract_unix_compress_file(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& destination,
    bool overwrite,
    const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    if (archive_size < 3U) {
        throw ArchiveError("Unix Compress stream is too small");
    }

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Unix Compress archive: " + archive_path.string());
    }
    std::array<unsigned char, 3> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input) {
        throw ArchiveError("failed to read Unix Compress header");
    }
    if (header[0] != kMagic0 || header[1] != kMagic1) {
        throw ArchiveError("invalid Unix Compress header");
    }
    if ((header[2] & kReservedFlags) != 0U) {
        throw ArchiveError("Unix Compress header uses reserved flags");
    }
    const bool block_mode = (header[2] & kBlockModeFlag) != 0U;
    const int max_bits = static_cast<int>(header[2] & kMaxBitsMask);
    if (max_bits < kInitialBits || max_bits > kMaxBits) {
        throw ArchiveError("Unix Compress header has unsupported maxbits");
    }

    // `.Z` streams have no embedded trusted filename; derive one safe member
    // name from the host archive path and validate it through the shared join.
    const auto entry_name = unix_compress_output_entry_name(archive_path);
    std::filesystem::create_directories(destination);
    const auto target = safe_join_archive_path(destination, entry_name);
    if (!overwrite && std::filesystem::exists(target)) {
        throw SecurityError("refusing to overwrite existing Unix Compress extraction target: " + target.string());
    }
    std::filesystem::create_directories(target.parent_path());

    const auto temporary = reserve_file_publish_target(target);
    bool temporary_active = true;
    std::uint64_t output_size = 0;
    try {
        std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ArchiveError("cannot create Unix Compress extraction target: " + target.string());
        }

        ProgressState progress;
        const auto payload_bytes = archive_size - 3U;
        progress.start(OperationKind::Extract, payload_bytes, 1);
        progress.set_current(entry_name);
        publish_progress(progress, progress_callback);

        UnixCompressCodeReader reader(input, payload_bytes);
        std::vector<std::uint32_t> prefix(kMaxDictionaryEntries);
        std::vector<unsigned char> suffix(kMaxDictionaryEntries);
        const std::uint32_t dictionary_limit = 1U << static_cast<unsigned int>(max_bits);
        std::uint32_t next_code = block_mode ? kFirstBlockCode : kFirstNonBlockCode;
        int code_width = kInitialBits;
        std::uint32_t max_code = decoder_max_code_for_width(code_width, max_bits);
        std::uint64_t reported_input = 0;

        // Progress reports consumed compressed payload bytes, including any
        // alignment padding consumed during width changes.
        auto report_input_progress = [&]() {
            const auto consumed = reader.consumed_bytes();
            if (consumed > reported_input) {
                progress.add_bytes(consumed - reported_input);
                reported_input = consumed;
                publish_progress(progress, progress_callback);
            }
        };

        auto first_code = reader.read_code();
        report_input_progress();
        if (!first_code.has_value()) {
            output.close();
            if (!output) {
                throw ArchiveError("failed to finalize Unix Compress extraction target: " + target.string());
            }
            commit_verified_file(temporary.file, target, overwrite);
            cleanup_file_publish_target(temporary);
            temporary_active = false;
            progress.finish_entry();
            publish_progress(progress, progress_callback);

            OperationStats stats;
            stats.input_bytes = archive_size;
            stats.output_bytes = 0;
            stats.entries = 1;
            stats.gpu_used = false;
            stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return stats;
        }
        if (*first_code > 255U) {
            throw ArchiveError("Unix Compress stream starts with a non-literal code");
        }
        const unsigned char first_byte = static_cast<unsigned char>(*first_code);
        std::array<unsigned char, 1> first_sequence{first_byte};
        write_output_sequence(output, first_sequence, output_size);
        std::uint32_t old_code = *first_code;
        unsigned char first_char = first_byte;

        // Decode codes into a bounded prefix/suffix dictionary. The special
        // KwKwK case is accepted only when it references the next dictionary slot.
        while (true) {
            auto maybe_code = reader.read_code();
            report_input_progress();
            if (!maybe_code.has_value()) {
                break;
            }
            auto code = *maybe_code;
            if (block_mode && code == kClearCode) {
                reader.reset_width(kInitialBits);
                next_code = kClearCode;
                code_width = kInitialBits;
                max_code = decoder_max_code_for_width(code_width, max_bits);
                maybe_code = reader.read_code();
                report_input_progress();
                if (!maybe_code.has_value()) {
                    break;
                }
                code = *maybe_code;
                if (code > 255U) {
                    throw ArchiveError("Unix Compress stream has invalid code after CLEAR");
                }
                const unsigned char literal = static_cast<unsigned char>(code);
                std::array<unsigned char, 1> literal_sequence{literal};
                write_output_sequence(output, literal_sequence, output_size);
                old_code = code;
                first_char = literal;
                continue;
            }

            // Expand first, then add the previous-code/current-first-byte entry
            // exactly as LZW requires.
            std::vector<unsigned char> sequence;
            if (code == next_code) {
                sequence = expand_code(old_code, next_code, prefix, suffix);
                sequence.push_back(first_char);
            } else {
                if (code > next_code) {
                    throw ArchiveError(
                        "Unix Compress stream references future code " + std::to_string(code) +
                        " with next code " + std::to_string(next_code) +
                        " at width " + std::to_string(code_width));
                }
                sequence = expand_code(code, next_code, prefix, suffix);
            }
            if (sequence.empty()) {
                throw ArchiveError("Unix Compress decoded an empty sequence");
            }
            write_output_sequence(output, sequence, output_size);
            first_char = sequence.front();

            if (next_code < dictionary_limit) {
                prefix[next_code] = old_code;
                suffix[next_code] = first_char;
                ++next_code;
            }
            old_code = code;

            // Unix Compress changes width before reading the next code, with
            // padding discarded at the old width boundary.
            while (next_code > max_code && code_width < max_bits) {
                reader.set_width(code_width + 1);
                ++code_width;
                max_code = decoder_max_code_for_width(code_width, max_bits);
            }
        }

        output.close();
        if (!output) {
            throw ArchiveError("failed to finalize Unix Compress extraction target: " + target.string());
        }
        // Publish only after the whole LZW stream has decoded without dictionary
        // violations.
        commit_verified_file(temporary.file, target, overwrite);
        cleanup_file_publish_target(temporary);
        temporary_active = false;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    } catch (...) {
        if (temporary_active) {
            cleanup_file_publish_target(temporary);
        }
        throw;
    }

    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = output_size;
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
