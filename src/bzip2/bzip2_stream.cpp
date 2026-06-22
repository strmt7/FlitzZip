#include "bzip2/bzip2_stream.hpp"

#include "core/resource_limit_checks.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <fstream>
#include <limits>
#include <streambuf>
#include <string>

#include "bzlib.h"

namespace superzip {
namespace {

constexpr std::size_t kBzip2StreamBufferBytes = 64U * 1024U;
constexpr int kBzip2Verbosity = 0;
constexpr int kBzip2WorkFactor = 30;

// Purpose: Convert a libbzip2 status code into an actionable diagnostic suffix.
// Inputs: `status` is a return code from the libbzip2 streaming API.
// Outputs: Returns a stable text label for error reporting.
const char* bzip2_status_name(int status) {
    switch (status) {
    case BZ_OK:
        return "BZ_OK";
    case BZ_RUN_OK:
        return "BZ_RUN_OK";
    case BZ_FLUSH_OK:
        return "BZ_FLUSH_OK";
    case BZ_FINISH_OK:
        return "BZ_FINISH_OK";
    case BZ_STREAM_END:
        return "BZ_STREAM_END";
    case BZ_SEQUENCE_ERROR:
        return "BZ_SEQUENCE_ERROR";
    case BZ_PARAM_ERROR:
        return "BZ_PARAM_ERROR";
    case BZ_MEM_ERROR:
        return "BZ_MEM_ERROR";
    case BZ_DATA_ERROR:
        return "BZ_DATA_ERROR";
    case BZ_DATA_ERROR_MAGIC:
        return "BZ_DATA_ERROR_MAGIC";
    case BZ_IO_ERROR:
        return "BZ_IO_ERROR";
    case BZ_UNEXPECTED_EOF:
        return "BZ_UNEXPECTED_EOF";
    case BZ_OUTBUFF_FULL:
        return "BZ_OUTBUFF_FULL";
    case BZ_CONFIG_ERROR:
        return "BZ_CONFIG_ERROR";
    default:
        return "unknown status";
    }
}

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
std::uint64_t bzip2_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read Bzip2 file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("Bzip2 file size exceeds SuperZip limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
}

// Purpose: Write all bytes to a binary output stream while updating a telemetry counter.
// Inputs: `output` is the destination stream, `bytes` points to data, `size` is the byte count, and `written` is
// updated. Outputs: Appends bytes or throws on stream failure.
void write_counted(std::ofstream& output, const char* bytes, std::size_t size, std::uint64_t& written) {
    if (size == 0U) {
        return;
    }
    output.write(bytes, static_cast<std::streamsize>(size));
    if (!output) {
        throw ArchiveError("failed to write Bzip2 stream");
    }
    checked_add_stream_bytes(written, size, "Bzip2 output");
}

// Purpose: Validate a product compression level before passing it to libbzip2.
// Inputs: `compression_level` is the caller-selected 1-9 block-size level.
// Outputs: Returns the validated libbzip2 block size or throws `ArchiveError`.
int bzip2_compression_level(int compression_level) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("Bzip2 compression level must be between 1 and 9");
    }
    return compression_level;
}

}  // namespace

class Bzip2OutputStream::Buffer final : public std::streambuf {
  public:
    explicit Buffer(const std::filesystem::path& output_path, int compression_level)
        : output_(output_path, std::ios::binary | std::ios::trunc) {
        if (!output_) {
            throw ArchiveError("cannot create Bzip2 stream: " + output_path.string());
        }
        const auto status =
            BZ2_bzCompressInit(&stream_, bzip2_compression_level(compression_level), kBzip2Verbosity, kBzip2WorkFactor);
        if (status != BZ_OK) {
            throw ArchiveError(std::string("failed to initialize Bzip2 compressor: ") + bzip2_status_name(status));
        }
        stream_active_ = true;
    }

    ~Buffer() override {
        try {
            close();
        } catch (...) {
            if (stream_active_) {
                BZ2_bzCompressEnd(&stream_);
                stream_active_ = false;
            }
        }
    }

    // Purpose: Finish compression and close the output file.
    // Inputs: None.
    // Outputs: Closes compressor and output file; repeated calls are no-ops.
    void close() {
        if (closed_) {
            return;
        }
        int status = BZ_FINISH_OK;
        do {
            stream_.next_out = output_buffer_.data();
            stream_.avail_out = static_cast<unsigned int>(output_buffer_.size());
            status = BZ2_bzCompress(&stream_, BZ_FINISH);
            if (status != BZ_FINISH_OK && status != BZ_STREAM_END) {
                throw ArchiveError(std::string("Bzip2 compression failed during finalization: ") +
                                   bzip2_status_name(status));
            }
            const auto produced = output_buffer_.size() - stream_.avail_out;
            write_counted(output_, output_buffer_.data(), produced, output_bytes_);
        } while (status != BZ_STREAM_END);
        BZ2_bzCompressEnd(&stream_);
        stream_active_ = false;
        output_.close();
        if (!output_) {
            throw ArchiveError("failed to finalize Bzip2 stream");
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
    // Outputs: Returns bytes written to the `.bz2` stream.
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
    // Purpose: Compress one caller-provided uncompressed byte range.
    // Inputs: `data` points to bytes and `size` is the byte count.
    // Outputs: Writes compressed bytes to `output_` and updates byte counters.
    void compress_bytes(const unsigned char* data, std::size_t size) {
        if (closed_) {
            throw ArchiveError("cannot write to a closed Bzip2 stream");
        }
        checked_add_stream_bytes(input_bytes_, size, "Bzip2 input");
        std::size_t offset = 0;
        while (offset < size) {
            const auto chunk =
                std::min<std::size_t>({size - offset, input_buffer_.size(), static_cast<std::size_t>(UINT_MAX)});
            std::copy_n(data + offset, chunk, input_buffer_.data());
            stream_.next_in = reinterpret_cast<char*>(input_buffer_.data());
            stream_.avail_in = static_cast<unsigned int>(chunk);
            while (stream_.avail_in > 0U) {
                stream_.next_out = output_buffer_.data();
                stream_.avail_out = static_cast<unsigned int>(output_buffer_.size());
                const auto status = BZ2_bzCompress(&stream_, BZ_RUN);
                if (status != BZ_RUN_OK) {
                    throw ArchiveError(std::string("Bzip2 compression failed: ") + bzip2_status_name(status));
                }
                const auto produced = output_buffer_.size() - stream_.avail_out;
                write_counted(output_, output_buffer_.data(), produced, output_bytes_);
            }
            offset += chunk;
        }
    }

    std::ofstream output_;
    bz_stream stream_{};
    bool stream_active_ = false;
    bool closed_ = false;
    std::array<unsigned char, kBzip2StreamBufferBytes> input_buffer_{};
    std::array<char, kBzip2StreamBufferBytes> output_buffer_{};
    std::uint64_t input_bytes_ = 0;
    std::uint64_t output_bytes_ = 0;
};

class Bzip2InputStream::Buffer final : public std::streambuf {
  public:
    explicit Buffer(const std::filesystem::path& archive_path)
        : input_(archive_path, std::ios::binary), archive_size_(bzip2_file_size(archive_path)) {
        if (!input_) {
            throw ArchiveError("cannot open Bzip2 stream: " + archive_path.string());
        }
        const auto status = BZ2_bzDecompressInit(&stream_, kBzip2Verbosity, 0);
        if (status != BZ_OK) {
            throw ArchiveError(std::string("failed to initialize Bzip2 decompressor: ") + bzip2_status_name(status));
        }
        stream_active_ = true;
        compressed_remaining_ = archive_size_;
        setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data());
    }

    ~Buffer() override {
        if (stream_active_) {
            BZ2_bzDecompressEnd(&stream_);
        }
    }

    // Purpose: Drain all remaining uncompressed bytes and force Bzip2 validation.
    // Inputs: None.
    // Outputs: Throws when the Bzip2 stream is incomplete, corrupt, or has trailing data.
    void finish() {
        while (!finished_) {
            setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data());
            fill_output();
        }
    }

    // Purpose: Report compressed source byte size.
    // Inputs: None.
    // Outputs: Returns the `.bz2` file byte size.
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
        setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data());
        fill_output();
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        return traits_type::eof();
    }

  private:
    // Purpose: Fill the get area with newly decompressed bytes or finish validation.
    // Inputs: None.
    // Outputs: Updates `setg` when bytes are produced; throws on malformed Bzip2 payloads.
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
                    throw ArchiveError("Bzip2 compressed payload is truncated");
                }
                compressed_remaining_ -= to_read;
                stream_.next_in = reinterpret_cast<char*>(input_buffer_.data());
                stream_.avail_in = static_cast<unsigned int>(to_read);
            }
            if (stream_.avail_in == 0U && compressed_remaining_ == 0U) {
                throw ArchiveError("Bzip2 compressed payload ended before stream completion");
            }

            stream_.next_out = output_buffer_.data();
            stream_.avail_out = static_cast<unsigned int>(output_buffer_.size());
            const auto status = BZ2_bzDecompress(&stream_);
            if (status != BZ_OK && status != BZ_STREAM_END) {
                throw ArchiveError(std::string("Bzip2 decompression failed: ") + bzip2_status_name(status));
            }
            const auto produced = output_buffer_.size() - stream_.avail_out;
            if (status == BZ_STREAM_END) {
                if (stream_.avail_in != 0U || compressed_remaining_ != 0U) {
                    throw ArchiveError("Bzip2 stream contains trailing compressed data");
                }
                finish_stream();
            }
            if (produced > 0U) {
                output_bytes_ = checked_add_extracted_output_bytes(output_bytes_, produced, "Bzip2 output");
                setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data() + produced);
                return;
            }
            if (finished_) {
                return;
            }
        }
    }

    // Purpose: End libbzip2 state after a complete stream.
    // Inputs: None.
    // Outputs: Marks the stream finished or throws if cleanup fails.
    void finish_stream() {
        if (stream_active_) {
            const auto end_status = BZ2_bzDecompressEnd(&stream_);
            stream_active_ = false;
            if (end_status != BZ_OK) {
                throw ArchiveError(std::string("failed to finalize Bzip2 decompressor: ") +
                                   bzip2_status_name(end_status));
            }
        }
        finished_ = true;
    }

    std::ifstream input_;
    bz_stream stream_{};
    bool stream_active_ = false;
    bool finished_ = false;
    std::uint64_t archive_size_ = 0;
    std::uint64_t compressed_remaining_ = 0;
    std::array<unsigned char, kBzip2StreamBufferBytes> input_buffer_{};
    std::array<char, kBzip2StreamBufferBytes> output_buffer_{};
    std::uint64_t output_bytes_ = 0;
};

// Purpose: Construct an output stream that writes a complete Bzip2 member.
// Inputs: `output_path` is the target file and `compression_level` is the libbzip2 1-9 block-size setting.
// Outputs: Installs an owned stream buffer or throws on setup failure.
Bzip2OutputStream::Bzip2OutputStream(const std::filesystem::path& output_path, int compression_level)
    : std::ostream(nullptr), buffer_(std::make_unique<Buffer>(output_path, compression_level)) {
    rdbuf(buffer_.get());
}

Bzip2OutputStream::~Bzip2OutputStream() {
    try {
        close();
    } catch (...) {
    }
}

void Bzip2OutputStream::close() {
    buffer_->close();
}

std::uint64_t Bzip2OutputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t Bzip2OutputStream::output_bytes() const {
    return buffer_->output_bytes();
}

Bzip2InputStream::Bzip2InputStream(const std::filesystem::path& archive_path)
    : std::istream(nullptr), buffer_(std::make_unique<Buffer>(archive_path)) {
    rdbuf(buffer_.get());
}

Bzip2InputStream::~Bzip2InputStream() = default;

void Bzip2InputStream::finish() {
    buffer_->finish();
    clear();
}

std::uint64_t Bzip2InputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t Bzip2InputStream::output_bytes() const {
    return buffer_->output_bytes();
}

}  // namespace superzip
