#pragma once

#include "core/result.hpp"

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace flitzzip {

// Purpose: Read one archive text line without allocating beyond its format's metadata limit.
// Inputs: `input` supplies bytes, `line` receives CR/LF-stripped content, and `limit`/`format` define
// policy/diagnostics. Optional physical_bytes receives the consumed extent,
// including stripped CR/LF, on return. Returns true for a line, false at clean
// EOF; throws for overlong metadata or failed I/O without an extent guarantee.
inline bool read_bounded_text_line(std::istream& input, std::string& line, std::size_t limit, std::string_view format,
                                   std::size_t* physical_bytes = nullptr) {
    line.clear();
    std::size_t consumed = 0;
    bool terminated = false;
    std::istream::int_type next = std::char_traits<char>::eof();
    while (!std::char_traits<char>::eq_int_type((next = input.get()), std::char_traits<char>::eof())) {
        ++consumed;
        const auto byte = static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        if (byte == static_cast<unsigned char>('\n')) {
            terminated = true;
            break;
        }
        if (line.size() >= limit) {
            throw ArchiveError(std::string(format) + " line exceeds FlitzZip metadata limit");
        }
        line.push_back(static_cast<char>(byte));
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        throw ArchiveError("failed to read " + std::string(format) + " stream");
    }
    if (physical_bytes != nullptr) {
        *physical_bytes = consumed;
    }
    if (!terminated && line.empty()) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

// Purpose: Write one archive text line and report any incomplete stream write.
// Inputs: `output` is the destination, `line` excludes its LF terminator, and `format` labels diagnostics.
// Outputs: Appends the exact line plus LF or throws on I/O failure.
inline void write_archive_text_line(std::ostream& output, std::string_view line, std::string_view format) {
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    if (!output) {
        throw ArchiveError("failed to write " + std::string(format) + " stream");
    }
}

}  // namespace flitzzip
