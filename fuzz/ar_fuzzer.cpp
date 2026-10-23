#include "ar/ar_reader.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

// Purpose: Exercise the production AR reader and require complete input before accepting a publication plan.
// Inputs: LibFuzzer-owned bytes, bounded to one MiB; each stream owns its RAM-only copy.
// Outputs: Returns after valid parsing or expected archive rejection; sanitizer or completeness failures terminate.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size > 1024U * 1024U) {
        return 0;
    }
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    std::istringstream complete(bytes, std::ios::in | std::ios::binary);
    try {
        const auto scan = flitzzip::detail::scan_ar_stream(complete, size, flitzzip::ArchivePathEncoding::Utf8);
        std::uint64_t total = 0;
        for (const auto& entry : scan.entries) {
            if (entry.payload_offset > size || entry.size > size - entry.payload_offset) {
                std::abort();
            }
            total += entry.size;
        }
        if (total != scan.total_file_bytes) {
            std::abort();
        }
    } catch (const flitzzip::Error&) {
        return 0;
    }
    // A valid AR always contains its global header. Keep the declared extent while removing one physical byte.
    if (bytes.empty()) {
        std::abort();
    }
    std::istringstream truncated(bytes.substr(0, size - 1U), std::ios::in | std::ios::binary);
    try {
        (void)flitzzip::detail::scan_ar_stream(truncated, size, flitzzip::ArchivePathEncoding::Utf8);
    } catch (const flitzzip::Error&) {
        return 0;
    }
    std::abort();
}
