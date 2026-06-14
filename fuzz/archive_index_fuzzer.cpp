#include "core/archive_index.hpp"
#include "core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

// Purpose: Feed arbitrary bytes into the SuperZip archive-index parser.
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
    return 0;
}
