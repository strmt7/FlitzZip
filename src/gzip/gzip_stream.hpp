#pragma once

#include "core/resource_limits.hpp"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>

namespace superzip {

// Purpose: Stream Gzip-compressed bytes to a file while writing a valid single-member wrapper.
// Inputs: Construct with `output_path` and a 1-9 miniz `compression_level`; callers write uncompressed bytes through
// the `std::ostream` interface. Outputs: Writes Gzip header, raw deflate payload, and CRC32/ISIZE trailer; throws on
// I/O or compressor failure.
class GzipOutputStream final : public std::ostream {
  public:
    explicit GzipOutputStream(const std::filesystem::path& output_path,
                              int compression_level = kDefaultCompressionLevel);
    ~GzipOutputStream() override;

    GzipOutputStream(const GzipOutputStream&) = delete;
    GzipOutputStream& operator=(const GzipOutputStream&) = delete;

    // Purpose: Finish the deflate stream and append the Gzip trailer.
    // Inputs: None.
    // Outputs: Flushes and closes the file; repeated calls are no-ops after success.
    void close();

    // Purpose: Report uncompressed bytes accepted by the stream.
    // Inputs: None.
    // Outputs: Returns the byte count before modulo truncation for Gzip ISIZE.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report compressed file bytes written by the stream.
    // Inputs: None.
    // Outputs: Returns header, compressed payload, and trailer bytes written so far.
    [[nodiscard]] std::uint64_t output_bytes() const;

  private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

// Purpose: Stream-decompress a single-member Gzip file through `std::istream`.
// Inputs: Construct with `archive_path`; callers read uncompressed bytes through the `std::istream` interface.
// Outputs: Provides raw uncompressed bytes and validates CRC32/ISIZE when the stream is drained.
class GzipInputStream final : public std::istream {
  public:
    explicit GzipInputStream(const std::filesystem::path& archive_path);
    ~GzipInputStream() override;

    GzipInputStream(const GzipInputStream&) = delete;
    GzipInputStream& operator=(const GzipInputStream&) = delete;

    // Purpose: Drain unread compressed data so Gzip trailer validation is forced.
    // Inputs: None.
    // Outputs: Throws if compressed payload, CRC32, or ISIZE validation fails.
    void finish();

    // Purpose: Report compressed archive bytes consumed by the stream source.
    // Inputs: None.
    // Outputs: Returns the `.gz` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report uncompressed bytes emitted by the inflater.
    // Inputs: None.
    // Outputs: Returns bytes produced before modulo truncation for Gzip ISIZE.
    [[nodiscard]] std::uint64_t output_bytes() const;

  private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

}  // namespace superzip
