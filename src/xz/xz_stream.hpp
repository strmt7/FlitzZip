#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>

namespace superzip {

// Purpose: Stream-decompress an `.xz` file through `std::istream` with bounded XZ Embedded state.
// Inputs: Construct with `archive_path`; callers read uncompressed bytes through the `std::istream` interface.
// Outputs: Provides raw uncompressed bytes and validates XZ framing/checks when drained.
class XzInputStream final : public std::istream {
public:
    explicit XzInputStream(const std::filesystem::path& archive_path);
    ~XzInputStream() override;

    XzInputStream(const XzInputStream&) = delete;
    XzInputStream& operator=(const XzInputStream&) = delete;

    // Purpose: Drain unread compressed data so XZ validation is forced.
    // Inputs: None.
    // Outputs: Throws if compressed payload validation fails.
    void finish();

    // Purpose: Report compressed archive bytes consumed by the stream source.
    // Inputs: None.
    // Outputs: Returns the `.xz` file byte size.
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
