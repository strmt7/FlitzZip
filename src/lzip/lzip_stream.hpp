#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>

namespace flitzzip {

// Purpose: Stream-decompress an `.lz` lzip file through `std::istream` with bounded LZMA SDK state.
// Inputs: Construct with a regular `archive_path`; callers read through `std::istream` while its source stays pinned.
// Outputs: Provides concatenated member payload bytes and validates lzip CRC32, data size, member size, and EOS markers
// when drained.
// Rejects mutable/reparse sources and retains the source/parent locks until reader destruction, including after finish.
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

    // Write at most one owned decoded window, bounded by maximum_bytes. Zero
    // means EOF (or a zero maximum); short positive results are not EOF. This
    // does not update istream flags/gcount. Failed writes leave the window intact;
    // failed stream state throws ios_base::failure, other exceptions propagate.
    // No concurrent or reentrant source operation is permitted.
    [[nodiscard]] std::size_t transfer_to(std::ostream& output, std::size_t maximum_bytes);

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

}  // namespace flitzzip
