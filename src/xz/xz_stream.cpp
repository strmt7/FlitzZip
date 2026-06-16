#include "xz/xz_stream.hpp"

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <streambuf>
#include <string>

extern "C" {
#include "xz.h"
}

namespace superzip {
namespace {

constexpr std::size_t kXzStreamBufferBytes = 64U * 1024U;
constexpr std::uint32_t kXzMaxDictionaryBytes = 64U * 1024U * 1024U;

// Purpose: Initialize XZ Embedded checksum tables once per process.
// Inputs: None.
// Outputs: Prepares CRC32 and CRC64 lookup tables for subsequent decoders.
void initialize_xz_tables() {
    static std::once_flag once;
    std::call_once(once, [] {
        xz_crc32_init();
        xz_crc64_init();
    });
}

// Purpose: Convert an XZ Embedded status code into an actionable diagnostic suffix.
// Inputs: `status` is a return code from `xz_dec_catrun`.
// Outputs: Returns a stable string for exception messages.
const char* xz_status_name(enum xz_ret status) {
    switch (status) {
    case XZ_OK:
        return "ok";
    case XZ_STREAM_END:
        return "stream end";
    case XZ_UNSUPPORTED_CHECK:
        return "unsupported check";
    case XZ_MEM_ERROR:
        return "memory allocation failed";
    case XZ_MEMLIMIT_ERROR:
        return "dictionary exceeds SuperZip limit";
    case XZ_FORMAT_ERROR:
        return "not an XZ stream";
    case XZ_OPTIONS_ERROR:
        return "unsupported XZ options";
    case XZ_DATA_ERROR:
        return "corrupt data";
    case XZ_BUF_ERROR:
        return "truncated or stalled stream";
    }
    return "unknown status";
}

// Purpose: Read a filesystem file size into a 64-bit archive counter.
// Inputs: `path` is an existing file path.
// Outputs: Returns the file size or throws when it cannot be queried.
std::uint64_t xz_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw ArchiveError("cannot read XZ file size: " + path.string());
    }
    if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
        throw ArchiveError("XZ file size exceeds SuperZip limits: " + path.string());
    }
    return static_cast<std::uint64_t>(size);
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

}  // namespace

class XzInputStream::Buffer final : public std::streambuf {
public:
    explicit Buffer(const std::filesystem::path& archive_path)
        : input_(archive_path, std::ios::binary), archive_size_(xz_file_size(archive_path)) {
        if (!input_) {
            throw ArchiveError("cannot open XZ stream: " + archive_path.string());
        }
        initialize_xz_tables();
        decoder_ = xz_dec_init(XZ_DYNALLOC, kXzMaxDictionaryBytes);
        if (decoder_ == nullptr) {
            throw ArchiveError("failed to initialize XZ decoder");
        }
        buffer_.in = input_buffer_.data();
        buffer_.in_pos = 0;
        buffer_.in_size = 0;
        buffer_.out = output_buffer_.data();
        buffer_.out_pos = 0;
        buffer_.out_size = output_buffer_.size();
        setg(
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()));
    }

    ~Buffer() override {
        if (decoder_ != nullptr) {
            xz_dec_end(decoder_);
            decoder_ = nullptr;
        }
    }

    // Purpose: Drain all remaining uncompressed bytes and force XZ stream validation.
    // Inputs: None.
    // Outputs: Throws when the XZ stream is incomplete, corrupt, or exceeds limits.
    void finish() {
        while (!finished_) {
            setg(
                reinterpret_cast<char*>(output_buffer_.data()),
                reinterpret_cast<char*>(output_buffer_.data()),
                reinterpret_cast<char*>(output_buffer_.data()));
            fill_output();
        }
    }

    // Purpose: Report compressed source byte size.
    // Inputs: None.
    // Outputs: Returns the `.xz` file byte size.
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
        setg(
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()),
            reinterpret_cast<char*>(output_buffer_.data()));
        fill_output();
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        return traits_type::eof();
    }

private:
    // Purpose: Load another bounded compressed input chunk when the decoder has consumed the current one.
    // Inputs: None.
    // Outputs: Updates the XZ input buffer; marks source completion after a real EOF read.
    void refill_input_if_needed() {
        if (source_finished_ || buffer_.in_pos != buffer_.in_size) {
            return;
        }
        input_.read(reinterpret_cast<char*>(input_buffer_.data()), static_cast<std::streamsize>(input_buffer_.size()));
        const auto bytes_read = static_cast<std::size_t>(input_.gcount());
        if (input_.bad()) {
            throw ArchiveError("failed to read XZ stream");
        }
        buffer_.in = input_buffer_.data();
        buffer_.in_pos = 0;
        buffer_.in_size = bytes_read;
        if (bytes_read == 0U && input_.eof()) {
            source_finished_ = true;
        }
    }

    // Purpose: Fill the get area with newly decompressed bytes or finish validation.
    // Inputs: None.
    // Outputs: Updates `setg` when bytes are produced; throws on malformed XZ payloads.
    void fill_output() {
        if (finished_) {
            return;
        }
        for (;;) {
            refill_input_if_needed();
            buffer_.out = output_buffer_.data();
            buffer_.out_pos = 0;
            buffer_.out_size = output_buffer_.size();
            const auto before_in = buffer_.in_pos;
            const auto status = xz_dec_catrun(decoder_, &buffer_, source_finished_ ? 1 : 0);
            const auto consumed = buffer_.in_pos - before_in;
            const auto produced = buffer_.out_pos;
            if (produced > 0U) {
                checked_add_stream_bytes(output_bytes_, produced, "XZ output");
                setg(
                    reinterpret_cast<char*>(output_buffer_.data()),
                    reinterpret_cast<char*>(output_buffer_.data()),
                    reinterpret_cast<char*>(output_buffer_.data() + produced));
                if (status == XZ_STREAM_END) {
                    finished_ = true;
                }
                return;
            }
            if (status == XZ_STREAM_END) {
                finished_ = true;
                return;
            }
            if (status == XZ_OK) {
                if (consumed == 0U && source_finished_) {
                    throw ArchiveError("XZ stream ended before decoder completion");
                }
                continue;
            }
            throw ArchiveError(std::string("XZ decompression failed: ") + xz_status_name(status));
        }
    }

    std::ifstream input_;
    std::uint64_t archive_size_ = 0;
    std::uint64_t output_bytes_ = 0;
    struct xz_dec* decoder_ = nullptr;
    struct xz_buf buffer_{};
    bool source_finished_ = false;
    bool finished_ = false;
    std::array<std::uint8_t, kXzStreamBufferBytes> input_buffer_{};
    std::array<std::uint8_t, kXzStreamBufferBytes> output_buffer_{};
};

XzInputStream::XzInputStream(const std::filesystem::path& archive_path)
    : std::istream(nullptr), buffer_(std::make_unique<Buffer>(archive_path)) {
    rdbuf(buffer_.get());
}

XzInputStream::~XzInputStream() = default;

void XzInputStream::finish() {
    buffer_->finish();
}

std::uint64_t XzInputStream::input_bytes() const {
    return buffer_->input_bytes();
}

std::uint64_t XzInputStream::output_bytes() const {
    return buffer_->output_bytes();
}

}  // namespace superzip
