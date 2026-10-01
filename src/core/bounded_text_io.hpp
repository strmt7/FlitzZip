#pragma once

#include "core/result.hpp"

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace superzip {

// Purpose: Read one archive text line without allocating beyond its format's metadata limit.
// Inputs: `input` supplies bytes, `line` receives CR/LF-stripped content, and `limit`/`format` define
// policy/diagnostics. Outputs: Returns true for a line, false at clean EOF; throws for overlong metadata or failed I/O.
inline bool read_bounded_text_line(std::istream& input, std::string& line, std::size_t limit, std::string_view format) {
    line.clear();
    std::istream::int_type next = std::char_traits<char>::eof();
    while (!std::char_traits<char>::eq_int_type((next = input.get()), std::char_traits<char>::eof())) {
        const auto byte = static_cast<unsigned char>(std::char_traits<char>::to_char_type(next));
        if (byte == static_cast<unsigned char>('\n')) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }
        if (line.size() >= limit) {
            throw ArchiveError(std::string(format) + " line exceeds SuperZip metadata limit");
        }
        line.push_back(static_cast<char>(byte));
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        throw ArchiveError("failed to read " + std::string(format) + " stream");
    }
    if (line.empty()) {
        return false;
    }
    if (line.back() == '\r') {
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

}  // namespace superzip
