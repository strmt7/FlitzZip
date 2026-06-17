#include "zstd/zstd_stream.hpp"

#include "core/result.hpp"
#include "zstd/zstd_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <streambuf>
#include <string>

namespace superzip {
namespace {

constexpr std::size_t kZstdStreamBufferBytes = 64U * 1024U;
constexpr int kZstdMaxWindowLog = 26;

// Purpose: Convert a Zstandard runtime result into a stable SuperZip error when needed.
// Inputs: `runtime` is the loaded Zstandard API, `code` is a runtime result, and `context` names the failed operation.
// Outputs: Returns normally when `code` is not an error; otherwise throws `ArchiveError`.
void require_zstd_ok(const ZstdRuntime& runtime, std::size_t code, const char* context) {
    if (runtime.is_error(code)) {
        throw ArchiveError(std::string(context) + ": " + runtime.error_name(code));
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

// Purpose: Validate a product compression level before passing it to libzstd.
// Inputs: `compression_level` is the caller-selected 1-9 level.
// Outputs: Returns the validated Zstandard level or throws `ArchiveError`.
int zstd_compression_level(int compression_level) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("Zstandard compression level must be between 1 and 9");
    }
    return compression_level;
}

// Purpose: Read a filesystem file size into a 64-bit archive counter.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
std::uint64_t zstd_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read Zstandard file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("Zstandard file size exceeds SuperZip limits: " + path.string());
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
        throw ArchiveError("failed to write Zstandard stream");
    }
    checked_add_stream_bytes(written, size, "Zstandard output");
}

}  // namespace

class ZstdOutputStream::Buffer final : public std::streambuf {
  public:
    // Purpose: Open a Zstandard output file and initialize the compression context.
    // Inputs: `output_path` is the destination and `compression_level` is validated as 1-9.
    // Outputs: Creates an active libzstd stream or throws on I/O/runtime failure.
    explicit Buffer(const std::filesystem::path& output_path, int compression_level)
        : output_(output_path, std::ios::binary | std::ios::trunc) {
        if (!output_) {
            throw ArchiveError("cannot create Zstandard stream: " + output_path.string());
        }
        context_ = zstd_.create_compression_context();
        if (context_ == nullptr) {
            throw ArchiveError("failed to initialize Zstandard compressor");
        }
        require_zstd_ok(zstd_,
                        zstd_.set_compression_parameter(context_, kZstdCompressionLevelParameter,
                                                        zstd_compression_level(compression_level)),
                        "failed to set Zstandard compression level");
        require_zstd_ok(zstd_, zstd_.set_compression_parameter(context_, kZstdContentChecksumParameter, 1),
                        "failed to enable Zstandard content checksum");
    }

    ~Buffer() override {
        try {
            close();
        } catch (...) {
            if (context_ != nullptr) {
                zstd_.free_compression_context(context_);
                context_ = nullptr;
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
        ZstdInputBuffer input{nullptr, 0U, 0U};
        std::size_t remaining = 0;
        do {
            ZstdOutputBuffer output{output_buffer_.data(), output_buffer_.size(), 0U};
            remaining = zstd_.compress_stream(context_, &output, &input, ZstdEndDirective::End);
            require_zstd_ok(zstd_, remaining, "Zstandard compression finalization failed");
            write_counted(output_, output_buffer_.data(), output.pos, output_bytes_);
        } while (remaining != 0U);
        zstd_.free_compression_context(context_);
        context_ = nullptr;
        output_.close();
        if (!output_) {
            throw ArchiveError("failed to finalize Zstandard stream");
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
    // Outputs: Returns bytes written to the `.zst` stream.
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
            throw ArchiveError("cannot write to a closed Zstandard stream");
        }
        checked_add_stream_bytes(input_bytes_, size, "Zstandard input");
        ZstdInputBuffer input{data, size, 0U};
        while (input.pos < input.size) {
            ZstdOutputBuffer output{output_buffer_.data(), output_buffer_.size(), 0U};
            const auto status = zstd_.compress_stream(context_, &output, &input, ZstdEndDirective::Continue);
            require_zstd_ok(zstd_, status, "Zstandard compression failed");
            write_counted(output_, output_buffer_.data(), output.pos, output_bytes_);
        }
    }

    const ZstdRuntime& zstd_ = zstd_runtime();
    std::ofstream output_;
    ZstdCompressionContext* context_ = nullptr;
    bool closed_ = false;
    std::array<char, kZstdStreamBufferBytes> output_buffer_{};
    std::uint64_t input_bytes_ = 0;
    std::uint64_t output_bytes_ = 0;
};

class ZstdInputStream::Buffer final : public std::streambuf {
  public:
    // Purpose: Open and initialize a bounded Zstandard decompression stream.
    // Inputs: `archive_path` is an existing `.zst`/`.zstd` stream.
    // Outputs: Prepares the get area and decoder state, or throws on I/O/runtime failure.
    explicit Buffer(const std::filesystem::path& archive_path)
        : file_(archive_path, std::ios::binary), archive_size_(zstd_file_size(archive_path)),
          compressed_remaining_(archive_size_) {
        if (!file_) {
            throw ArchiveError("cannot open Zstandard stream: " + archive_path.string());
        }
        context_ = zstd_.create_decompression_stream();
        if (context_ == nullptr) {
            throw ArchiveError("failed to initialize Zstandard decompressor");
        }
        require_zstd_ok(zstd_,
                        zstd_.set_decompression_parameter(context_, kZstdWindowLogMaxParameter, kZstdMaxWindowLog),
                        "failed to set Zstandard window limit");
        setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data());
    }

    ~Buffer() override {
        if (context_ != nullptr) {
            zstd_.free_decompression_stream(context_);
            context_ = nullptr;
        }
    }

    // Purpose: Drain all remaining uncompressed bytes and force Zstandard validation.
    // Inputs: None.
    // Outputs: Throws when the Zstandard stream is incomplete, corrupt, or has trailing garbage.
    void finish() {
        while (!finished_) {
            setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data());
            fill_output();
        }
    }

    // Purpose: Report compressed source byte size.
    // Inputs: None.
    // Outputs: Returns the `.zst` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const {
        return archive_size_;
    }

    // Purpose: Report uncompressed byte count emitted by the decoder.
    // Inputs: None.
    // Outputs: Returns the uncompressed byte count produced so far.
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
    // Purpose: Load another bounded compressed input chunk when the decoder has consumed the current one.
    // Inputs: None.
    // Outputs: Updates the Zstandard input buffer; marks source completion after all file bytes are read.
    void refill_input_if_needed() {
        if (input_.pos != input_.size || compressed_remaining_ == 0U) {
            return;
        }
        const auto to_read =
            static_cast<std::size_t>(std::min<std::uint64_t>(input_buffer_.size(), compressed_remaining_));
        file_.read(input_buffer_.data(), static_cast<std::streamsize>(to_read));
        if (static_cast<std::size_t>(file_.gcount()) != to_read) {
            throw ArchiveError("Zstandard compressed payload is truncated");
        }
        compressed_remaining_ -= to_read;
        input_ = ZstdInputBuffer{input_buffer_.data(), to_read, 0U};
    }

    // Purpose: Fill the get area with newly decompressed bytes or finish validation.
    // Inputs: None.
    // Outputs: Updates `setg` when bytes are produced; throws on malformed Zstandard payloads.
    void fill_output() {
        if (finished_) {
            return;
        }
        for (;;) {
            refill_input_if_needed();
            if (input_.pos == input_.size && compressed_remaining_ == 0U) {
                if (frame_complete_) {
                    finished_ = true;
                    return;
                }
                throw ArchiveError("Zstandard stream ended before decoder completion");
            }

            ZstdOutputBuffer output{output_buffer_.data(), output_buffer_.size(), 0U};
            const auto status = zstd_.decompress_stream(context_, &output, &input_);
            require_zstd_ok(zstd_, status, "Zstandard decompression failed");
            if (status == 0U) {
                frame_complete_ = true;
            } else {
                frame_complete_ = false;
            }
            if (output.pos > 0U) {
                checked_add_stream_bytes(output_bytes_, output.pos, "Zstandard output");
                setg(output_buffer_.data(), output_buffer_.data(), output_buffer_.data() + output.pos);
                return;
            }
        }
    }

    std::ifstream file_;
    const ZstdRuntime& zstd_ = zstd_runtime();
    std::uint64_t archive_size_ = 0;
    std::uint64_t output_bytes_ = 0;
    std::uint64_t compressed_remaining_ = 0;
    ZstdDecompressionStream* context_ = nullptr;
    ZstdInputBuffer input_{nullptr, 0U, 0U};
    bool frame_complete_ = false;
    bool finished_ = false;
    std::array<char, kZstdStreamBufferBytes> input_buffer_{};
    std::array<char, kZstdStreamBufferBytes> output_buffer_{};
};

// Purpose: Construct an output stream that writes a complete checksummed Zstandard frame.
// Inputs: `output_path` is the target file and `compression_level` is the Zstandard 1-9 setting.
// Outputs: Installs an owned stream buffer or throws on setup failure.
ZstdOutputStream::ZstdOutputStream(const std::filesystem::path& output_path, int compression_level)
    : std::ostream(nullptr), buffer_(std::make_unique<Buffer>(output_path, compression_level)) {
    rdbuf(buffer_.get());
}

ZstdOutputStream::~ZstdOutputStream() {
    try {
        close();
    } catch (...) {
    }
}

void ZstdOutputStream::close() {
    buffer_->close();
}

std::uint64_t ZstdOutputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t ZstdOutputStream::output_bytes() const {
    return buffer_->output_bytes();
}

ZstdInputStream::ZstdInputStream(const std::filesystem::path& archive_path)
    : std::istream(nullptr), buffer_(std::make_unique<Buffer>(archive_path)) {
    rdbuf(buffer_.get());
}

ZstdInputStream::~ZstdInputStream() = default;

void ZstdInputStream::finish() {
    buffer_->finish();
    clear();
}

std::uint64_t ZstdInputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t ZstdInputStream::output_bytes() const {
    return buffer_->output_bytes();
}

}  // namespace superzip
