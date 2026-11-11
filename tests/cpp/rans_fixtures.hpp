#pragma once

// Fixed payloads from the independently differential-qualified integer oracle.
// These are correctness fixtures, not compression measurements.
#include <array>
#include <cstddef>

namespace flitzzip_test {
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
