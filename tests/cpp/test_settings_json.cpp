#include "app/settings_json.hpp"
#include "core/result.hpp"
#include "test_util.hpp"

#include <array>
#include <exception>
#include <limits>
#include <sstream>
#include <thread>

#include <objbase.h>

namespace {

// Purpose: Require malformed settings to stop before exposing any partial values.
// Inputs: One candidate document. Outputs: Fails unless the production parser raises the application error type.
void require_rejected_settings(std::string_view json) {
    bool rejected = false;
    try {
        const flitzzip::app::SettingsJson settings(json);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("malformed settings unexpectedly accepted: " + std::string(json));
    }
}

// Purpose: Model an I/O failure during a settings read, independently of file permissions or machine state.
// Inputs: Any attempted read. Outputs: Throws so istream records a transport error.
class FailingSettingsBuffer final : public std::streambuf {
    std::streamsize xsgetn(char*, std::streamsize) override {
        throw std::runtime_error("settings read failure");
    }
};

// Purpose: Check WinRT initialization ownership on a fresh thread and through invalid-input cleanup.
// Inputs: Existing apartment mode, or -1 for an uninitialized caller. Outputs: Leaves the original model/count intact.
void check_settings_apartment(int mode) {
    APTTYPE type{};
    APTTYPEQUALIFIER qualifier{};
    REQUIRE_EQ(CoGetApartmentType(&type, &qualifier), CO_E_NOTINITIALIZED);
    APTTYPE original_type{};
    APTTYPEQUALIFIER original_qualifier{};
    if (mode >= 0) {
        REQUIRE_EQ(CoInitializeEx(nullptr, static_cast<DWORD>(mode)), S_OK);
        REQUIRE_EQ(CoGetApartmentType(&original_type, &original_qualifier), S_OK);
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        const flitzzip::app::SettingsJson settings(R"({"enabled":true})");
        REQUIRE_TRUE(settings.boolean(L"enabled", false));
        require_rejected_settings(R"({"enabled":false trailing})");
    }
    if (mode >= 0) {
        REQUIRE_EQ(CoGetApartmentType(&type, &qualifier), S_OK);
        REQUIRE_EQ(type, original_type);
        REQUIRE_EQ(qualifier, original_qualifier);
        CoUninitialize();
    }
    REQUIRE_EQ(CoGetApartmentType(&type, &qualifier), CO_E_NOTINITIALIZED);
}

}  // namespace

// Purpose: Prevent key-looking string values and nested members from changing top-level settings.
// Inputs: Independent reproductions of the former substring lookup defects. Outputs: Only root members apply.
TEST_CASE(settings_json_uses_only_typed_top_level_members) {
    const flitzzip::app::SettingsJson settings(
        R"({"legacy":{"overwrite":true,"compressionLevel":9},"description":"overwrite",
            "unrelated":true,"overwrite":false,"compressionLevel":3})");
    REQUIRE_TRUE(!settings.boolean(L"overwrite", true));
    REQUIRE_EQ(settings.integer(L"compressionLevel", 5, 1, 9), 3);
    REQUIRE_TRUE(settings.contains(L"legacy"));
    REQUIRE_TRUE(!settings.contains(L"missing"));
    REQUIRE_TRUE(!settings.boolean(L"Overwrite", false));
    REQUIRE_TRUE(settings.string_equals(L"description", L"overwrite"));
    REQUIRE_TRUE(!settings.string_equals(L"overwrite", L"false"));
    REQUIRE_TRUE(!settings.string_equals(L"missing", L""));
    const flitzzip::app::SettingsJson shadow(
        R"({"description":"compressionLevel","unrelated":9,"compressionLevel":3})");
    REQUIRE_EQ(shadow.integer(L"compressionLevel", 5, 1, 9), 3);
}

// Purpose: Keep malformed documents from enabling overwrite/GPU options through a valid-looking prefix.
// Inputs: Truncations, invalid tokens, extra documents and invalid separators. Outputs: Every document is rejected.
TEST_CASE(settings_json_rejects_malformed_whole_documents) {
    for (const auto document : {"",
                                " ",
                                "[]",
                                "null",
                                "true",
                                "1",
                                "\"object\"",
                                "{",
                                "}",
                                "{}{}",
                                "{} trailing",
                                R"({"overwrite":truejunk})",
                                R"({"overwrite":false0})",
                                R"({"overwrite":TRUE})",
                                R"({"overwrite":false,})",
                                R"({"overwrite":false "extra":true})",
                                R"({"overwrite" false})",
                                R"({"overwrite":})",
                                R"({overwrite:true})",
                                R"({"level":01})",
                                R"({"level":+1})",
                                R"({"level":1.})",
                                R"({"level":1e})",
                                R"({"level":NaN})",
                                R"({"level":Infinity})",
                                R"({"text":"unterminated})",
                                R"({"text":"\q"})",
                                R"({"text":"\u123"})",
                                R"({"unknown":[1,]})",
                                R"({/* comment */"overwrite":true})"}) {
        require_rejected_settings(document);
    }
    require_rejected_settings(std::string("{}\0trailing", 11));
    require_rejected_settings(std::string("{\"text\":\"\xC0\xAF\"}"));
    require_rejected_settings(std::string("{\"text\":\"\xED\xA0\x80\"}"));
}

// Purpose: Enforce field types independently from truthiness and text conversion.
// Inputs: Every JSON type at a boolean/integer setting. Outputs: Only the exact expected type is consumed.
TEST_CASE(settings_json_requires_exact_scalar_types) {
    for (const auto value : {"null", "0", "1", "-1", "0.5", "\"true\"", "\"false\"", "[]", "{}"}) {
        const flitzzip::app::SettingsJson settings(std::string("{\"flag\":") + value + "}");
        REQUIRE_TRUE(!settings.boolean(L"flag", false));
        REQUIRE_TRUE(settings.boolean(L"flag", true));
    }
    for (const auto value :
         {"null", "true", "false", "\"7\"", "[]", "{}", "1.5", "2147483648", "-2147483649", "1e100"}) {
        const flitzzip::app::SettingsJson settings(std::string("{\"level\":") + value + "}");
        REQUIRE_EQ(settings.integer(L"level", 3, 1, 9), 3);
    }
    const flitzzip::app::SettingsJson empty("{}");
    REQUIRE_EQ(empty.integer(L"level", 3, 1, 9), 3);
    REQUIRE_TRUE(empty.boolean(L"flag", true));
}

// Purpose: Preserve integer bounds and safely reject unrepresentable values before narrowing.
// Inputs: Exact int endpoints, supported numeric spellings and values around the application interval.
// Outputs: Independent expected clamping results, without overflow or fractional truncation.
TEST_CASE(settings_json_clamps_representable_whole_numbers) {
    for (const auto& [text, expected] : std::array<std::pair<const char*, int>, 9>{{{"-2147483648", 1},
                                                                                    {"2147483647", 9},
                                                                                    {"-1", 1},
                                                                                    {"0", 1},
                                                                                    {"1", 1},
                                                                                    {"5", 5},
                                                                                    {"9", 9},
                                                                                    {"10", 9},
                                                                                    {"7e0", 7}}}) {
        const flitzzip::app::SettingsJson settings(std::string("{\"level\":") + text + "}");
        REQUIRE_EQ(settings.integer(L"level", 3, 1, 9), expected);
    }
    const flitzzip::app::SettingsJson ends(R"({"low":-2147483648,"high":2147483647,"zero":-0})");
    REQUIRE_EQ(ends.integer(L"low", 7, INT_MIN, INT_MAX), INT_MIN);
    REQUIRE_EQ(ends.integer(L"high", 7, INT_MIN, INT_MAX), INT_MAX);
    REQUIRE_EQ(ends.integer(L"zero", 7, INT_MIN, INT_MAX), 0);
    bool rejected = false;
    try {
        (void)ends.integer(L"low", 7, 9, 1);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Verify JSON escape decoding, schema identity and the OS's documented duplicate-member semantics.
// Inputs: Escaped keys, a misleading schema value, Unicode text and duplicate boolean keys.
// Outputs: Exact decoded matching; the last duplicate wins consistently with Windows.Data.Json.
TEST_CASE(settings_json_decodes_keys_and_preserves_schema_identity) {
    const flitzzip::app::SettingsJson settings(
        R"({"\u0073chema":"flitzzip.settings.v3","note":"flitzzip.settings.v1",
            "\u006fverwrite":true,"overwrite":false,"unicode":"\u03A9\uD83D\uDE80","escaped":"a\"b\\c"})");
    REQUIRE_TRUE(settings.contains(L"schema"));
    REQUIRE_TRUE(settings.string_equals(L"schema", L"flitzzip.settings.v3"));
    REQUIRE_TRUE(!settings.string_equals(L"schema", L"flitzzip.settings.v1"));
    REQUIRE_TRUE(!settings.boolean(L"overwrite", true));
    REQUIRE_TRUE(settings.string_equals(L"unicode", L"\u03A9\U0001F680"));
    REQUIRE_TRUE(settings.string_equals(L"escaped", L"a\"b\\c"));
    const flitzzip::app::SettingsJson bom(std::string("\xEF\xBB\xBF") + "{\"flag\":true}");
    REQUIRE_TRUE(bom.boolean(L"flag", false));
}

// Purpose: Bound document memory and parser recursion, without counting punctuation inside strings as containers.
// Inputs: Documents at and beyond the byte/depth budgets, plus escaped braces and quotes.
// Outputs: Exact limits succeed; excessive bytes/depth are rejected before platform parsing.
TEST_CASE(settings_json_enforces_size_and_nesting_budgets) {
    std::string padded = "{}";
    padded.resize(flitzzip::app::kMaxSettingsJsonBytes, ' ');
    const flitzzip::app::SettingsJson exact(padded);
    REQUIRE_TRUE(!exact.contains(L"absent"));
    padded.push_back(' ');
    require_rejected_settings(padded);
    std::string nested = "null";
    for (std::size_t level = 0; level < flitzzip::app::kMaxSettingsJsonNesting; ++level) {
        nested = "{\"unknown\":" + nested + "}";
    }
    const flitzzip::app::SettingsJson allowed(nested);
    REQUIRE_TRUE(allowed.contains(L"unknown"));
    require_rejected_settings("{\"unknown\":" + nested + "}");
    const flitzzip::app::SettingsJson quoted("{\"text\":\"" + std::string(500, '[') + "\\\"\\\\\",\"flag\":true}");
    REQUIRE_TRUE(quoted.boolean(L"flag", false));
}

// Purpose: Reject read failures and oversized files without a file-size/read race or unbounded allocation.
// Inputs: In-memory streams with exact limits, a sentinel beyond the budget, and injected transport failures.
// Outputs: Complete bounded reads; oversized/read-error streams raise the application error type.
TEST_CASE(settings_json_stream_read_is_bounded_and_checks_failure) {
    for (const auto& text :
         {std::string{}, std::string("{\"flag\":true}"), std::string(flitzzip::app::kMaxSettingsJsonBytes, ' ')}) {
        std::istringstream input(text);
        REQUIRE_EQ(flitzzip::app::read_settings_json_text(input), text);
    }
    std::istringstream oversized(std::string(flitzzip::app::kMaxSettingsJsonBytes + 100U, ' '));
    FailingSettingsBuffer failure;
    std::istream broken(&failure);
    std::istringstream unavailable("{}");
    unavailable.setstate(std::ios::failbit);
    for (std::istream* input :
         {static_cast<std::istream*>(&oversized), &broken, static_cast<std::istream*>(&unavailable)}) {
        bool rejected = false;
        try {
            (void)flitzzip::app::read_settings_json_text(*input);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    REQUIRE_EQ(oversized.tellg(), std::streampos(flitzzip::app::kMaxSettingsJsonBytes + 1U));
}

// Purpose: Prevent the settings parser from leaking COM initialization or changing a host thread's apartment.
// Inputs: Independent fresh, STA and MTA threads, each including successful and failed parsing.
// Outputs: Exact original apartment state and initialization ownership after all temporary JSON objects are gone.
TEST_CASE(settings_json_preserves_caller_apartment_ownership) {
    for (const int mode : {-1, static_cast<int>(COINIT_APARTMENTTHREADED), static_cast<int>(COINIT_MULTITHREADED)}) {
        std::exception_ptr failure;
        std::thread worker([&failure, mode] {
            try {
                check_settings_apartment(mode);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}
