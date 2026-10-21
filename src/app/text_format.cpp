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
