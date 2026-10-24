#pragma once

#include "core/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace flitzzip::tar_detail {

// Purpose: Copy bounded TAR string metadata without allowing an offset/width pair to escape its header.
// Inputs: A field view and encoded metadata; the header's caller initializes any unused bytes.
// Outputs: Copies only the value bytes, or throws before modifying an undersized field.
template <std::size_t Width> void put_tar_string(std::span<char, Width> field, std::string_view value) {
    if (value.size() > field.size()) {
        throw ArchiveError("TAR header field is too long: " + std::string(value));
    }
    std::copy(value.begin(), value.end(), field.begin());
}

// Purpose: Encode the numeric field widths used by USTAR headers, with GNU positive base-256 overflow encoding.
// Inputs: A compile-time bounded checksum, identity/mode, or size/time field and one unsigned value.
// Outputs: Writes exact octal/NUL or GNU bytes; an unrepresentable value leaves the entire field unchanged.
template <std::size_t Width>
    requires(Width == 7 || Width == 8 || Width == 12)
void put_tar_number(std::span<char, Width> field, std::uint64_t value) {
    constexpr std::uint64_t octal_limit = std::uint64_t{1} << ((Width - 1U) * 3U);
    auto remaining = value;
    if (value < octal_limit) {
        for (std::size_t index = Width - 1U; index > 0; --index) {
            field[index - 1U] = static_cast<char>('0' + (remaining & 7U));
            remaining >>= 3U;
        }
        field[Width - 1U] = '\0';
        return;
    }
    if constexpr (Width <= sizeof(value)) {
        constexpr std::uint64_t binary_limit = std::uint64_t{1} << ((Width - 1U) * 8U);
        if (value >= binary_limit) {
            throw ArchiveError("TAR numeric field cannot fit value");
        }
    }
    for (std::size_t index = Width - 1U; index > 0; --index) {
        field[index] = static_cast<char>(remaining & 0xFFU);
        remaining >>= 8U;
    }
    field[0] = static_cast<char>(0x80U);
}

}  // namespace flitzzip::tar_detail
