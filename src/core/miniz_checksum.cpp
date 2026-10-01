#include "core/checksum.hpp"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

// Purpose: Supply Miniz's supported external IEEE CRC hook through the shared capability-aware backend.
// Inputs: crc is a finalized 32-bit seed; ptr borrows buf_len readable bytes, or null requests CRC initialization.
// Outputs: Returns a finalized 32-bit CRC without allocation or exceptions; a non-null empty range preserves seed.
extern "C" mz_ulong mz_crc32(mz_ulong crc, const unsigned char* ptr, size_t buf_len) {
    if (ptr == nullptr) {
        return MZ_CRC32_INIT;
    }
    const auto bytes = std::as_bytes(std::span(ptr, buf_len));
    return superzip::crc32(bytes, static_cast<std::uint32_t>(crc));
}
