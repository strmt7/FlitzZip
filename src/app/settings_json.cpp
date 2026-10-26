#include "app/settings_json.hpp"

#include "core/result.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <windows.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace flitzzip::app {
namespace {

// Purpose: Decode bounded settings without the replacement characters allowed by the projection's converter.
// Inputs: Nonempty UTF-8 bytes within the settings budget. Outputs: Exact UTF-16 or an error for invalid encoding.
std::wstring decode_settings_utf8(std::string_view json) {
    const auto count = static_cast<int>(json.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.data(), count, nullptr, 0);
    if (required <= 0) {
        throw ArchiveError("settings JSON is not valid UTF-8");
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, json.data(), count, wide.data(), required) != required) {
        throw ArchiveError("could not decode complete settings JSON");
    }
    return wide;
}

// Purpose: Bound parser nesting before passing untrusted bytes to the platform implementation.
// Inputs: Size-bounded UTF-8 JSON. Outputs: Rejects raw NUL and excessive container depth; the OS validates grammar.
void check_parser_budget(std::string_view json) {
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (const char value : json) {
        if (value == '\0') {
            throw ArchiveError("settings JSON contains a raw NUL byte");
        }
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                quoted = false;
            }
        } else if (value == '"') {
            quoted = true;
        } else if (value == '{' || value == '[') {
            if (++depth > kMaxSettingsJsonNesting) {
                throw ArchiveError("settings JSON exceeds the nesting limit");
            }
        } else if ((value == '}' || value == ']') && depth > 0) {
            --depth;
        }
    }
}

class RuntimeApartment final {
  public:
    RuntimeApartment(const RuntimeApartment&) = delete;
    RuntimeApartment& operator=(const RuntimeApartment&) = delete;

    // Purpose: Initialize WinRT while preserving a caller's existing COM concurrency model.
    // Inputs: The current thread. Outputs: Owns one successful initialization; other failures propagate.
    RuntimeApartment() : result_(RoInitialize(RO_INIT_SINGLETHREADED)) {
        if (result_ != RPC_E_CHANGED_MODE) {
            winrt::check_hresult(result_);
        }
    }

    // Purpose: Balance only the initialization this guard acquired, including S_FALSE.
    // Inputs: Recorded initialization status. Outputs: Leaves preexisting apartment ownership intact.
    ~RuntimeApartment() noexcept {
        if (SUCCEEDED(result_)) {
            RoUninitialize();
        }
    }

  private:
    HRESULT result_;
};

// Purpose: Skip only JSON whitespace in a document whose complete grammar the OS has already validated.
// Inputs: Bounded source and cursor. Outputs: Advances the cursor without crossing the source extent.
void skip_json_space(std::string_view text, std::size_t& position) {
    while (position < text.size() &&
           (text[position] == ' ' || text[position] == '\t' || text[position] == '\r' || text[position] == '\n')) {
        ++position;
    }
}

// Purpose: Locate one raw value without decoding or rounding its contents; this is not a grammar validator.
// Inputs: OS-validated object bytes and the start of a member value. Outputs: Its closing delimiter offset.
std::size_t raw_json_value_end(std::string_view text, std::size_t begin) {
    std::size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t position = begin; position < text.size(); ++position) {
        const char ch = text[position];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                quoted = false;
        } else if (ch == '"')
            quoted = true;
        else if (ch == '[' || ch == '{')
            ++depth;
        else if (ch == '}' || ch == ']') {
            if (depth == 0)
                return position;
            --depth;
        } else if (ch == ',' && depth == 0)
            return position;
    }
    throw ArchiveError("settings object has no complete member boundary");
}

// Purpose: Retain exact member bytes after OS grammar validation, including unknown large integers and subtrees.
// Inputs: Complete bounded object and apartment-owned platform factory. Outputs: Decoded keys and raw members;
// duplicate keys retain the last member, matching the platform's typed view. No COM object escapes this call.
std::map<std::wstring, std::string, std::less<>>
capture_raw_members(std::string_view text, const winrt::Windows::Data::Json::IJsonObjectStatics& factory) {
    std::map<std::wstring, std::string, std::less<>> members;
    std::size_t position = 0;
    skip_json_space(text, position);
    if (position == text.size() || text[position++] != '{')
        throw ArchiveError("settings must be an object");
    for (;;) {
        skip_json_space(text, position);
        if (position == text.size())
            throw ArchiveError("settings object is incomplete");
        if (text[position] == '}')
            break;
        const auto begin = position;
        if (text[position++] != '"')
            throw ArchiveError("settings member has no quoted key");
        bool escaped = false;
        while (position < text.size()) {
            const char ch = text[position++];
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                break;
        }
        const auto key_end = position;
        skip_json_space(text, position);
        if (position == text.size() || text[position++] != ':')
            throw ArchiveError("settings member has no value");
        winrt::Windows::Data::Json::JsonObject key_object{nullptr};
        const std::string key_document = "{" + std::string(text.substr(begin, key_end - begin)) + ":null}";
        if (!factory.TryParse(decode_settings_utf8(key_document), key_object))
            throw ArchiveError("could not decode the settings member name");
        const std::wstring key(key_object.First().Current().Key());
        const auto end = raw_json_value_end(text, position);
        auto member_end = end;
        while (member_end > begin && (text[member_end - 1] == ' ' || text[member_end - 1] == '\t' ||
                                      text[member_end - 1] == '\r' || text[member_end - 1] == '\n'))
            --member_end;
        members.insert_or_assign(key, std::string(text.substr(begin, member_end - begin)));
        position = end;
        if (position == text.size() || (text[position] != ',' && text[position] != '}'))
            throw ArchiveError("settings member boundary is invalid");
        if (text[position] == '}')
            break;
        ++position;
    }
    return members;
}

}  // namespace

// Purpose: Parse complete bounded settings with the OS JSON grammar and copy independent scalar values.
// Inputs: UTF-8 bytes, optionally BOM-prefixed. Outputs: Top-level fields, with the platform's last duplicate
// member winning; malformed documents fail before any setting can be consumed.
SettingsJson::SettingsJson(std::string_view json) try {
    if (json.empty() || json.size() > kMaxSettingsJsonBytes) {
        throw ArchiveError("settings JSON is empty or exceeds the size limit");
    }
    if (json.starts_with("\xEF\xBB\xBF")) {
        json.remove_prefix(3);
    }
    check_parser_budget(json);
    const auto text = decode_settings_utf8(json);
    const RuntimeApartment apartment;
    using namespace winrt::Windows::Data::Json;
    // Request only the registered OS implementation: no cached apartment pointer or DLL-search fallback.
    // Both factory and JSON objects must be released before this temporary apartment is uninitialized.
    const winrt::hstring class_name(winrt::name_of<JsonObject>());
    IJsonObjectStatics factory{nullptr};
    winrt::check_hresult(RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(class_name)),
                                                winrt::guid_of<IJsonObjectStatics>(), winrt::put_abi(factory)));
    JsonObject object{nullptr};
    if (!factory.TryParse(text, object)) {
        throw ArchiveError("settings file must contain one complete JSON object");
    }
    for (const auto& member : object) {
        const auto name = member.Key();
        const auto value = member.Value();
        Scalar scalar;
        switch (value.ValueType()) {
        case JsonValueType::Boolean:
            scalar = value.GetBoolean();
            break;
        case JsonValueType::Number:
            scalar = value.GetNumber();
            break;
        case JsonValueType::String:
            scalar = std::wstring(value.GetString());
            break;
        default:
            break;
        }
        fields_.emplace(std::wstring(name), std::move(scalar));
    }
    raw_members_ = capture_raw_members(json, factory);
} catch (const winrt::hresult_error&) {
    throw ArchiveError("Windows could not decode the settings JSON");
}

// Purpose: Detect a top-level member. Inputs: Exact decoded key. Outputs: Presence independent of type.
bool SettingsJson::contains(std::wstring_view key) const {
    return fields_.contains(key);
}

// Purpose: Preserve unowned fields exactly while replacing explicitly supplied member values.
// Inputs: Validated updates, optional baseline and coupled keys; documents own all borrowed bytes during this call.
// Outputs: Complete bounded JSON or an error, preserving unchanged fields despite runtime normalization.
std::string SettingsJson::overlay(const SettingsJson& updates, const SettingsJson* baseline,
                                  std::span<const std::wstring_view> coupled_keys) const {
    auto merged = raw_members_;
    bool coupled_changed = baseline == nullptr;
    for (const auto name : coupled_keys) {
        if (!updates.raw_members_.contains(name) || (baseline && !baseline->raw_members_.contains(name)))
            throw ArchiveError("coupled settings update is incomplete");
    }
    if (baseline) {
        for (const auto name : coupled_keys) {
            const auto before = baseline->raw_members_.find(name);
            const auto after = updates.raw_members_.find(name);
            if (before == baseline->raw_members_.end() || after == updates.raw_members_.end() ||
                before->second != after->second)
                coupled_changed = true;
        }
    }
    for (const auto& [name, value] : updates.raw_members_) {
        const auto before = baseline ? baseline->raw_members_.find(name) : updates.raw_members_.end();
        if (!baseline || before == baseline->raw_members_.end() || before->second != value ||
            (coupled_changed && std::find(coupled_keys.begin(), coupled_keys.end(), name) != coupled_keys.end()))
            merged.insert_or_assign(name, value);
    }
    std::string output = "{\n";
    bool first = true;
    for (const auto& [name, value] : merged) {
        if (!first)
            output += ",\n";
        first = false;
        output += "  " + value;
        if (output.size() > kMaxSettingsJsonBytes - 3)
            throw ArchiveError("merged settings exceed the size limit");
    }
    output += "\n}\n";
    return output;
}

// Purpose: Compare a string setting by type and complete value. Inputs: Key and value. Outputs: Exact match.
bool SettingsJson::string_equals(std::wstring_view key, std::wstring_view value) const {
    const auto found = fields_.find(key);
    const auto* text = found == fields_.end() ? nullptr : std::get_if<std::wstring>(&found->second);
    return text != nullptr && *text == value;
}

// Purpose: Read a typed boolean. Inputs: Key and default. Outputs: Stored boolean or the explicit default.
bool SettingsJson::boolean(std::wstring_view key, bool fallback) const {
    const auto found = fields_.find(key);
    const auto* value = found == fields_.end() ? nullptr : std::get_if<bool>(&found->second);
    return value == nullptr ? fallback : *value;
}

// Purpose: Normalize an integral JSON number without a narrowing conversion outside the int range.
// Inputs: Key, default and ordered bounds. Outputs: Bounded integer or explicit default for an invalid scalar.
int SettingsJson::integer(std::wstring_view key, int fallback, int minimum, int maximum) const {
    if (minimum > maximum) {
        throw ArchiveError("settings integer bounds are reversed");
    }
    const auto found = fields_.find(key);
    const auto* value = found == fields_.end() ? nullptr : std::get_if<double>(&found->second);
    if (value == nullptr || !std::isfinite(*value) || *value < std::numeric_limits<int>::min() ||
        *value > std::numeric_limits<int>::max()) {
        return fallback;
    }
    double integral = 0.0;
    if (std::modf(*value, &integral) != 0.0) {
        return fallback;
    }
    return std::clamp(static_cast<int>(integral), minimum, maximum);
}

// Purpose: Consume at most one byte beyond the settings budget to distinguish an exact-size document.
// Inputs: Open binary stream. Outputs: Complete bytes, with stream errors and oversized input rejected.
std::string read_settings_json_text(std::istream& input) {
    std::string bytes(kMaxSettingsJsonBytes + 1U, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || (input.fail() && !input.eof())) {
        throw ArchiveError("could not read complete settings file");
    }
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > kMaxSettingsJsonBytes) {
        throw ArchiveError("settings file exceeds the size limit");
    }
    bytes.resize(count);
    return bytes;
}

}  // namespace flitzzip::app
