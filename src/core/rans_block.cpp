#include "core/rans_block.hpp"

#include "core/byte_region.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>

namespace flitzzip {
namespace {

constexpr std::uint32_t kRansLower = 1U << 23U;
constexpr std::uint32_t kRansStateLimit = 1U << 31U;
constexpr std::size_t kRansCdfWords = 257U;

// Reads only an admitted four-byte little-endian word; callers first prove its
// complete extent or obtain it from the bounded cursor.
std::uint32_t little_u32(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(std::uint32_t)) {
        throw ArchiveError("GPU rANS word is truncated");
    }
    std::uint32_t value = 0U;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<std::uint32_t>(bytes[i]) << (8U * i);
    }
    return value;
}

}  // namespace

// The cursor owns no bytes. Each read proves the remaining extent before
// forming views or advancing; failure retains its previous boundary.
struct GpuRansBlockView::Cursor {
    std::span<const std::byte> remaining;

    std::span<const std::byte> take(std::size_t count) {
        if (count > remaining.size()) {
            throw ArchiveError("GPU rANS metadata is truncated");
        }
        const auto result = remaining.first(count);
        remaining = remaining.subspan(count);
        return result;
    }

    std::uint32_t byte() {
        return static_cast<std::uint8_t>(take(1U).front());
    }

    std::uint32_t word() {
        return little_u32(take(sizeof(std::uint32_t)));
    }

    std::uint32_t frequency() {
        const auto bytes = take(2U);
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U);
    }
};

GpuRansBlockView::GpuRansBlockView(std::span<const std::byte> payload, std::uint32_t decoded_size)
    : payload_(payload), decoded_size_(decoded_size) {
    if (decoded_size == 0U || decoded_size > kMaxArchiveBlockBytes || payload.size() >= decoded_size) {
        throw ArchiveError("GPU rANS block extent is invalid");
    }
    Cursor cursor{payload};
    precision_ = static_cast<std::uint8_t>(cursor.byte());
    context_bits_ = static_cast<std::uint8_t>(cursor.byte());
    segment_shift_ = static_cast<std::uint8_t>(cursor.byte());
    row_count_ = static_cast<std::uint16_t>(cursor.byte() + 1U);
    if (precision_ < 8U || precision_ > 16U || context_bits_ > 8U || segment_shift_ < 8U || segment_shift_ > 12U ||
        row_count_ > (1U << context_bits_)) {
        throw ArchiveError("GPU rANS options are invalid");
    }
    segment_count_ = ((decoded_size - 1U) >> segment_shift_) + 1U;
    read_models(cursor);
    read_streams(cursor);
}

// Sparse rows are canonical: increasing context/symbol IDs, positive frequency
// mass and an exact integer total. Singleton mass is implicit, not a u16 zero.
void GpuRansBlockView::read_models(Cursor& cursor) {
    int previous_context = -1;
    const auto total = 1U << precision_;
    for (std::size_t i = 0; i < row_count_; ++i) {
        const auto context = cursor.byte();
        const auto alphabet = cursor.byte() + 1U;
        if (static_cast<int>(context) <= previous_context || context >= (1U << context_bits_)) {
            throw ArchiveError("GPU rANS contexts are not canonical");
        }
        previous_context = static_cast<int>(context);
        const auto symbols = cursor.take(alphabet == 1U ? 1U : 3U * alphabet);
        if (alphabet != 1U) {
            Cursor entries{symbols};
            int previous_symbol = -1;
            std::uint32_t sum = 0U;
            for (std::uint32_t entry = 0; entry < alphabet; ++entry) {
                const auto symbol = entries.byte();
                const auto frequency = entries.frequency();
                if (static_cast<int>(symbol) <= previous_symbol || frequency == 0U || frequency > total - sum) {
                    throw ArchiveError("GPU rANS frequencies are not canonical");
                }
                previous_symbol = static_cast<int>(symbol);
                sum += frequency;
            }
            if (sum != total) {
                throw ArchiveError("GPU rANS frequency mass is invalid");
            }
        }
        rows_[i] = Row{symbols, static_cast<std::uint16_t>(alphabet), static_cast<std::uint8_t>(context)};
    }
    if (rows_[0].context != 0U) {
        throw ArchiveError("GPU rANS initial context is absent");
    }
}

// Prove all offsets plus minimum states before slicing the table. Then admit
// every stream independently, including the tail, without decoded allocation.
void GpuRansBlockView::read_streams(Cursor& cursor) {
    if (cursor.remaining.size() < 4U || segment_count_ > (cursor.remaining.size() - 4U) / 8U) {
        throw ArchiveError("GPU rANS segment inventory cannot fit");
    }
    offsets_ = cursor.take((static_cast<std::size_t>(segment_count_) + 1U) * sizeof(std::uint32_t));
    streams_ = cursor.remaining;
    if (little_u32(offsets_) != 0U) {
        throw ArchiveError("GPU rANS offsets must start at zero");
    }
    std::uint32_t previous = 0U;
    for (std::uint32_t segment = 0; segment < segment_count_; ++segment) {
        const auto next = little_u32(offsets_.subspan((static_cast<std::size_t>(segment) + 1U) * 4U, 4U));
        const auto count = std::min(1U << segment_shift_, decoded_size_ - (segment << segment_shift_));
        if (next <= previous || next > streams_.size() || next - previous < 4U || next - previous > 2U * count + 4U) {
            throw ArchiveError("GPU rANS stream extent is invalid");
        }
        previous = next;
    }
    if (previous != streams_.size()) {
        throw ArchiveError("GPU rANS streams do not cover the payload");
    }
}

std::size_t GpuRansBlockView::model_workspace_words() const noexcept {
    return (std::size_t{1} << context_bits_) + static_cast<std::size_t>(row_count_) * kRansCdfWords;
}

std::span<std::uint32_t> GpuRansBlockView::admit_workspace(std::span<std::uint32_t> workspace) const {
    if (workspace.size() < model_workspace_words()) {
        throw ArchiveError("GPU rANS model workspace is too small");
    }
    const auto used = workspace.first(model_workspace_words());
    if (byte_regions_overlap(payload_, std::as_bytes(used))) {
        throw ArchiveError("GPU rANS workspace overlaps its payload");
    }
    return used;
}

void GpuRansBlockView::prepare_models(std::span<std::uint32_t> workspace) const {
    const auto models = admit_workspace(workspace);
    std::ranges::fill(models, 0U);
    const auto context_count = std::uint32_t{1} << context_bits_;
    for (std::size_t i = 0; i < row_count_; ++i) {
        const auto offset = context_count + i * kRansCdfWords;
        models[rows_[i].context] = static_cast<std::uint32_t>(offset);
        auto cdf = models.subspan(offset, kRansCdfWords);
        std::array<std::uint32_t, 256> frequencies{};
        Cursor entries{rows_[i].symbols};
        if (rows_[i].alphabet_size == 1U) {
            frequencies[entries.byte()] = 1U << precision_;
        } else {
            for (std::uint16_t j = 0; j < rows_[i].alphabet_size; ++j) {
                const auto symbol = entries.byte();
                frequencies[symbol] = entries.frequency();
            }
        }
        for (std::size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
            cdf[symbol + 1U] = cdf[symbol] + frequencies[symbol];
        }
    }
}

std::span<const std::byte> GpuRansBlockView::stream(std::uint32_t segment) const {
    if (segment >= segment_count_) {
        throw ArchiveError("GPU rANS segment index is invalid");
    }
    const auto offset = static_cast<std::size_t>(segment) * sizeof(std::uint32_t);
    const auto begin = little_u32(offsets_.subspan(offset, 4U));
    const auto end = little_u32(offsets_.subspan(offset + 4U, 4U));
    return streams_.subspan(begin, end - begin);
}

void GpuRansBlockView::decode(std::span<std::byte> output, std::span<std::uint32_t> workspace) const {
    const auto models = admit_workspace(workspace);
    if (output.size() != decoded_size_ || byte_regions_overlap(payload_, output) ||
        byte_regions_overlap(output, std::as_bytes(models))) {
        throw ArchiveError("GPU rANS output extent or ownership is invalid");
    }
    prepare_models(models);
    for (std::uint32_t segment = 0; segment < segment_count_; ++segment) {
        const auto offset = static_cast<std::size_t>(segment) << segment_shift_;
        const auto count = std::min<std::size_t>(std::size_t{1} << segment_shift_, output.size() - offset);
        decode_segment(stream(segment), output.subspan(offset, count), models);
    }
}

// Positive admitted CDF intervals keep the update below 2^31. Precision <=16
// needs at most two renormalization bytes per symbol; every segment resets its
// previous byte and must consume its exact stream at the terminal lower state.
void GpuRansBlockView::decode_segment(std::span<const std::byte> encoded, std::span<std::byte> output,
                                      std::span<const std::uint32_t> models) const {
    Cursor cursor{encoded};
    auto state = cursor.word();
    if (state < kRansLower || state >= kRansStateLimit) {
        throw ArchiveError("GPU rANS initial state is invalid");
    }
    std::uint32_t previous = 0U;
    const auto context_count = 1U << context_bits_;
    const auto mask = (1U << precision_) - 1U;
    for (auto& byte : output) {
        const auto offset = models[previous >> (8U - context_bits_)];
        if (offset < context_count || offset > models.size() - kRansCdfWords) {
            throw ArchiveError("GPU rANS decoded context has no model");
        }
        const auto cdf = models.subspan(offset, kRansCdfWords);
        const auto slot = state & mask;
        const auto upper = std::upper_bound(cdf.begin(), cdf.end(), slot);
        const auto symbol = static_cast<std::size_t>(upper - cdf.begin() - 1);
        const auto frequency = cdf[symbol + 1U] - cdf[symbol];
        state = frequency * (state >> precision_) + slot - cdf[symbol];
        for (unsigned int renormalize = 0; renormalize < 2U && state < kRansLower; ++renormalize) {
            state = (state << 8U) | cursor.byte();
        }
        if (state < kRansLower || state >= kRansStateLimit) {
            throw ArchiveError("GPU rANS decoder state exceeds its work bound");
        }
        byte = static_cast<std::byte>(symbol);
        previous = static_cast<std::uint32_t>(symbol);
    }
    if (!cursor.remaining.empty() || state != kRansLower) {
        throw ArchiveError("GPU rANS stream has trailing bytes or invalid final state");
    }
}

}  // namespace flitzzip
