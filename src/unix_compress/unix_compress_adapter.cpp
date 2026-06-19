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
        const bool set =
            (current_byte_ & static_cast<unsigned char>(1U << static_cast<unsigned int>(bits_in_byte_))) != 0U;
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
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
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
// Inputs: `code` is the requested code, `next_code` bounds valid dictionary entries, and `prefix`/`suffix` store the
// dictionary.
// Outputs: Returns decoded bytes in forward order, or throws on malformed/cyclic dictionary references.
std::vector<unsigned char> expand_code(std::uint32_t code, std::uint32_t next_code,
                                       std::span<const std::uint32_t> prefix, std::span<const unsigned char> suffix) {
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

struct UnixCompressStreamHeader {
    bool block_mode = false;
    int max_bits = kInitialBits;
    std::uint64_t payload_bytes = 0;
};

struct UnixCompressDecodeState {
    // Purpose: Initialize bounded Unix Compress LZW dictionary state from trusted header values.
    // Inputs: `block_mode` selects CLEAR-code semantics and `max_bits` is the validated stream dictionary width.
    // Outputs: Creates prefix/suffix dictionaries and decoder counters for the payload stream.
    UnixCompressDecodeState(bool block_mode, int max_bits)
        : prefix(kMaxDictionaryEntries), suffix(kMaxDictionaryEntries),
          dictionary_limit(1U << static_cast<unsigned int>(max_bits)),
          next_code(block_mode ? kFirstBlockCode : kFirstNonBlockCode),
          max_code(decoder_max_code_for_width(code_width, max_bits)) {}

    std::vector<std::uint32_t> prefix;
    std::vector<unsigned char> suffix;
    std::uint32_t dictionary_limit = 0;
    std::uint32_t next_code = 0;
    int code_width = kInitialBits;
    std::uint32_t max_code = 0;
    std::uint32_t old_code = 0;
    unsigned char first_char = 0;
};

// Purpose: Read and validate the three-byte Unix Compress stream header.
// Inputs: `input` is positioned at the start of `archive_path`; `archive_size` bounds payload byte accounting.
// Outputs: Returns validated block mode, maxbits, and payload size; throws on unsupported or malformed headers.
UnixCompressStreamHeader read_unix_compress_header(std::ifstream& input, const std::filesystem::path& archive_path,
                                                   std::uint64_t archive_size) {
    if (archive_size < 3U) {
        throw ArchiveError("Unix Compress stream is too small");
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
    const int max_bits = static_cast<int>(header[2] & kMaxBitsMask);
    if (max_bits < kInitialBits || max_bits > kMaxBits) {
        throw ArchiveError("Unix Compress header has unsupported maxbits");
    }
    if (!input.good() && !input.eof()) {
        throw ArchiveError("cannot open Unix Compress archive: " + archive_path.string());
    }
    return UnixCompressStreamHeader{
        .block_mode = (header[2] & kBlockModeFlag) != 0U,
        .max_bits = max_bits,
        .payload_bytes = archive_size - 3U,
    };
}

// Purpose: Publish newly consumed compressed payload bytes to the caller progress callback.
// Inputs: `reader` tracks consumed bytes, `progress` is the active operation state, and `reported_input` is mutated.
// Outputs: Updates progress only when the reader has advanced.
void report_unix_compress_input_progress(UnixCompressCodeReader& reader, ProgressState& progress,
                                         const ProgressCallback& progress_callback, std::uint64_t& reported_input) {
    const auto consumed = reader.consumed_bytes();
    if (consumed <= reported_input) {
        return;
    }
    progress.add_bytes(consumed - reported_input);
    reported_input = consumed;
    publish_progress(progress, progress_callback);
}

// Purpose: Read one Unix Compress code and immediately report compressed-input progress.
// Inputs: `reader` is the LZW code source and progress arguments mirror `report_unix_compress_input_progress`.
// Outputs: Returns the next code, or empty at clean end of stream.
std::optional<std::uint32_t> read_unix_compress_code_with_progress(UnixCompressCodeReader& reader,
                                                                   ProgressState& progress,
                                                                   const ProgressCallback& progress_callback,
                                                                   std::uint64_t& reported_input) {
    auto code = reader.read_code();
    report_unix_compress_input_progress(reader, progress, progress_callback, reported_input);
    return code;
}

// Purpose: Write the first literal code in a Unix Compress payload.
// Inputs: `first_code` is the initial code; `output` and `output_size` receive the decoded byte.
// Outputs: Initializes `state` or throws when the stream starts with an invalid dictionary code.
void initialize_unix_compress_decoder_state(UnixCompressDecodeState& state, std::uint32_t first_code,
                                            std::ofstream& output, std::uint64_t& output_size) {
    if (first_code > 255U) {
        throw ArchiveError("Unix Compress stream starts with a non-literal code");
    }
    const unsigned char first_byte = static_cast<unsigned char>(first_code);
    const std::array<unsigned char, 1> first_sequence{first_byte};
    write_output_sequence(output, first_sequence, output_size);
    state.old_code = first_code;
    state.first_char = first_byte;
}

// Purpose: Decode the literal code that follows a Unix Compress CLEAR marker.
// Inputs: `reader` is positioned after CLEAR and `max_bits` is the validated stream dictionary width.
// Outputs: Returns false on clean EOF after CLEAR, otherwise writes one literal and resets `state`.
bool decode_unix_compress_clear_literal(UnixCompressCodeReader& reader, UnixCompressDecodeState& state, int max_bits,
                                        std::ofstream& output, std::uint64_t& output_size, ProgressState& progress,
                                        const ProgressCallback& progress_callback, std::uint64_t& reported_input) {
    reader.reset_width(kInitialBits);
    state.next_code = kClearCode;
    state.code_width = kInitialBits;
    state.max_code = decoder_max_code_for_width(state.code_width, max_bits);
    auto maybe_code = read_unix_compress_code_with_progress(reader, progress, progress_callback, reported_input);
    if (!maybe_code.has_value()) {
        return false;
    }
    if (*maybe_code > 255U) {
        throw ArchiveError("Unix Compress stream has invalid code after CLEAR");
    }
    const unsigned char literal = static_cast<unsigned char>(*maybe_code);
    const std::array<unsigned char, 1> literal_sequence{literal};
    write_output_sequence(output, literal_sequence, output_size);
    state.old_code = *maybe_code;
    state.first_char = literal;
    return true;
}

// Purpose: Expand one non-CLEAR Unix Compress code through the bounded LZW dictionary.
// Inputs: `code` is the current code and `state` carries the previous-code context.
// Outputs: Returns the decoded byte sequence or throws on malformed future-code references.
std::vector<unsigned char> decode_unix_compress_sequence(std::uint32_t code, const UnixCompressDecodeState& state) {
    if (code == state.next_code) {
        auto sequence = expand_code(state.old_code, state.next_code, state.prefix, state.suffix);
        sequence.push_back(state.first_char);
        return sequence;
    }
    if (code > state.next_code) {
        throw ArchiveError("Unix Compress stream references future code " + std::to_string(code) + " with next code " +
                           std::to_string(state.next_code) + " at width " + std::to_string(state.code_width));
    }
    return expand_code(code, state.next_code, state.prefix, state.suffix);
}

// Purpose: Advance Unix Compress dictionary and code-width state after writing one decoded sequence.
// Inputs: `code`, `sequence`, and `max_bits` describe the current decoded LZW step.
// Outputs: Mutates `state` and reader alignment exactly as the `.Z` format requires.
void advance_unix_compress_dictionary(UnixCompressCodeReader& reader, UnixCompressDecodeState& state,
                                      std::uint32_t code, std::span<const unsigned char> sequence, int max_bits) {
    if (sequence.empty()) {
        throw ArchiveError("Unix Compress decoded an empty sequence");
    }
    state.first_char = sequence.front();
    if (state.next_code < state.dictionary_limit) {
        state.prefix[state.next_code] = state.old_code;
        state.suffix[state.next_code] = state.first_char;
        ++state.next_code;
    }
    state.old_code = code;
    while (state.next_code > state.max_code && state.code_width < max_bits) {
        reader.set_width(state.code_width + 1);
        ++state.code_width;
        state.max_code = decoder_max_code_for_width(state.code_width, max_bits);
    }
}

// Purpose: Decode a validated Unix Compress payload into an already-open temporary output file.
// Inputs: `input` is positioned after the header, `stream` carries validated header values, and progress is active.
// Outputs: Writes decoded bytes and returns output size; throws before final publication on malformed LZW data.
std::uint64_t decode_unix_compress_payload(std::ifstream& input, std::ofstream& output,
                                           const UnixCompressStreamHeader& stream, ProgressState& progress,
                                           const ProgressCallback& progress_callback) {
    UnixCompressCodeReader reader(input, stream.payload_bytes);
    UnixCompressDecodeState state(stream.block_mode, stream.max_bits);
    std::uint64_t reported_input = 0;
    std::uint64_t output_size = 0;
    auto first_code = read_unix_compress_code_with_progress(reader, progress, progress_callback, reported_input);
    if (!first_code.has_value()) {
        return output_size;
    }
    initialize_unix_compress_decoder_state(state, *first_code, output, output_size);

    while (true) {
        auto maybe_code = read_unix_compress_code_with_progress(reader, progress, progress_callback, reported_input);
        if (!maybe_code.has_value()) {
            break;
        }
        const auto code = *maybe_code;
        if (stream.block_mode && code == kClearCode) {
            if (!decode_unix_compress_clear_literal(reader, state, stream.max_bits, output, output_size, progress,
                                                    progress_callback, reported_input)) {
                break;
            }
            continue;
        }
        const auto sequence = decode_unix_compress_sequence(code, state);
        write_output_sequence(output, sequence, output_size);
        advance_unix_compress_dictionary(reader, state, code, sequence, stream.max_bits);
    }
    return output_size;
}

struct UnixCompressEncodeState {
    // Purpose: Initialize bounded Unix Compress encoder dictionary state.
    // Inputs: None; SuperZip emits block-mode 16-bit `.Z` streams.
    // Outputs: Creates the compressor dictionary and counters for a new payload.
    UnixCompressEncodeState() {
        dictionary.reserve(kMaxDictionaryEntries);
    }

    std::unordered_map<std::uint32_t, std::uint32_t> dictionary;
    std::uint32_t next_code = kFirstBlockCode;
    int code_width = kInitialBits;
    std::uint32_t extcode = compressor_extcode_for_width(code_width);
    bool have_current = false;
    std::uint32_t current_code = 0;
};

// Purpose: Advance the Unix Compress encoder width at the reference packed boundary.
// Inputs: `writer` owns output bit packing and `state` carries dictionary counters.
// Outputs: Mutates the active writer width and threshold when the dictionary requires it.
void advance_unix_compress_encoder_width(UnixCompressCodeWriter& writer, UnixCompressEncodeState& state) {
    if (state.next_code < state.extcode || state.current_code >= kFirstBlockCode) {
        return;
    }
    if (state.code_width < kMaxBits) {
        writer.set_width(state.code_width + 1);
        ++state.code_width;
        state.extcode = compressor_extcode_for_width(state.code_width);
        return;
    }
    state.extcode = kMaxDictionaryEntries + static_cast<std::uint32_t>(kIoBufferBytes);
}

// Purpose: Encode one source byte into the Unix Compress LZW stream.
// Inputs: `value` is the next uncompressed byte; `writer` and `state` carry encoder state.
// Outputs: Writes completed codes as needed and extends the dictionary until the format limit.
void encode_unix_compress_byte(UnixCompressCodeWriter& writer, UnixCompressEncodeState& state, unsigned char value) {
    if (!state.have_current) {
        state.current_code = value;
        state.have_current = true;
        return;
    }
    advance_unix_compress_encoder_width(writer, state);
    const auto key = dictionary_key(state.current_code, value);
    const auto found = state.dictionary.find(key);
    if (found != state.dictionary.end()) {
        state.current_code = found->second;
        return;
    }

    writer.write_code(state.current_code);
    if (state.next_code < kMaxDictionaryEntries) {
        state.dictionary.emplace(key, state.next_code);
        ++state.next_code;
    }
    state.current_code = value;
}

// Purpose: Encode a buffered byte range into a Unix Compress payload.
// Inputs: `bytes` is a contiguous chunk read from the source file.
// Outputs: Appends any complete LZW codes and mutates `state` for subsequent chunks.
void encode_unix_compress_buffer(UnixCompressCodeWriter& writer, UnixCompressEncodeState& state,
                                 std::span<const unsigned char> bytes) {
    for (const auto value : bytes) {
        encode_unix_compress_byte(writer, state, value);
    }
}

// Purpose: Write a complete Unix Compress payload from an input stream to an output stream.
// Inputs: `input` is the opened source, `output` is a temporary archive, and progress is active.
// Outputs: Emits header/payload bytes, updates progress, and throws on read/write failures.
void encode_unix_compress_payload(std::ifstream& input, std::ofstream& output, ProgressState& progress,
                                  const ProgressCallback& progress_callback, const std::filesystem::path& source_file) {
    const std::array<unsigned char, 3> header{kMagic0, kMagic1, static_cast<unsigned char>(kBlockModeFlag | kMaxBits)};
    write_exact(output, header);

    UnixCompressCodeWriter writer(output);
    UnixCompressEncodeState state;
    std::array<unsigned char, kIoBufferBytes> buffer{};
    for (;;) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto bytes_read = static_cast<std::size_t>(input.gcount());
        encode_unix_compress_buffer(writer, state, std::span<const unsigned char>(buffer.data(), bytes_read));
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

    if (state.have_current) {
        writer.write_code(state.current_code);
    }
    writer.finish();
}

}  // namespace

// Purpose: Compress one regular file into a Unix Compress `.Z` compatibility archive.
// Inputs: `source_file` is the single regular file, `output_archive` is the target path, and progress is optional.
// Outputs: Publishes the archive atomically and returns operation statistics; throws on unsafe overwrite or I/O
// failure.
OperationStats compress_unix_compress_file(const std::filesystem::path& source_file,
                                           const std::filesystem::path& output_archive,
                                           const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    if (!std::filesystem::is_regular_file(source_file)) {
        throw ArchiveError("Unix Compress requires one regular file: " + source_file.string());
    }
    std::error_code equivalent_error;
    if (std::filesystem::exists(output_archive) &&
        std::filesystem::equivalent(source_file, output_archive, equivalent_error) && !equivalent_error) {
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
        encode_unix_compress_payload(input, output, progress, progress_callback, source_file);
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

// Purpose: Compress the supported Unix Compress source set.
// Inputs: `sources` must contain exactly one regular-file path, `output_archive` is the target, and progress is
// optional.
// Outputs: Delegates to the single-file writer and returns operation statistics.
OperationStats compress_unix_compress(const std::vector<std::filesystem::path>& sources,
                                      const std::filesystem::path& output_archive,
                                      const ProgressCallback& progress_callback) {
    if (sources.size() != 1U) {
        throw ArchiveError("Unix Compress compatibility requires exactly one regular-file source");
    }
    return compress_unix_compress_file(sources.front(), output_archive, progress_callback);
}

// Purpose: Extract one Unix Compress `.Z` stream into a destination directory.
// Inputs: `archive_path` is the source stream, `destination` is the output root, and `overwrite` permits replacement.
// Outputs: Publishes one validated decoded file and returns operation statistics; throws on malformed or unsafe input.
OperationStats extract_unix_compress_file(const std::filesystem::path& archive_path,
                                          const std::filesystem::path& destination, bool overwrite,
                                          const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open Unix Compress archive: " + archive_path.string());
    }
    const auto stream = read_unix_compress_header(input, archive_path, archive_size);

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
        progress.start(OperationKind::Extract, stream.payload_bytes, 1);
        progress.set_current(entry_name);
        publish_progress(progress, progress_callback);

        output_size = decode_unix_compress_payload(input, output, stream, progress, progress_callback);

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
