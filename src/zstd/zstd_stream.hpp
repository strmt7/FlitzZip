#pragma once

#include "core/resource_limits.hpp"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>
#include <optional>

namespace superzip {

// Purpose: Bound asynchronous compression workers for sufficiently large, known-size streams.
// Inputs: Optional exact byte count, product effort 1-9, and the available logical processor count.
// Outputs: Returns 0-4 workers; unknown/small inputs and high-effort history-sensitive streams stay synchronous.
[[nodiscard]] std::uint32_t zstd_stream_worker_count(std::optional<std::uint64_t> input_bytes, int compression_level,
                                                     unsigned int logical_processors);

// Purpose: Stream Zstandard-compressed bytes to a file with libzstd-managed framing.
// Inputs: Construct with `output_path` and product effort 1-9 (Zstandard 1-22); callers write uncompressed bytes
// through the `std::ostream` interface. Outputs: Writes a complete `.zst` stream with a content checksum; throws on I/O
// or compressor failure. Invalid effort is rejected before opening the destination. Optional exact input size
// bounds codec workspace and is enforced before frame completion; omission retains unknown-size streaming.
class ZstdOutputStream final : public std::ostream {
  public:
    explicit ZstdOutputStream(const std::filesystem::path& output_path,
                              int compression_level = kDefaultCompressionLevel,
                              std::optional<std::uint64_t> expected_input_bytes = std::nullopt);
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

    // Purpose: Report current codec-owned compression memory separately from process or wrapper allocation.
    // Inputs: No concurrent writes or close calls; threaded compression must not have accepted any input yet.
    // Outputs: Returns context/workspace bytes or zero after release; throws if asynchronous jobs may be active.
    [[nodiscard]] std::size_t workspace_bytes() const;

    // Purpose: Report allocated codec workspace after all compression jobs have completed.
    // Inputs: Call after successful close; no concurrent stream operation.
    // Outputs: Returns the completion snapshot, excluding caller buffers and process overhead; zero before close.
    [[nodiscard]] std::size_t completed_workspace_bytes() const;

    // Purpose: Report the number of libzstd compression workers selected for this stream.
    // Inputs: No concurrent stream operation.
    // Outputs: Returns 0 for synchronous compression, or the checked asynchronous worker count.
    [[nodiscard]] std::uint32_t compression_workers() const;

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
