#include "core/archive_index.hpp"
#include "core/compound_block.hpp"
#include "core/byte_plane_block.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

// Purpose: Feed arbitrary bytes into the native index and closed GPU stage/frame parsers.
// Inputs: `data` and `size` are libFuzzer-owned bytes for one fuzz iteration.
// Outputs: Returns 0 after successful parsing or expected parser rejection; sanitizer findings crash the process.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size > 1U * 1024U * 1024U) {
        return 0;
    }

    const std::string bytes(reinterpret_cast<const char*>(data), size);
    std::istringstream input(bytes, std::ios::binary);
    try {
        (void)superzip::read_archive_index(input);
    } catch (const superzip::Error&) {
    }
    if (size >= sizeof(std::uint32_t)) {
        std::uint32_t decoded_bytes = 0U;
        for (std::size_t byte = 0U; byte < sizeof(decoded_bytes); ++byte) {
            decoded_bytes |= static_cast<std::uint32_t>(data[byte]) << (byte * 8U);
        }
        const auto payload =
            std::span(reinterpret_cast<const std::byte*>(data + sizeof(decoded_bytes)), size - sizeof(decoded_bytes));
        try {
            (void)superzip::parse_gpu_compound_block(payload,
                                                     {.kind = superzip::BlockKind::GpuCompound,
                                                      .uncompressed_len = decoded_bytes,
                                                      .encoded_len = static_cast<std::uint32_t>(payload.size())});
        } catch (const superzip::Error&) {
        }
        try {
            (void)superzip::parse_gpu_byte_plane_contexts(payload,
                                                          {.kind = superzip::BlockKind::GpuBytePlaneContexts,
                                                           .uncompressed_len = decoded_bytes,
                                                           .encoded_len = static_cast<std::uint32_t>(payload.size())});
        } catch (const superzip::Error&) {
        }
        try {
            (void)superzip::parse_gpu_byte_plane_block(payload,
                                                       {.kind = superzip::BlockKind::GpuBytePlane,
                                                        .uncompressed_len = decoded_bytes,
                                                        .encoded_len = static_cast<std::uint32_t>(payload.size())});
        } catch (const superzip::Error&) {
        }
    }
    return 0;
}
