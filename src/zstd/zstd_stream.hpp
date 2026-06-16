#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>

namespace superzip {

// Purpose: Stream Zstandard-compressed bytes to a file with libzstd-managed framing.
// Inputs: Construct with `output_path`; callers write uncompressed bytes through the `std::ostream` interface.
// Outputs: Writes a complete `.zst` stream with a content checksum; throws on I/O or compressor failure.
class ZstdOutputStream final : public std::ostream {
public:
    explicit ZstdOutputStream(const std::filesystem::path& output_path);
    ~ZstdOutputStream() override;

    ZstdOutputStream(const ZstdOutputStream&) = delete;
    ZstdOutputStream& operator=(const ZstdOutputStream&) = delete;

    // Purpose: Finish the Zstandard frame and close the destination file.
    // Inputs: None.
    // Outputs: Flushes and closes the file; repeated calls are no-ops after success.
    void close();

    // Purpose: Report uncompressed bytes accepted by the stream.
    // Inputs: None.
    // Outputs: Returns the byte count accepted by libzstd.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report compressed file bytes written by the stream.
    // Inputs: None.
    // Outputs: Returns `.zst` bytes written so far.
    [[nodiscard]] std::uint64_t output_bytes() const;

private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

// Purpose: Stream-decompress a Zstandard file through `std::istream` with bounded window memory.
// Inputs: Construct with `archive_path`; callers read uncompressed bytes through the `std::istream` interface.
// Outputs: Provides raw uncompressed bytes and validates Zstandard framing/checks when drained.
class ZstdInputStream final : public std::istream {
public:
    explicit ZstdInputStream(const std::filesystem::path& archive_path);
    ~ZstdInputStream() override;

    ZstdInputStream(const ZstdInputStream&) = delete;
    ZstdInputStream& operator=(const ZstdInputStream&) = delete;

    // Purpose: Drain unread compressed data so Zstandard validation is forced.
    // Inputs: None.
    // Outputs: Throws if compressed payload validation fails.
    void finish();

    // Purpose: Report compressed archive bytes consumed by the stream source.
    // Inputs: None.
    // Outputs: Returns the `.zst` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report uncompressed bytes emitted by the decoder.
    // Inputs: None.
    // Outputs: Returns bytes produced before EOF.
    [[nodiscard]] std::uint64_t output_bytes() const;

private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

}  // namespace superzip
