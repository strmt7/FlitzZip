#include "gzip/gzip_stream.hpp"

#include "core/resource_limit_checks.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <fstream>
#include <limits>
#include <streambuf>
#include <string>

#include "miniz.h"

namespace superzip {
namespace {

constexpr std::size_t kGzipStreamBufferBytes = 64U * 1024U;
constexpr std::uint8_t kGzipFlagText = 0x01U;
constexpr std::uint8_t kGzipFlagHeaderCrc = 0x02U;
constexpr std::uint8_t kGzipFlagExtra = 0x04U;
constexpr std::uint8_t kGzipFlagName = 0x08U;
constexpr std::uint8_t kGzipFlagComment = 0x10U;
constexpr std::uint8_t kGzipReservedFlags = 0xE0U;

struct ParsedGzipHeader {
    std::uint64_t compressed_offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint32_t expected_crc32 = 0;
    std::uint32_t expected_isize = 0;
};

// Purpose: Add byte counts while detecting unsigned wraparound.
// Inputs: `total` is mutated by adding `bytes`; `context` identifies the counter for diagnostics.
// Outputs: Updates `total`, or throws before wraparound.
void checked_add_stream_bytes(std::uint64_t& total, std::uint64_t bytes, const char* context) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
        throw ArchiveError(std::string(context) + " byte count overflows");
    }
    total += bytes;
}

// Purpose: Read a filesystem file size into a 64-bit archive counter.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
std::uint64_t gzip_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read Gzip file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("Gzip file size exceeds SuperZip limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

// Purpose: Write all bytes to a binary output stream while updating a telemetry counter.
// Inputs: `output` is the destination stream, `bytes` points to data, `size` is the byte count, and `written` is
// updated. Outputs: Appends bytes or throws on stream failure.
void write_counted(std::ofstream& output, const unsigned char* bytes, std::size_t size, std::uint64_t& written) {
    if (size == 0U) {
        return;
    }
    output.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!output) {
        throw ArchiveError("failed to write Gzip stream");
    }
    checked_add_stream_bytes(written, size, "Gzip output");
}

// Purpose: Validate a product compression level before passing it to miniz.
// Inputs: `compression_level` is the caller-selected 1-9 level.
// Outputs: Returns the validated miniz level or throws `ArchiveError`.
int gzip_compression_level(int compression_level) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("Gzip compression level must be between 1 and 9");
    }
    return compression_level;
}

// Purpose: Write a little-endian 32-bit Gzip trailer field.
// Inputs: `output` is the destination stream, `value` is the field, and `written` is updated.
// Outputs: Appends four bytes or throws on stream failure.
void write_gzip_le32(std::ofstream& output, std::uint32_t value, std::uint64_t& written) {
    const std::array<unsigned char, 4> bytes{
        static_cast<unsigned char>(value & 0xFFU),
        static_cast<unsigned char>((value >> 8U) & 0xFFU),
        static_cast<unsigned char>((value >> 16U) & 0xFFU),
        static_cast<unsigned char>((value >> 24U) & 0xFFU),
    };
    write_counted(output, bytes.data(), bytes.size(), written);
}

// Purpose: Read a little-endian 32-bit Gzip trailer field.
// Inputs: `bytes` points to at least four bytes.
// Outputs: Returns the decoded value.
std::uint32_t read_gzip_le32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

// Purpose: Convert an unsigned file offset to the signed iostream offset type.
// Inputs: `value` is a byte offset and `context` labels diagnostics.
// Outputs: Returns a representable `std::streamoff`, or throws.
std::streamoff to_gzip_streamoff(std::uint64_t value, const char* context) {
    const auto max_streamoff = static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (value > max_streamoff) {
        throw ArchiveError(std::string(context) + " offset exceeds host stream limits");
    }
    return static_cast<std::streamoff>(value);
}

// Purpose: Seek a Gzip source stream to an absolute byte offset.
// Inputs: `input` is the source stream, `offset` is the target byte position, and `context` labels diagnostics.
// Outputs: Positions the stream or throws on seek failure.
void seek_gzip_input(std::ifstream& input, std::uint64_t offset, const char* context) {
    input.seekg(to_gzip_streamoff(offset, context), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek Gzip ") + context);
    }
}

// Purpose: Advance over a bounded optional Gzip header field.
// Inputs: `input` is positioned by caller, `offset` is updated, `bytes` is the skip count, `limit` is the
// compressed-data limit, and `field` names the field. Outputs: Advances position, or throws when the field overlaps
// payload/trailer.
void skip_gzip_header_bytes(std::ifstream& input, std::uint64_t& offset, std::uint64_t bytes, std::uint64_t limit,
                            const char* field) {
    if (bytes > limit - offset) {
        throw ArchiveError(std::string("Gzip ") + field + " field exceeds header bounds");
    }
    offset += bytes;
    seek_gzip_input(input, offset, field);
}

// Purpose: Skip a bounded zero-terminated optional Gzip header string.
// Inputs: `input` is positioned at the field, `offset` is updated, `limit` is the compressed-data limit, and `field`
// names the field. Outputs: Positions after the NUL terminator, or throws on malformed metadata.
void skip_gzip_zero_string(std::ifstream& input, std::uint64_t& offset, std::uint64_t limit, const char* field) {
    while (offset < limit) {
        char value = 0;
        input.read(&value, 1);
        if (!input) {
            throw ArchiveError(std::string("failed to read Gzip ") + field + " field");
        }
        ++offset;
        if (value == '\0') {
            return;
        }
    }
    throw ArchiveError(std::string("Gzip ") + field + " field is not terminated");
}

// Purpose: Parse a single-member Gzip wrapper while ignoring untrusted optional names.
// Inputs: `input` is a seekable binary Gzip file and `file_size` is its total byte size.
// Outputs: Returns compressed payload bounds and trailer expectations.
ParsedGzipHeader parse_gzip_stream_header(std::ifstream& input, std::uint64_t file_size) {
    if (file_size < 18U) {
        throw ArchiveError("Gzip stream is too small");
    }
    const auto compressed_limit = file_size - 8U;
    std::array<unsigned char, 10> header{};
    seek_gzip_input(input, 0, "header");
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input) {
        throw ArchiveError("failed to read Gzip header");
    }
    if (header[0] != 0x1FU || header[1] != 0x8BU || header[2] != 8U) {
        throw ArchiveError("invalid Gzip header");
    }
    const auto flags = header[3];
    if ((flags & kGzipReservedFlags) != 0U) {
        throw ArchiveError("Gzip header uses reserved flags");
    }
    // FTEXT is advisory only; stream extraction remains binary-safe regardless of it.

    std::uint64_t offset = header.size();
    if ((flags & kGzipFlagExtra) != 0U) {
        if (compressed_limit - offset < 2U) {
            throw ArchiveError("Gzip extra field length exceeds header bounds");
        }
        std::array<unsigned char, 2> extra_size{};
        input.read(reinterpret_cast<char*>(extra_size.data()), static_cast<std::streamsize>(extra_size.size()));
        if (!input) {
            throw ArchiveError("failed to read Gzip extra field length");
        }
        offset += extra_size.size();
        const auto bytes =
            static_cast<std::uint64_t>(extra_size[0]) | (static_cast<std::uint64_t>(extra_size[1]) << 8U);
        skip_gzip_header_bytes(input, offset, bytes, compressed_limit, "extra");
    }
    if ((flags & kGzipFlagName) != 0U) {
        skip_gzip_zero_string(input, offset, compressed_limit, "name");
    }
    if ((flags & kGzipFlagComment) != 0U) {
        skip_gzip_zero_string(input, offset, compressed_limit, "comment");
    }
    if ((flags & kGzipFlagHeaderCrc) != 0U) {
        skip_gzip_header_bytes(input, offset, 2U, compressed_limit, "header CRC");
    }
    if (offset >= compressed_limit) {
        throw ArchiveError("Gzip stream has no compressed payload");
    }

    std::array<unsigned char, 8> trailer{};
    seek_gzip_input(input, compressed_limit, "trailer");
    input.read(reinterpret_cast<char*>(trailer.data()), static_cast<std::streamsize>(trailer.size()));
    if (!input) {
        throw ArchiveError("failed to read Gzip trailer");
    }
    return ParsedGzipHeader{
        .compressed_offset = offset,
        .compressed_size = compressed_limit - offset,
        .expected_crc32 = read_gzip_le32(trailer.data()),
        .expected_isize = read_gzip_le32(trailer.data() + 4),
    };
}

}  // namespace

class GzipOutputStream::Buffer final : public std::streambuf {
  public:
    // Purpose: Open a Gzip output file and initialize the raw-deflate encoder.
    // Inputs: `output_path` is the destination and `compression_level` is validated as 1-9.
    // Outputs: Creates the wrapper header and active compressor or throws on I/O/encoder failure.
    explicit Buffer(const std::filesystem::path& output_path, int compression_level)
        : output_(output_path, std::ios::binary | std::ios::trunc) {
        if (!output_) {
            throw ArchiveError("cannot create Gzip stream: " + output_path.string());
        }
        const std::array<unsigned char, 10> header{0x1F, 0x8B, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 255U};
        write_counted(output_, header.data(), header.size(), output_bytes_);
        if (mz_deflateInit2(&stream_, gzip_compression_level(compression_level), MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS,
                            9, MZ_DEFAULT_STRATEGY) != MZ_OK) {
            throw ArchiveError("failed to initialize Gzip compressor");
        }
        stream_active_ = true;
        crc32_ = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, nullptr, 0));
    }

    ~Buffer() override {
        try {
            close();
        } catch (...) {
            if (stream_active_) {
                mz_deflateEnd(&stream_);
                stream_active_ = false;
            }
        }
    }

    // Purpose: Finish compression and write the Gzip trailer.
    // Inputs: None.
    // Outputs: Closes compressor and output file; repeated calls are no-ops.
    void close() {
        if (closed_) {
            return;
        }
        int status = MZ_OK;
        do {
            stream_.next_out = output_buffer_.data();
            stream_.avail_out = static_cast<unsigned int>(output_buffer_.size());
            status = mz_deflate(&stream_, MZ_FINISH);
            if (status != MZ_OK && status != MZ_STREAM_END) {
                throw ArchiveError("Gzip compression failed during finalization");
            }
            const auto produced = output_buffer_.size() - stream_.avail_out;
            write_counted(output_, output_buffer_.data(), produced, output_bytes_);
        } while (status != MZ_STREAM_END);
        mz_deflateEnd(&stream_);
        stream_active_ = false;
        write_gzip_le32(output_, crc32_, output_bytes_);
        write_gzip_le32(output_, static_cast<std::uint32_t>(input_bytes_ & 0xFFFFFFFFULL), output_bytes_);
        output_.close();
        if (!output_) {
            throw ArchiveError("failed to finalize Gzip stream");
        }
        closed_ = true;
    }

    // Purpose: Report uncompressed byte count accepted by the compressor.
    // Inputs: None.
    // Outputs: Returns the uncompressed byte count.
    [[nodiscard]] std::uint64_t input_bytes() const {
        return input_bytes_;
    }

    // Purpose: Report compressed byte count written to disk.
    // Inputs: None.
    // Outputs: Returns bytes written, including header and trailer.
    [[nodiscard]] std::uint64_t output_bytes() const {
        return output_bytes_;
    }

  protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) {
            return traits_type::not_eof(ch);
        }
        const auto byte = static_cast<unsigned char>(traits_type::to_char_type(ch));
        compress_bytes(&byte, 1U);
        return ch;
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0) {
            return 0;
        }
        compress_bytes(reinterpret_cast<const unsigned char*>(data), static_cast<std::size_t>(count));
        return count;
    }

    int sync() override {
        return output_ ? 0 : -1;
    }

  private:
    // Purpose: Deflate one caller-provided uncompressed byte range.
    // Inputs: `data` points to bytes and `size` is the byte count.
    // Outputs: Writes compressed bytes to `output_` and updates CRC/ISIZE state.
    void compress_bytes(const unsigned char* data, std::size_t size) {
        if (closed_) {
            throw ArchiveError("cannot write to a closed Gzip stream");
        }
        crc32_ = static_cast<std::uint32_t>(mz_crc32(crc32_, data, size));
        checked_add_stream_bytes(input_bytes_, size, "Gzip input");
        std::size_t offset = 0;
        while (offset < size) {
            const auto chunk =
                std::min<std::size_t>({size - offset, input_buffer_.size(), static_cast<std::size_t>(UINT_MAX)});
            std::copy_n(data + offset, chunk, input_buffer_.data());
            stream_.next_in = input_buffer_.data();
            stream_.avail_in = static_cast<unsigned int>(chunk);
            while (stream_.avail_in > 0U) {
                stream_.next_out = output_buffer_.data();
                stream_.avail_out = static_cast<unsigned int>(output_buffer_.size());
                const auto status = mz_deflate(&stream_, MZ_NO_FLUSH);
                if (status != MZ_OK) {
                    throw ArchiveError("Gzip compression failed");
                }
                const auto produced = output_buffer_.size() - stream_.avail_out;
                write_counted(output_, output_buffer_.data(), produced, output_bytes_);
            }
            offset += chunk;
        }
    }

    std::ofstream output_;
    mz_stream stream_{};
    bool stream_active_ = false;
    bool closed_ = false;
    std::array<unsigned char, kGzipStreamBufferBytes> input_buffer_{};
    std::array<unsigned char, kGzipStreamBufferBytes> output_buffer_{};
    std::uint32_t crc32_ = 0;
    std::uint64_t input_bytes_ = 0;
    std::uint64_t output_bytes_ = 0;
};

class GzipInputStream::Buffer final : public std::streambuf {
  public:
    // Purpose: Open and initialize a bounded raw-deflate reader for one Gzip stream.
    // Inputs: `archive_path` is an existing `.gz` stream with a validated header/trailer.
    // Outputs: Prepares the get area and decompressor state, or throws on I/O/format failure.
    explicit Buffer(const std::filesystem::path& archive_path)
        : input_(archive_path, std::ios::binary), archive_size_(gzip_file_size(archive_path)) {
        if (!input_) {
            throw ArchiveError("cannot open Gzip stream: " + archive_path.string());
        }
        header_ = parse_gzip_stream_header(input_, archive_size_);
        seek_gzip_input(input_, header_.compressed_offset, "payload");
        compressed_remaining_ = header_.compressed_size;
        if (mz_inflateInit2(&stream_, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
            throw ArchiveError("failed to initialize Gzip decompressor");
        }
        stream_active_ = true;
        crc32_ = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, nullptr, 0));
        setg(reinterpret_cast<char*>(output_buffer_.data()), reinterpret_cast<char*>(output_buffer_.data()),
             reinterpret_cast<char*>(output_buffer_.data()));
    }

    ~Buffer() override {
        if (stream_active_) {
            mz_inflateEnd(&stream_);
        }
    }

    // Purpose: Drain all remaining uncompressed bytes and force trailer validation.
    // Inputs: None.
    // Outputs: Throws when the Gzip member is incomplete or invalid.
    void finish() {
        while (!finished_) {
            setg(reinterpret_cast<char*>(output_buffer_.data()), reinterpret_cast<char*>(output_buffer_.data()),
                 reinterpret_cast<char*>(output_buffer_.data()));
            fill_output();
        }
    }

    // Purpose: Report compressed source byte size.
    // Inputs: None.
    // Outputs: Returns the `.gz` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const {
        return archive_size_;
    }

    // Purpose: Report uncompressed bytes produced by the inflater.
    // Inputs: None.
    // Outputs: Returns the uncompressed byte count emitted so far.
    [[nodiscard]] std::uint64_t output_bytes() const {
        return output_bytes_;
    }

  protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        setg(reinterpret_cast<char*>(output_buffer_.data()), reinterpret_cast<char*>(output_buffer_.data()),
             reinterpret_cast<char*>(output_buffer_.data()));
        fill_output();
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        return traits_type::eof();
    }

  private:
    // Purpose: Fill the get area with newly decompressed bytes or finish validation.
    // Inputs: None.
    // Outputs: Updates `setg` when bytes are produced; throws on malformed Gzip payloads.
    void fill_output() {
        if (finished_) {
            return;
        }
        while (true) {
            if (stream_.avail_in == 0U && compressed_remaining_ > 0U) {
                const auto to_read =
                    static_cast<std::size_t>(std::min<std::uint64_t>(input_buffer_.size(), compressed_remaining_));
                input_.read(reinterpret_cast<char*>(input_buffer_.data()), static_cast<std::streamsize>(to_read));
                if (static_cast<std::size_t>(input_.gcount()) != to_read) {
                    throw ArchiveError("Gzip compressed payload is truncated");
                }
                compressed_remaining_ -= to_read;
                stream_.next_in = input_buffer_.data();
                stream_.avail_in = static_cast<unsigned int>(to_read);
            }

            stream_.next_out = output_buffer_.data();
            stream_.avail_out = static_cast<unsigned int>(output_buffer_.size());
            const auto status = mz_inflate(&stream_, MZ_NO_FLUSH);
            if (status != MZ_OK && status != MZ_STREAM_END) {
                throw ArchiveError("Gzip decompression failed");
            }
            const auto produced = output_buffer_.size() - stream_.avail_out;
            if (produced > 0U) {
                crc32_ = static_cast<std::uint32_t>(mz_crc32(crc32_, output_buffer_.data(), produced));
                output_bytes_ = checked_add_extracted_output_bytes(output_bytes_, produced, "Gzip output");
                setg(reinterpret_cast<char*>(output_buffer_.data()), reinterpret_cast<char*>(output_buffer_.data()),
                     reinterpret_cast<char*>(output_buffer_.data() + produced));
                return;
            }
            if (status == MZ_STREAM_END) {
                if (stream_.avail_in != 0U || compressed_remaining_ != 0U) {
                    throw ArchiveError("Gzip stream contains trailing compressed data before the trailer");
                }
                validate_trailer();
                return;
            }
            if (stream_.avail_in == 0U && compressed_remaining_ == 0U) {
                throw ArchiveError("Gzip compressed payload ended before the deflate stream completed");
            }
        }
    }

    // Purpose: Validate the parsed Gzip trailer against decompressed output state.
    // Inputs: None.
    // Outputs: Marks the stream finished or throws on CRC/ISIZE mismatch.
    void validate_trailer() {
        if (crc32_ != header_.expected_crc32) {
            throw ArchiveError("Gzip CRC32 verification failed");
        }
        if (static_cast<std::uint32_t>(output_bytes_ & 0xFFFFFFFFULL) != header_.expected_isize) {
            throw ArchiveError("Gzip uncompressed-size verification failed");
        }
        if (stream_active_) {
            mz_inflateEnd(&stream_);
            stream_active_ = false;
        }
        finished_ = true;
    }

    std::ifstream input_;
    ParsedGzipHeader header_{};
    std::uint64_t archive_size_ = 0;
    std::uint64_t compressed_remaining_ = 0;
    mz_stream stream_{};
    bool stream_active_ = false;
    bool finished_ = false;
    std::array<unsigned char, kGzipStreamBufferBytes> input_buffer_{};
    std::array<unsigned char, kGzipStreamBufferBytes> output_buffer_{};
    std::uint32_t crc32_ = 0;
    std::uint64_t output_bytes_ = 0;
};

// Purpose: Construct an output stream that writes a complete Gzip member.
// Inputs: `output_path` is the target file and `compression_level` is the miniz 1-9 setting.
// Outputs: Installs an owned stream buffer or throws on setup failure.
GzipOutputStream::GzipOutputStream(const std::filesystem::path& output_path, int compression_level)
    : std::ostream(nullptr), buffer_(std::make_unique<Buffer>(output_path, compression_level)) {
    rdbuf(buffer_.get());
}

GzipOutputStream::~GzipOutputStream() {
    try {
        close();
    } catch (...) {
    }
}

void GzipOutputStream::close() {
    buffer_->close();
}

std::uint64_t GzipOutputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t GzipOutputStream::output_bytes() const {
    return buffer_->output_bytes();
}

GzipInputStream::GzipInputStream(const std::filesystem::path& archive_path)
    : std::istream(nullptr), buffer_(std::make_unique<Buffer>(archive_path)) {
    rdbuf(buffer_.get());
}

GzipInputStream::~GzipInputStream() = default;

void GzipInputStream::finish() {
    buffer_->finish();
    clear();
}

std::uint64_t GzipInputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t GzipInputStream::output_bytes() const {
    return buffer_->output_bytes();
}

}  // namespace superzip
