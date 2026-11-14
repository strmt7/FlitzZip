#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>

namespace flitzzip {

// Purpose: Stream-decompress an `.xz` file through `std::istream` with bounded XZ Embedded state.
// Inputs: Construct with a regular `archive_path`; callers read through `std::istream` while its source stays pinned.
// Outputs: Provides raw uncompressed bytes and validates XZ framing/checks when drained.
// Rejects mutable/reparse sources and retains the source/parent locks until reader destruction, including after finish.
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

    // Write at most one owned decoded window, bounded by maximum_bytes. Zero
    // means EOF (or a zero maximum); short positive results are not EOF. This
    // does not update istream flags/gcount. Failed writes leave the window intact;
    // failed stream state throws ios_base::failure, other exceptions propagate.
    // No concurrent or reentrant source operation is permitted.
    [[nodiscard]] std::size_t transfer_to(std::ostream& output, std::size_t maximum_bytes);

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

}  // namespace flitzzip
