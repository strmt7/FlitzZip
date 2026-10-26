#pragma once

#include <cstddef>
#include <istream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace flitzzip::app {

inline constexpr std::size_t kMaxSettingsJsonBytes = 64U * 1024U;
inline constexpr std::size_t kMaxSettingsJsonNesting = 64U;

// A bounded settings document decoded by the Windows JSON implementation.
// Typed scalars and original member bytes are retained. No COM object escapes construction.
class SettingsJson final {
  public:
    // Purpose: Decode one complete UTF-8 JSON object using the platform parser.
    // Inputs: At most kMaxSettingsJsonBytes bytes; an optional UTF-8 BOM is accepted.
    // Outputs: Owns decoded top-level scalars; malformed input or runtime failures throw.
    explicit SettingsJson(std::string_view json);

    // Purpose: Distinguish an absent schema from a present field of any JSON type.
    // Inputs: Exact, case-sensitive decoded field name. Outputs: True only for a top-level member.
    [[nodiscard]] bool contains(std::wstring_view key) const;

    // Purpose: Compare an explicitly typed string without coercion or substring matching.
    // Inputs: Exact field name and expected decoded text. Outputs: False for missing or non-string fields.
    [[nodiscard]] bool string_equals(std::wstring_view key, std::wstring_view value) const;

    // Purpose: Read a boolean setting without accepting text, numbers or nested fields.
    // Inputs: Exact field name and default. Outputs: The boolean value, or the default for absent/wrong types.
    [[nodiscard]] bool boolean(std::wstring_view key, bool fallback) const;

    // Purpose: Read a whole-number setting within its documented range.
    // Inputs: Exact key, default and ordered inclusive bounds. Outputs: Clamped int, or default for wrong types,
    // fractions and values outside the int representation; invalid bounds throw.
    [[nodiscard]] int integer(std::wstring_view key, int fallback, int minimum, int maximum) const;

    // Purpose: Overlay explicit fields while preserving unknown JSON values without numeric/string conversion.
    // Inputs: Validated updates, optional normalized UI baseline and preference keys that must change together.
    // Outputs: Bounded deterministic JSON; unchanged/unknown values survive without numeric conversion.
    [[nodiscard]] std::string overlay(const SettingsJson& updates, const SettingsJson* baseline = nullptr,
                                      std::span<const std::wstring_view> coupled_keys = {}) const;

  private:
    using Scalar = std::variant<std::monostate, bool, double, std::wstring>;
    std::map<std::wstring, Scalar, std::less<>> fields_;
    std::map<std::wstring, std::string, std::less<>> raw_members_;
};

// Purpose: Read a settings stream without loading an unbounded or partially failed file.
// Inputs: A binary input stream. Outputs: Complete bounded bytes; oversized content and read errors throw.
[[nodiscard]] std::string read_settings_json_text(std::istream& input);

}  // namespace flitzzip::app
