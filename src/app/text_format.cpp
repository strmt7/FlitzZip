#include "app/main_window_support.hpp"

#include "core/result.hpp"

#include <climits>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace flitzzip::app {

// Purpose: Convert validated UTF-8 text to UTF-16 for Win32 rendering.
// Inputs: `value` is bounded UTF-8 text from product state or archive metadata.
// Outputs: Returns UTF-16 text, or throws ArchiveError instead of silently losing malformed or excessive text.
std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        throw ArchiveError("text is too large for Win32 UTF-8 conversion");
    }
    const auto length = static_cast<int>(value.size());
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (needed <= 0) {
        throw ArchiveError("text is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), needed) != needed) {
        throw ArchiveError("UTF-8 text conversion failed");
    }
    return result;
}

// Purpose: Display opaque exception diagnostics without interpreting bytes through the host ANSI code page.
// Inputs: UTF-8 text or arbitrary narrow exception bytes, bounded by the Win32 conversion limit.
// Outputs: Preserves valid Unicode; explicitly labels malformed input and escapes its non-ASCII bytes exactly.
std::wstring diagnostic_text(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        throw ArchiveError("diagnostic is too large for Win32 text conversion");
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) >
        0) {
        return widen(value);
    }
    if (GetLastError() != ERROR_NO_UNICODE_TRANSLATION) {
        throw ArchiveError("diagnostic text conversion failed");
    }
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result = L"[invalid UTF-8 bytes] ";
    for (const unsigned char byte : value) {
        if (byte >= 0x20 && byte <= 0x7E && byte != '\\') {
            result.push_back(static_cast<wchar_t>(byte));
        } else {
            result += L"\\x";
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0F]);
        }
    }
    return result;
}

// Purpose: Format a nonnegative byte count with binary units.
// Inputs: `value` is a finite byte count or byte-per-second value.
// Outputs: Returns a compact display string such as `1.5 MiB`, or throws for invalid telemetry.
std::string human_bytes(double value) {
    if (!std::isfinite(value) || value < 0.0) {
        throw ArchiveError("byte count must be finite and nonnegative");
    }
    constexpr std::array<std::string_view, 5> units{"B", "KiB", "MiB", "GiB", "TiB"};
    std::size_t unit = 0U;
    while (value >= 1024.0 && unit + 1U < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0U ? 0 : 1) << value << ' ' << units[unit];
    return out.str();
}

}  // namespace flitzzip::app
