#pragma once

// Fixed payloads from the independently differential-qualified integer oracle.
// These are correctness fixtures, not compression measurements.
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace flitzzip_test {

// A singleton has frequency 2^precision and leaves state L unchanged. Emit that
// identity directly, without a production encoder, normalizer or decoder. The
// canonical reader still owns payload-size admission; tiny inputs can be useful
// rejection fixtures. All rows optionally exercise maximum model workspace.
inline std::vector<std::byte> rans_singleton_payload(std::uint32_t decoded, unsigned int precision,
                                                     unsigned int contexts, unsigned int shift,
                                                     std::uint8_t symbol = 0U, bool all_rows = false) {
    if (decoded == 0U || decoded > 16U * 1024U * 1024U || precision < 8U || precision > 16U || contexts > 8U ||
        shift < 8U || shift > 12U) {
        throw std::invalid_argument("Invalid singleton correctness fixture options");
    }
    const auto context_count = 1U << contexts;
    const auto next_context = static_cast<unsigned int>(symbol) >> (8U - contexts);
    const auto rows = all_rows ? context_count : (next_context == 0U ? 1U : 2U);
    const auto segments = (decoded + (1U << shift) - 1U) >> shift;
    std::vector<std::byte> payload{static_cast<std::byte>(precision), static_cast<std::byte>(contexts),
                                   static_cast<std::byte>(shift), static_cast<std::byte>(rows - 1U)};
    for (unsigned int context = 0U; context < context_count; ++context) {
        if (all_rows || context == 0U || context == next_context) {
            payload.insert(payload.end(),
                           {static_cast<std::byte>(context), std::byte{0}, static_cast<std::byte>(symbol)});
        }
    }
    const auto append_word = [&payload](std::uint32_t word) {
        for (unsigned int byte = 0U; byte < 4U; ++byte) {
            payload.push_back(static_cast<std::byte>((word >> (8U * byte)) & 255U));
        }
    };
    for (std::uint32_t segment = 0U; segment <= segments; ++segment) {
        append_word(segment * 4U);
    }
    for (std::uint32_t segment = 0U; segment < segments; ++segment) {
        append_word(1U << 23U);
    }
    return payload;
}

inline constexpr std::array<std::byte, 19> kRansZero16{{
    std::byte{16}, std::byte{0}, std::byte{8}, std::byte{0},   std::byte{0}, std::byte{0}, std::byte{0},
    std::byte{0},  std::byte{0}, std::byte{0}, std::byte{0},   std::byte{4}, std::byte{0}, std::byte{0},
    std::byte{0},  std::byte{0}, std::byte{0}, std::byte{128}, std::byte{0},
}};

inline constexpr std::array<std::byte, 40> kRansBinary8{{
    std::byte{8},   std::byte{0},   std::byte{8},   std::byte{0},   std::byte{0},   std::byte{1},   std::byte{0},
    std::byte{128}, std::byte{0},   std::byte{255}, std::byte{128}, std::byte{0},   std::byte{0},   std::byte{0},
    std::byte{0},   std::byte{0},   std::byte{20},  std::byte{0},   std::byte{0},   std::byte{0},   std::byte{42},
    std::byte{0},   std::byte{128}, std::byte{0},   std::byte{170}, std::byte{170}, std::byte{170}, std::byte{170},
    std::byte{170}, std::byte{170}, std::byte{170}, std::byte{170}, std::byte{170}, std::byte{170}, std::byte{170},
    std::byte{170}, std::byte{170}, std::byte{170}, std::byte{170}, std::byte{128},
}};

inline constexpr std::array<std::byte, 45> kRansSegmented12{{
    std::byte{12},  std::byte{8},   std::byte{8},   std::byte{1},   std::byte{0},   std::byte{1},   std::byte{0},
    std::byte{47},  std::byte{0},   std::byte{255}, std::byte{209}, std::byte{15},  std::byte{255}, std::byte{0},
    std::byte{0},   std::byte{0},   std::byte{0},   std::byte{0},   std::byte{0},   std::byte{5},   std::byte{0},
    std::byte{0},   std::byte{0},   std::byte{10},  std::byte{0},   std::byte{0},   std::byte{0},   std::byte{14},
    std::byte{0},   std::byte{0},   std::byte{0},   std::byte{40},  std::byte{224}, std::byte{190}, std::byte{0},
    std::byte{101}, std::byte{40},  std::byte{224}, std::byte{190}, std::byte{0},   std::byte{101}, std::byte{1},
    std::byte{16},  std::byte{147}, std::byte{43},
}};

inline constexpr std::array<std::byte, 38> kRansSingleton8{{
    std::byte{8},   std::byte{8},   std::byte{8},   std::byte{1}, std::byte{0},   std::byte{0}, std::byte{255},
    std::byte{255}, std::byte{0},   std::byte{255}, std::byte{0}, std::byte{0},   std::byte{0}, std::byte{0},
    std::byte{4},   std::byte{0},   std::byte{0},   std::byte{0}, std::byte{8},   std::byte{0}, std::byte{0},
    std::byte{0},   std::byte{12},  std::byte{0},   std::byte{0}, std::byte{0},   std::byte{0}, std::byte{0},
    std::byte{128}, std::byte{0},   std::byte{0},   std::byte{0}, std::byte{128}, std::byte{0}, std::byte{0},
    std::byte{0},   std::byte{128}, std::byte{0},
}};

}  // namespace flitzzip_test
