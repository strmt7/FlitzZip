#include "core/archive_name_encoding.hpp"

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace superzip {
namespace {

// Purpose: Validate UTF-8 scalar encoding without locale-dependent conversions.
// Inputs: A byte sequence bounded by the archive path limit.
// Outputs: Throws on truncated, overlong, surrogate, out-of-range, or invalid continuation sequences.
void validate_utf8_name(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset++]);
        if (first < 0x80U) {
            continue;
        }
        const unsigned length = first >= 0xc2U && first <= 0xdfU   ? 2U
                                : first >= 0xe0U && first <= 0xefU ? 3U
                                : first >= 0xf0U && first <= 0xf4U ? 4U
                                                                   : 0U;
        if (length == 0U || text.size() - offset < length - 1U) {
            throw ArchiveError("archive member name is not valid UTF-8");
        }
        std::uint32_t scalar = first & (0x7fU >> length);
        for (unsigned index = 1; index < length; ++index) {
            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0U) != 0x80U) {
                throw ArchiveError("archive member name is not valid UTF-8");
            }
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        const std::uint32_t minimum = length == 2U ? 0x80U : length == 3U ? 0x800U : 0x10000U;
        if (scalar < minimum || scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
            throw ArchiveError("archive member name is not valid UTF-8");
        }
    }
}

#ifdef _WIN32
// Purpose: Convert explicitly selected Windows ANSI metadata without lossy decoding.
// Inputs: Nonempty raw bytes already checked against the archive path limit.
// Outputs: Returns bounded UTF-8 or throws on conversion failure before allocating an oversized result.
std::string decode_windows_name(std::string_view raw) {
    const auto code_page = GetACP();
    const auto count =
        MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (count <= 0) {
        throw ArchiveError("cannot decode archive member name using Windows ANSI");
    }
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), wide.data(),
                            count) != count) {
        throw ArchiveError("cannot decode archive member name using Windows ANSI");
    }
    const auto bytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || static_cast<std::uint32_t>(bytes) > kMaxArchivePathBytes) {
        throw ArchiveError("decoded archive member name is invalid or exceeds path limits");
    }
    std::string decoded(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), count, decoded.data(), bytes, nullptr,
                            nullptr) != bytes) {
        throw ArchiveError("cannot encode archive member name as UTF-8");
    }
    return decoded;
}
#endif

}  // namespace

// Purpose: Keep CLI encoding selection aligned with the shared product options.
// Inputs: A stable encoding key from command-line arguments.
// Outputs: Returns a validated encoding or throws for unknown or unavailable choices.
ArchivePathEncoding parse_archive_name_encoding(std::string_view token) {
    for (const auto& choice : kArchiveNameEncodingChoices) {
        if (choice.key == token) {
            validate_archive_name_encoding(choice.encoding);
            return choice.encoding;
        }
    }
    throw ArchiveError("--name-encoding must be utf8 or system");
}

// Purpose: Validate archive-name encoding independently of archive contents.
// Inputs: A caller-selected enum, including potentially invalid values.
// Outputs: Returns for supported modes or throws before any extraction side effects.
void validate_archive_name_encoding(ArchivePathEncoding encoding) {
    if (encoding == ArchivePathEncoding::Utf8) {
        return;
    }
#ifdef _WIN32
    if (encoding == ArchivePathEncoding::HostCodePage) {
        return;
    }
#endif
    throw ArchiveError("unsupported archive member name encoding");
}

// Purpose: Normalize explicitly encoded archive metadata before path validation.
// Inputs: Raw member name bytes and the caller-selected encoding.
// Outputs: Returns bounded strict UTF-8, never guessing another encoding on failure.
std::string decode_archive_name(std::string_view raw, ArchivePathEncoding encoding) {
    validate_archive_name_encoding(encoding);
    if (raw.empty() || raw.size() > kMaxArchivePathBytes || raw.find('\0') != std::string_view::npos) {
        throw ArchiveError("archive member name is empty, contains NUL, or exceeds path limits");
    }
    if (encoding == ArchivePathEncoding::Utf8) {
        validate_utf8_name(raw);
        return std::string(raw);
    }
#ifdef _WIN32
    return decode_windows_name(raw);
#else
    throw ArchiveError("Windows ANSI archive names are unsupported on this platform");
#endif
}

}  // namespace superzip
