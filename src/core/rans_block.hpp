#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace flitzzip {

// An admitted view of the approved compact entropy payload, not public kind or
// archive-version admission. The payload must remain immutable and owned through
// every use of the view. Successful decoding allocates no buffer storage and
// never encodes on CPU.
class GpuRansBlockView {
  public:
    // Validates complete sparse models, offsets and bounded stream extents before
    // retaining views. Decoded size is nonzero, bounded and greater than payload.
    GpuRansBlockView(std::span<const std::byte> payload, std::uint32_t decoded_size);

    [[nodiscard]] std::uint32_t decoded_size() const noexcept {
        return decoded_size_;
    }
    [[nodiscard]] std::uint32_t segment_count() const noexcept {
        return segment_count_;
    }
    [[nodiscard]] std::uint8_t precision() const noexcept {
        return precision_;
    }
    [[nodiscard]] std::uint8_t context_bits() const noexcept {
        return context_bits_;
    }
    [[nodiscard]] std::uint8_t segment_shift() const noexcept {
        return segment_shift_;
    }

    // Exact caller-owned u32 storage: context offsets plus one 257-entry CDF per
    // present row. Callers charge this storage before allocating their workspace.
    [[nodiscard]] std::size_t model_workspace_words() const noexcept;

    // Writes only the requested workspace extent. Missing contexts have offset
    // zero; present rows include full-width singleton mass 65536. Rejects overlap
    // with immutable payload before writing; no heap owner crosses this boundary.
    void prepare_models(std::span<std::uint32_t> workspace) const;

    // Borrow one validated dense segment stream; rejects an out-of-range index.
    [[nodiscard]] std::span<const std::byte> stream(std::uint32_t segment) const;

    // Independently restores exact output bytes with per-segment context reset.
    // Payload, output and the used workspace must be disjoint. Admission failures
    // precede writes; corrupt coded data may alter scratch/output before throwing.
    // The caller must still verify decoded CRC before publishing any result.
    void decode(std::span<std::byte> output, std::span<std::uint32_t> workspace) const;

  private:
    struct Cursor;
    struct Row {
        std::span<const std::byte> symbols;
        std::uint16_t alphabet_size = 0;
        std::uint8_t context = 0;
    };

    void read_models(Cursor& cursor);
    void read_streams(Cursor& cursor);
    [[nodiscard]] std::span<std::uint32_t> admit_workspace(std::span<std::uint32_t> workspace) const;
    void decode_segment(std::span<const std::byte> encoded, std::span<std::byte> output,
                        std::span<const std::uint32_t> models) const;

    std::span<const std::byte> payload_;
    std::span<const std::byte> offsets_;
    std::span<const std::byte> streams_;
    std::array<Row, 256> rows_{};
    std::uint32_t decoded_size_ = 0;
    std::uint32_t segment_count_ = 0;
    std::uint16_t row_count_ = 0;
    std::uint8_t precision_ = 0;
    std::uint8_t context_bits_ = 0;
    std::uint8_t segment_shift_ = 0;
};

}  // namespace flitzzip
