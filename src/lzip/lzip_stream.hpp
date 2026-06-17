#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>

namespace superzip {

// Purpose: Stream-decompress an `.lz` lzip file through `std::istream` with bounded LZMA SDK state.
// Inputs: Construct with `archive_path`; callers read uncompressed bytes through the `std::istream` interface.
// Outputs: Provides concatenated member payload bytes and validates lzip CRC32, data size, member size, and EOS markers when drained.
class LzipInputStream final : public std::istream {
public:
    explicit LzipInputStream(const std::filesystem::path& archive_path);
    ~LzipInputStream() override;

    LzipInputStream(const LzipInputStream&) = delete;
    LzipInputStream& operator=(const LzipInputStream&) = delete;

    // Purpose: Drain unread compressed data so every lzip member trailer is validated.
    // Inputs: None.
    // Outputs: Throws if any member is malformed, truncated, or fails integrity checks.
    void finish();

    // Purpose: Report compressed archive bytes consumed by the stream source.
    // Inputs: None.
    // Outputs: Returns the `.lz` file byte size.
    [[nodiscard]] std::uint64_t input_bytes() const;

    // Purpose: Report uncompressed bytes emitted by all lzip members.
    // Inputs: None.
    // Outputs: Returns bytes produced before EOF.
    [[nodiscard]] std::uint64_t output_bytes() const;

private:
    class Buffer;
    std::unique_ptr<Buffer> buffer_;
};

}  // namespace superzip
