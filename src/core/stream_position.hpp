#pragma once

#include <cstdint>
#include <optional>

namespace flitzzip {

// Purpose: Resolve a signed stream displacement without signed overflow or unsigned wraparound of the result.
// Inputs: `base` is the current origin, `offset` is signed, and `extent` is the inclusive maximum absolute position.
// Outputs: Returns an in-bounds position, or nullopt for an invalid origin, underflow or position past the extent.
[[nodiscard]] constexpr std::optional<std::uint64_t>
checked_stream_seek_position(std::uint64_t base, std::int64_t offset, std::uint64_t extent) noexcept {
    if (base > extent) {
        return std::nullopt;
    }
    if (offset < 0) {
        // Unsigned subtraction represents INT64_MIN's magnitude without ever negating it as a signed value.
        const auto distance = std::uint64_t{0} - static_cast<std::uint64_t>(offset);
        if (distance > base) {
            return std::nullopt;
        }
        return base - distance;
    }
    const auto distance = static_cast<std::uint64_t>(offset);
    if (distance > extent - base) {
        return std::nullopt;
    }
    return base + distance;
}

}  // namespace flitzzip
