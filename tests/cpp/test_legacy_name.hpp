#pragma once

#include "test_util.hpp"

#include <array>
#include <string>
#include <utility>

#include <windows.h>

// Purpose: Choose a non-ASCII filename that is exactly representable in the host ANSI code page.
// Inputs: The actual Windows code page, including UTF-8 and multibyte configurations.
// Outputs: Returns raw bytes and their independently round-tripped wide filename; never uses best-fit substitutions.
inline std::pair<std::string, std::wstring> test_legacy_archive_name() {
    constexpr std::array candidates{L"\u00e9.txt", L"\u65e5.txt", L"\u0430.txt", L"\u03b1.txt",
                                    L"\u0627.txt", L"\u05d0.txt", L"\u0e01.txt", L"\uac00.txt"};
    const auto page = GetACP();
    for (const std::wstring candidate : candidates) {
        std::array<char, 64> bytes{};
        BOOL substituted = FALSE;
        const auto count =
            WideCharToMultiByte(page, page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS, candidate.data(),
                                static_cast<int>(candidate.size()), bytes.data(), static_cast<int>(bytes.size()),
                                nullptr, page == CP_UTF8 ? nullptr : &substituted);
        if (count <= 0 || substituted) {
            continue;
        }
        std::array<wchar_t, 64> decoded{};
        const auto units = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, bytes.data(), count, decoded.data(),
                                               static_cast<int>(decoded.size()));
        if (units > 0 && std::wstring(decoded.data(), static_cast<std::size_t>(units)) == candidate) {
            return {std::string(bytes.data(), static_cast<std::size_t>(count)), candidate};
        }
    }
    throw std::runtime_error("no exact non-ASCII fixture available for the active Windows ANSI code page");
}
