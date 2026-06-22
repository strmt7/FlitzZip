#include "hqx/hqx_adapter.hpp"

#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/resource_limit_checks.hpp"
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
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kMaxHqxPreambleBytes = 64U * 1024U;
constexpr std::uint8_t kHqxRleMarker = 0x90U;
constexpr std::string_view kHqxComment = "(This file must be converted with BinHex 4.0)";
constexpr std::string_view kHqxAlphabet = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";
static_assert(kHqxAlphabet.size() == 64U);

enum class HqxParseState {
    Header,
    HeaderCrcHigh,
    HeaderCrcLow,
    Data,
    DataCrcHigh,
    DataCrcLow,
    Resource,
    ResourceCrcHigh,
    ResourceCrcLow,
    Done,
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

// Purpose: Update the BinHex CRC-16 register with one byte using the documented bit order.
// Inputs: `crc` is the mutable register and `byte` is the next data byte.
// Outputs: Mutates `crc` without allocation or I/O.
void update_hqx_crc(std::uint16_t& crc, std::uint8_t byte) {
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
        const bool carried = (crc & 0x8000U) != 0U;
        crc = static_cast<std::uint16_t>((crc << 1U) | ((byte >> 7U) & 0x01U));
        if (carried) {
            crc = static_cast<std::uint16_t>(crc ^ 0x1021U);
        }
        byte = static_cast<std::uint8_t>(byte << 1U);
    }
}

// Purpose: Finalize a BinHex CRC by processing the two zero bytes included by the format.
// Inputs: `crc` is the running register before the encoded CRC field.
// Outputs: Returns the finalized CRC value expected in the stream.
std::uint16_t finalized_hqx_crc(std::uint16_t crc) {
    update_hqx_crc(crc, 0U);
    update_hqx_crc(crc, 0U);
    return crc;
}

// Purpose: Compute a finalized BinHex CRC for a contiguous metadata byte block.
// Inputs: `bytes` contains untrusted decoded header bytes excluding the stored CRC.
// Outputs: Returns the finalized CRC value expected after `bytes`.
std::uint16_t hqx_crc_for(std::span<const unsigned char> bytes) {
    std::uint16_t crc = 0;
    for (const auto byte : bytes) {
        update_hqx_crc(crc, static_cast<std::uint8_t>(byte));
    }
    return finalized_hqx_crc(crc);
}

// Purpose: Read a big-endian 32-bit integer from decoded BinHex header bytes.
// Inputs: `bytes` is the header buffer and `offset` is the first of four bytes.
// Outputs: Returns the unsigned integer or throws when the header is truncated.
std::uint32_t read_be_u32(std::span<const unsigned char> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw ArchiveError("BinHex header is truncated");
    }
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3U]);
}

// Purpose: Decode one HQX alphabet character into its six-bit value.
// Inputs: `ch` is a non-whitespace encoded character from the stream body.
// Outputs: Returns the six-bit value, or throws for bytes outside the HQX alphabet.
std::uint8_t decode_hqx_six(char ch) {
    const auto found = kHqxAlphabet.find(ch);
    if (found == std::string_view::npos) {
        throw ArchiveError("BinHex stream contains an invalid HQX character");
    }
    return static_cast<std::uint8_t>(found);
}

class HqxExtractParser {
  public:
    // Purpose: Build a parser that publishes exactly one validated data fork.
    // Inputs: `destination` is the extraction root, `overwrite` controls target replacement, and `progress` is updated
    // after header validation. Outputs: Initializes parser state; filesystem output is not opened until the trusted
    // header is complete.
    HqxExtractParser(const std::filesystem::path& destination, bool overwrite, ProgressState& progress)
        : destination_(destination), overwrite_(overwrite), progress_(progress) {}

    // Purpose: Consume one decoded and RLE-expanded BinHex byte.
    // Inputs: `byte` is one logical byte from the header, data fork, resource fork, or CRC fields.
    // Outputs: Mutates parse state, writes only verified-length data fork bytes to a private temp file, or throws on
    // invalid structure.
    void feed(std::uint8_t byte) {
        switch (state_) {
        case HqxParseState::Header:
            feed_header_byte(byte);
            return;
        case HqxParseState::HeaderCrcHigh:
            header_crc_expected_ = static_cast<std::uint16_t>(byte << 8U);
            state_ = HqxParseState::HeaderCrcLow;
            return;
        case HqxParseState::HeaderCrcLow:
            header_crc_expected_ = static_cast<std::uint16_t>(header_crc_expected_ | byte);
            validate_header_crc_and_open_output();
            return;
        case HqxParseState::Data:
            feed_data_byte(byte);
            return;
        case HqxParseState::DataCrcHigh:
            data_crc_expected_ = static_cast<std::uint16_t>(byte << 8U);
            state_ = HqxParseState::DataCrcLow;
            return;
        case HqxParseState::DataCrcLow:
            data_crc_expected_ = static_cast<std::uint16_t>(data_crc_expected_ | byte);
            validate_data_crc();
            return;
        case HqxParseState::Resource:
            feed_resource_byte(byte);
            return;
        case HqxParseState::ResourceCrcHigh:
            resource_crc_expected_ = static_cast<std::uint16_t>(byte << 8U);
            state_ = HqxParseState::ResourceCrcLow;
            return;
        case HqxParseState::ResourceCrcLow:
            resource_crc_expected_ = static_cast<std::uint16_t>(resource_crc_expected_ | byte);
            validate_resource_crc();
            return;
        case HqxParseState::Done:
            throw ArchiveError("BinHex stream contains decoded data after the resource fork");
        }
    }

    // Purpose: Finalize extraction after the terminating HQX colon has been reached.
    // Inputs: None; the parser must already be in the done state.
    // Outputs: Publishes the verified data fork into the destination or throws without leaving a final partial file.
    void finish() {
        if (state_ != HqxParseState::Done) {
            throw ArchiveError("BinHex stream ended before all CRC-checked sections were complete");
        }
        output_.close();
        if (!output_) {
            throw ArchiveError("failed to finalize BinHex extraction target: " + target_.string());
        }
        commit_verified_file(temporary_.file, target_, overwrite_);
        cleanup_file_publish_target(temporary_);
        temporary_active_ = false;
    }

    // Purpose: Remove any private temporary output reserved by this parser.
    // Inputs: None.
    // Outputs: Best-effort cleanup of the reserved temp target if publication did not complete.
    void cleanup() {
        if (output_.is_open()) {
            output_.close();
        }
        if (temporary_active_) {
            cleanup_file_publish_target(temporary_);
            temporary_active_ = false;
        }
    }

    // Purpose: Return the number of data-fork bytes written to the temporary output.
    // Inputs: None.
    // Outputs: Returns the validated data-fork byte count for operation statistics.
    [[nodiscard]] std::uint64_t output_size() const {
        return output_size_;
    }

  private:
    // Purpose: Consume one header byte and detect the final header metadata boundary.
    // Inputs: `byte` is a decoded header byte from untrusted input.
    // Outputs: Appends to the bounded header buffer and advances to header CRC state when complete.
    void feed_header_byte(std::uint8_t byte) {
        if (header_bytes_.empty()) {
            if (byte == 0U || byte > 63U) {
                throw ArchiveError("BinHex header filename length is outside 1..63 bytes");
            }
            expected_header_bytes_ = static_cast<std::size_t>(byte) + 20U;
        }
        if (header_bytes_.size() >= expected_header_bytes_) {
            throw ArchiveError("BinHex header exceeds its declared size");
        }
        header_bytes_.push_back(byte);
        if (header_bytes_.size() == expected_header_bytes_) {
            parse_header_metadata();
            state_ = HqxParseState::HeaderCrcHigh;
        }
    }

    // Purpose: Parse trusted-size header fields without validating the stored header CRC yet.
    // Inputs: `header_bytes_` contains exactly the decoded header body excluding the CRC.
    // Outputs: Sets entry name and fork lengths, or throws on unsafe names or malformed lengths.
    void parse_header_metadata() {
        const auto name_length = static_cast<std::size_t>(header_bytes_.front());
        const auto name_begin = header_bytes_.begin() + 1;
        const auto name_end = name_begin + static_cast<std::ptrdiff_t>(name_length);
        const std::string raw_name(name_begin, name_end);
        entry_name_ = normalize_archive_path_key(raw_name);

        const auto length_offset = 1U + name_length + 1U + 4U + 4U + 2U;
        data_length_ = read_be_u32(header_bytes_, length_offset);
        resource_length_ = read_be_u32(header_bytes_, length_offset + 4U);
    }

    // Purpose: Validate header CRC before opening any output file.
    // Inputs: Uses `header_bytes_` and the stored CRC just consumed from the stream.
    // Outputs: Creates the destination temp file and advances to data or data-CRC state.
    void validate_header_crc_and_open_output() {
        if (hqx_crc_for(header_bytes_) != header_crc_expected_) {
            throw ArchiveError("BinHex header CRC mismatch");
        }
        open_output_target();
        state_ = data_length_ == 0U ? HqxParseState::DataCrcHigh : HqxParseState::Data;
    }

    // Purpose: Reserve and open the private temporary output for the decoded data fork.
    // Inputs: Uses `destination_`, `entry_name_`, and `overwrite_`.
    // Outputs: Opens `output_` under a private temp path or throws before final-file publication.
    void open_output_target() {
        std::filesystem::create_directories(destination_);
        target_ = safe_join_archive_path(destination_, entry_name_);
        if (!overwrite_ && std::filesystem::exists(target_)) {
            throw SecurityError("refusing to overwrite existing BinHex extraction target: " + target_.string());
        }
        std::filesystem::create_directories(target_.parent_path());
        temporary_ = reserve_file_publish_target(target_);
        temporary_active_ = true;
        output_.open(temporary_.file, std::ios::binary | std::ios::trunc);
        if (!output_) {
            throw ArchiveError("cannot create BinHex extraction target: " + target_.string());
        }
        progress_.set_current(entry_name_);
    }

    // Purpose: Write one data-fork byte and update the data CRC.
    // Inputs: `byte` is a decoded data-fork byte.
    // Outputs: Writes to the private temp file and advances to data CRC state at the declared length.
    void feed_data_byte(std::uint8_t byte) {
        if (data_consumed_ >= data_length_) {
            throw ArchiveError("BinHex data fork exceeds its declared length");
        }
        output_.put(static_cast<char>(byte));
        if (!output_) {
            throw ArchiveError("failed to write BinHex extraction target: " + target_.string());
        }
        update_hqx_crc(data_crc_, byte);
        ++data_consumed_;
        output_size_ = checked_add_extracted_output_bytes(output_size_, 1U, "BinHex output");
        if (data_consumed_ == data_length_) {
            state_ = HqxParseState::DataCrcHigh;
        }
    }

    // Purpose: Verify the data-fork CRC and move to resource-fork validation.
    // Inputs: Uses the running data CRC and the stored CRC consumed from the stream.
    // Outputs: Advances to resource byte or resource-CRC state, or throws on mismatch.
    void validate_data_crc() {
        if (finalized_hqx_crc(data_crc_) != data_crc_expected_) {
            throw ArchiveError("BinHex data fork CRC mismatch");
        }
        state_ = resource_length_ == 0U ? HqxParseState::ResourceCrcHigh : HqxParseState::Resource;
    }

    // Purpose: Consume one resource-fork byte for integrity validation without publishing it.
    // Inputs: `byte` is decoded resource-fork data from the archive.
    // Outputs: Updates resource CRC and advances at the declared length; no resource fork is written on Windows.
    void feed_resource_byte(std::uint8_t byte) {
        if (resource_consumed_ >= resource_length_) {
            throw ArchiveError("BinHex resource fork exceeds its declared length");
        }
        update_hqx_crc(resource_crc_, byte);
        ++resource_consumed_;
        if (resource_consumed_ == resource_length_) {
            state_ = HqxParseState::ResourceCrcHigh;
        }
    }

    // Purpose: Verify the resource-fork CRC after discarding resource bytes.
    // Inputs: Uses the running resource CRC and the stored CRC consumed from the stream.
    // Outputs: Marks the archive body complete or throws on mismatch.
    void validate_resource_crc() {
        if (finalized_hqx_crc(resource_crc_) != resource_crc_expected_) {
            throw ArchiveError("BinHex resource fork CRC mismatch");
        }
        state_ = HqxParseState::Done;
    }

    const std::filesystem::path& destination_;
    bool overwrite_ = false;
    ProgressState& progress_;
    HqxParseState state_ = HqxParseState::Header;
    std::vector<unsigned char> header_bytes_;
    std::size_t expected_header_bytes_ = 0;
    std::string entry_name_;
    std::filesystem::path target_;
    ReservedFilePublishTarget temporary_;
    bool temporary_active_ = false;
    std::ofstream output_;
    std::uint32_t data_length_ = 0;
    std::uint32_t resource_length_ = 0;
    std::uint32_t data_consumed_ = 0;
    std::uint32_t resource_consumed_ = 0;
    std::uint16_t header_crc_expected_ = 0;
    std::uint16_t data_crc_ = 0;
    std::uint16_t data_crc_expected_ = 0;
    std::uint16_t resource_crc_ = 0;
    std::uint16_t resource_crc_expected_ = 0;
    std::uint64_t output_size_ = 0;
};

class HqxRleDecoder {
  public:
    // Purpose: Build an RLE decoder that forwards expanded bytes to a BinHex parser.
    // Inputs: `parser` receives decoded logical bytes after RLE expansion.
    // Outputs: Initializes decoder state without taking ownership of `parser`.
    explicit HqxRleDecoder(HqxExtractParser& parser) : parser_(parser) {}

    // Purpose: Consume one byte from the six-bit HQX layer and expand BinHex RLE markers.
    // Inputs: `byte` is one pre-RLE byte.
    // Outputs: Forwards zero or more expanded bytes to `parser`; throws on malformed marker sequences.
    void feed(std::uint8_t byte) {
        if (pending_marker_) {
            feed_marker_count(byte);
            return;
        }
        if (byte == kHqxRleMarker) {
            pending_marker_ = true;
            return;
        }
        emit(byte);
    }

    // Purpose: Finalize RLE decoding at the end of the HQX body.
    // Inputs: None.
    // Outputs: Throws if the stream ended after a marker without a count byte.
    void finish() const {
        if (pending_marker_) {
            throw ArchiveError("BinHex RLE marker is missing its repeat count");
        }
    }

  private:
    // Purpose: Consume the count byte that follows an RLE marker.
    // Inputs: `count` is zero for a literal marker or a total repeat count for the previous byte.
    // Outputs: Emits the literal marker or repeated bytes, or throws when no previous byte exists.
    void feed_marker_count(std::uint8_t count) {
        pending_marker_ = false;
        if (count == 0U) {
            emit(kHqxRleMarker);
            return;
        }
        if (!have_previous_) {
            throw ArchiveError("BinHex RLE repeat appears before any byte");
        }
        for (std::uint16_t i = 1U; i < count; ++i) {
            emit(previous_);
        }
    }

    // Purpose: Forward one expanded byte and remember it for future RLE repeats.
    // Inputs: `byte` is the decoded logical byte.
    // Outputs: Sends `byte` to the parser and updates repeat state.
    void emit(std::uint8_t byte) {
        parser_.feed(byte);
        previous_ = byte;
        have_previous_ = true;
    }

    HqxExtractParser& parser_;
    bool pending_marker_ = false;
    bool have_previous_ = false;
    std::uint8_t previous_ = 0;
};

class HqxSixBitDecoder {
  public:
    // Purpose: Build a six-bit HQX decoder that forwards bytes into the RLE layer.
    // Inputs: `rle` receives decoded pre-RLE bytes.
    // Outputs: Initializes group state without taking ownership of `rle`.
    explicit HqxSixBitDecoder(HqxRleDecoder& rle) : rle_(rle) {}

    // Purpose: Consume one HQX alphabet character.
    // Inputs: `ch` is a validated non-whitespace body character before the terminating colon.
    // Outputs: Emits one group of decoded bytes each time four characters are available.
    void feed(char ch) {
        sextets_[sextet_count_++] = decode_hqx_six(ch);
        if (sextet_count_ == sextets_.size()) {
            emit_three_bytes();
            sextet_count_ = 0;
        }
    }

    // Purpose: Finalize a partial six-bit group at the terminating colon.
    // Inputs: None.
    // Outputs: Emits the remaining one or two bytes for legal group sizes, then finalizes RLE.
    void finish() {
        if (sextet_count_ == 1U) {
            throw ArchiveError("BinHex HQX body has an invalid trailing six-bit group");
        }
        if (sextet_count_ == 2U) {
            rle_.feed(static_cast<std::uint8_t>((sextets_[0] << 2U) | (sextets_[1] >> 4U)));
        } else if (sextet_count_ == 3U) {
            rle_.feed(static_cast<std::uint8_t>((sextets_[0] << 2U) | (sextets_[1] >> 4U)));
            rle_.feed(static_cast<std::uint8_t>((sextets_[1] << 4U) | (sextets_[2] >> 2U)));
        }
        rle_.finish();
    }

  private:
    // Purpose: Emit three decoded bytes from a full four-character HQX group.
    // Inputs: Uses `sextets_`, which must contain four values.
    // Outputs: Forwards three bytes to the RLE decoder.
    void emit_three_bytes() {
        rle_.feed(static_cast<std::uint8_t>((sextets_[0] << 2U) | (sextets_[1] >> 4U)));
        rle_.feed(static_cast<std::uint8_t>((sextets_[1] << 4U) | (sextets_[2] >> 2U)));
        rle_.feed(static_cast<std::uint8_t>((sextets_[2] << 6U) | sextets_[3]));
    }

    HqxRleDecoder& rle_;
    std::array<std::uint8_t, 4> sextets_{};
    std::size_t sextet_count_ = 0;
};

// Purpose: Test whether one byte is ASCII whitespace accepted between HQX body characters.
// Inputs: `byte` is an untrusted stream byte.
// Outputs: Returns true for spaces, tabs, CR/LF, and other C-locale whitespace bytes.
bool is_hqx_whitespace(unsigned char byte) {
    return std::isspace(byte) != 0;
}

// Purpose: Scan to the required BinHex 4.0 comment and payload-start colon.
// Inputs: `input` is positioned at stream start.
// Outputs: Leaves `input` positioned immediately after the payload-start colon, or throws on missing/malformed
// preamble.
void seek_hqx_payload_start(std::ifstream& input) {
    std::size_t matched = 0;
    for (std::size_t scanned = 0; scanned < kMaxHqxPreambleBytes; ++scanned) {
        const auto next = input.get();
        if (next == std::char_traits<char>::eof()) {
            throw ArchiveError("BinHex stream ended before the required header comment");
        }
        const auto ch = std::char_traits<char>::to_char_type(next);
        if (ch == kHqxComment[matched]) {
            ++matched;
            if (matched == kHqxComment.size()) {
                break;
            }
            continue;
        }
        matched = ch == kHqxComment.front() ? 1U : 0U;
    }
    if (matched != kHqxComment.size()) {
        throw ArchiveError("BinHex preamble exceeds SuperZip metadata limit");
    }
    for (;;) {
        const auto next = input.get();
        if (next == std::char_traits<char>::eof()) {
            throw ArchiveError("BinHex stream ended before the payload-start colon");
        }
        const auto byte = static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        if (byte == static_cast<unsigned char>(':')) {
            return;
        }
        if (!is_hqx_whitespace(byte)) {
            throw ArchiveError("BinHex header comment is not followed by a payload-start colon");
        }
    }
}

// Purpose: Decode the HQX payload body into one data-fork parser.
// Inputs: `input` is positioned after the payload-start colon and `parser` receives logical bytes.
// Outputs: Stops after the terminating colon and rejects non-whitespace trailing data.
void decode_hqx_payload(std::ifstream& input, HqxExtractParser& parser) {
    HqxRleDecoder rle(parser);
    HqxSixBitDecoder decoder(rle);
    bool saw_end_colon = false;
    for (;;) {
        const auto next = input.get();
        if (next == std::char_traits<char>::eof()) {
            break;
        }
        const auto byte = static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        if (byte == static_cast<unsigned char>(':')) {
            saw_end_colon = true;
            break;
        }
        if (is_hqx_whitespace(byte)) {
            continue;
        }
        decoder.feed(static_cast<char>(byte));
    }
    if (!saw_end_colon) {
        throw ArchiveError("BinHex stream ended before the terminating colon");
    }
    decoder.finish();
    std::istream::int_type next = std::char_traits<char>::eof();
    while (!std::char_traits<char>::eq_int_type((next = input.get()), std::char_traits<char>::eof())) {
        const auto byte = static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        if (!is_hqx_whitespace(byte)) {
            throw ArchiveError("BinHex stream contains trailing data after the terminating colon");
        }
    }
    if (input.bad()) {
        throw ArchiveError("failed to read BinHex archive");
    }
}

}  // namespace

// Purpose: Extract the data fork from one BinHex 4.0 `.hqx` stream with path-safe publication.
// Inputs: `archive_path` is the `.hqx` stream, `destination` is the extraction root, `overwrite` controls replacement,
// and `progress_callback` receives synchronous progress snapshots. Outputs: Returns operation statistics; throws on
// malformed HQX text, CRC mismatch, unsafe header path, refused overwrite, or verified-file publication failure.
OperationStats extract_hqx_file(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                                bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_size = regular_file_size(archive_path);
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open BinHex archive: " + archive_path.string());
    }

    ProgressState progress;
    progress.start(OperationKind::Extract, archive_size, 1);
    progress.set_current("BinHex data fork");
    publish_progress(progress, progress_callback);

    HqxExtractParser parser(destination, overwrite, progress);
    try {
        seek_hqx_payload_start(input);
        decode_hqx_payload(input, parser);
        parser.finish();
    } catch (...) {
        parser.cleanup();
        throw;
    }

    progress.add_bytes(archive_size);
    progress.finish_entry();
    publish_progress(progress, progress_callback);

    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = parser.output_size();
    stats.entries = 1;
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
