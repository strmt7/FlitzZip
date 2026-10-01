#pragma once

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace superzip {

// Purpose: Assign distinct bounded histogram budgets without completing the sample before maximum effort.
// Inputs: Nonempty bounded block bytes and validated entropy effort 2-9.
// Outputs: Returns exact sample bytes; throws GpuError for invalid input. Small tails may share a budget.
inline std::size_t entropy_sample_count(std::size_t block_bytes, int effort) {
    if (block_bytes == 0U || block_bytes > kMaxArchiveBlockBytes || effort < 2 || effort > 9) {
        throw GpuError("GPU entropy sampling settings are outside resource limits");
    }
    if (effort == 9) {
        return block_bytes;
    }
    constexpr std::array<std::size_t, 7> budgets{4096U, 16384U, 65536U, 262144U, 1048576U, 4194304U, 8388608U};
    const auto relative_budget = block_bytes * static_cast<std::size_t>(effort - 1) / 8U;
    return std::max<std::size_t>(1U, std::min(relative_budget, budgets[static_cast<std::size_t>(effort - 2)]));
}

// Purpose: Maintain a bounded nested histogram without retaining a copy of the source.
// Inputs: A caller-owned immutable block that remains live throughout sampling.
// Outputs: Supplies exact incremental frequencies without heap allocation or repeated byte visits.
class EntropyHistogramSampler {
  public:
    // Purpose: Start a cache-line-stratified permutation of one entropy block without allocating source storage.
    // Inputs: A nonempty source span bounded by kMaxArchiveBlockBytes; its lifetime must exceed this sampler's.
    // Outputs: Creates an empty histogram or throws GpuError for invalid source length.
    explicit EntropyHistogramSampler(std::span<const std::byte> block) : block_(block) {
        if (block.empty() || block.size() > kMaxArchiveBlockBytes) {
            throw GpuError("GPU entropy histogram source is outside resource limits");
        }
        line_count_ = (block.size() + kLineBytes - 1U) / kLineBytes;
        permutation_size_ = std::bit_ceil(line_count_);
        if (sample_line() >= line_count_) {
            advance_line();
        }
    }

    // Purpose: Extend a nested, tile-stratified histogram, visiting no source byte more than once.
    // Inputs: An exact nondecreasing sample count in [1, block.size()].
    // Outputs: Returns accumulated frequencies summing to target; throws GpuError for an invalid target.
    const std::array<std::uint64_t, 256>& sample_to(std::size_t target) {
        if (target == 0U || target < sampled_ || target > block_.size()) {
            throw GpuError("GPU entropy histogram target is invalid");
        }
        while (sampled_ < target) {
            const auto line_start = sample_line() * kLineBytes;
            const auto line_end = std::min(block_.size(), line_start + kLineBytes);
            const auto count = std::min(target - sampled_, line_end - line_start - offset_);
            for (std::size_t index = 0U; index < count; ++index) {
                ++histogram_[static_cast<std::uint8_t>(block_[line_start + offset_ + index])];
            }
            offset_ += count;
            sampled_ += count;
            if (line_start + offset_ == line_end && sampled_ < block_.size()) {
                advance_line();
                offset_ = 0U;
            }
        }
        return histogram_;
    }

  private:
    // Purpose: Scramble tile phases within each 16 KiB stratum without changing its membership or duplicating tiles.
    // Inputs: Current bit-reversed line and bounded power-of-two permutation size.
    // Outputs: Returns a deterministic domain tile; XOR with a fixed per-stratum mask is a bijection.
    std::size_t sample_line() const {
        auto phase = static_cast<std::uint32_t>(line_ >> 8U) + 0x9E3779B9U;
        phase ^= phase >> 16U;
        phase *= 0x85EBCA6BU;
        phase ^= phase >> 13U;
        phase *= 0xC2B2AE35U;
        phase ^= phase >> 16U;
        const auto mask = std::min<std::size_t>(255U, permutation_size_ - 1U);
        return line_ ^ (static_cast<std::size_t>(phase) & mask);
    }

    // Purpose: Advance the scrambled bit-reversed tile permutation, skipping the incomplete power-of-two domain.
    // Inputs: Current line and bounded permutation size; at least one unread source line must remain.
    // Outputs: Updates line_ to the next valid unread line without division or allocation.
    void advance_line() {
        do {
            auto bit = permutation_size_ / 2U;
            while ((line_ & bit) != 0U) {
                line_ ^= bit;
                bit /= 2U;
            }
            line_ ^= bit;
        } while (sample_line() >= line_count_);
    }

    static constexpr std::size_t kLineBytes = 64U;
    std::span<const std::byte> block_;
    std::array<std::uint64_t, 256> histogram_{};
    std::size_t sampled_ = 0U;
    std::size_t offset_ = 0U;
    std::size_t line_ = 0U;
    std::size_t line_count_ = 0U;
    std::size_t permutation_size_ = 0U;
};

}  // namespace superzip
