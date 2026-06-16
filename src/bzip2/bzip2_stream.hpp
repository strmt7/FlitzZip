#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>

namespace superzip {

// Purpose: Stream Bzip2-compressed bytes to a file with libbzip2-managed framing.
// Inputs: Construct with `output_path`; callers write uncompressed bytes through the `std::ostream` interface.
// Outputs: Writes a complete `.bz2` stream; throws on I/O or compressor failure.
class Bzip2OutputStream final : public std::ostream {
public:
    explicit Bzip2OutputStream(const std::filesystem::path& output_path);
    ~Bzip2OutputStream() override;

    Bzip2OutputStream(const Bzip2OutputStream&) = delete;
    Bzip2OutputStream& operator=(const Bzip2OutputStream&) = delete;

    // Purpose: Finish the Bzip2 stream and close the destination file.
    // Inputs: None.
    // Outputs: Flushes and closes the file; repeated calls are no-ops after success.
    void close();

    // Purpose: Report uncompressed bytes accepted by the stream.
    // Inputs: None.
    // Outputs: Returns the byte count accepted by libbzip2.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report compressed file bytes written by the stream.
    // Inputs: None.
    // Outputs: Returns `.bz2` bytes written so far.
    [[nodiscard]] std::uint64_t output_bytes() const;

private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

// Purpose: Stream-decompress a Bzip2 file through `std::istream`.
// Inputs: Construct with `archive_path`; callers read uncompressed bytes through the `std::istream` interface.
// Outputs: Provides raw uncompressed bytes and validates libbzip2 CRC/state when drained.
class Bzip2InputStream final : public std::istream {
public:
    explicit Bzip2InputStream(const std::filesystem::path& archive_path);
    ~Bzip2InputStream() override;

    Bzip2InputStream(const Bzip2InputStream&) = delete;
    Bzip2InputStream& operator=(const Bzip2InputStream&) = delete;

    // Purpose: Drain unread compressed data so Bzip2 validation is forced.
    // Inputs: None.
    // Outputs: Throws if compressed payload validation fails.
    void finish();

    // Purpose: Report compressed archive bytes consumed by the stream source.
    // Inputs: None.
    // Outputs: Returns the `.bz2` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report uncompressed bytes emitted by the inflater.
    // Inputs: None.
    // Outputs: Returns bytes produced before EOF.
    [[nodiscard]] std::uint64_t output_bytes() const;

private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

}  // namespace superzip
