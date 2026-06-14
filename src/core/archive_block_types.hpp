#pragma once

#include <cstdint>

namespace superzip {

enum class BlockKind : std::uint8_t {
    Raw = 0,
    Fill = 1,
    Deflate = 2,
    Pattern = 3,
};

struct BlockDescriptor {
    BlockKind kind = BlockKind::Raw;
    std::uint8_t fill_value = 0;
    std::uint32_t uncompressed_len = 0;
    std::uint64_t encoded_offset = 0;
    std::uint32_t encoded_len = 0;
};

}  // namespace superzip
