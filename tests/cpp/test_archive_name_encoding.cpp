#include "test_util.hpp"

#include "core/archive_name_encoding.hpp"
#include "core/archive_format.hpp"
#include "ar/ar_adapter.hpp"
#include "cpio/cpio_adapter.hpp"
#include "rpm/rpm_adapter.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <array>
#include <string>
#include <string_view>

// Purpose: Preserve valid UTF-8 scalar boundary values without normalization or replacement.
// Inputs: ASCII and shortest encodings at every scalar-length and surrogate boundary.
// Outputs: Requires byte-identical decoded names.
TEST_CASE(archive_name_encoding_valid_utf8_boundaries) {
    constexpr std::array names{"ascii.txt",    "\x7f",         "\xc2\x80",     "\xdf\xbf",         "\xe0\xa0\x80",
                               "\xed\x9f\xbf", "\xee\x80\x80", "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"};
    for (const auto* name : names) {
        REQUIRE_EQ(superzip::decode_archive_name(name, superzip::ArchivePathEncoding::Utf8), name);
    }
}

// Purpose: Reject non-scalar UTF-8 and malformed names instead of guessing another encoding.
// Inputs: Empty, NUL, truncated, overlong, surrogate, continuation, and out-of-range metadata.
// Outputs: Every malformed sequence raises ArchiveError.
TEST_CASE(archive_name_encoding_rejects_malformed_utf8) {
    constexpr std::array<std::string_view, 17> names{"",
                                                     std::string_view("a\0b", 3),
                                                     "\x80",
                                                     "\xbf",
                                                     "\xc0\x80",
                                                     "\xc1\xbf",
                                                     "\xc2",
                                                     "\xc2x",
                                                     "\xe0\x9f\xbf",
                                                     "\xe1\x80",
                                                     "\xed\xa0\x80",
                                                     "\xed\xbf\xbf",
                                                     "\xf0\x8f\xbf\xbf",
                                                     "\xf1\x80\x80",
                                                     "\xf4\x90\x80\x80",
                                                     "\xf5\x80\x80\x80",
                                                     "\xff"};
    for (const auto name : names) {
        bool rejected = false;
        try {
            (void)superzip::decode_archive_name(name, superzip::ArchivePathEncoding::Utf8);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Enforce name length and encoding selection independently of filesystem path validation.
// Inputs: Names at and beyond the byte limit and an invalid enum value.
// Outputs: Accepts the exact bound and rejects excessive metadata or unsupported modes.
TEST_CASE(archive_name_encoding_limits_and_selection) {
    const std::string at_limit(superzip::kMaxArchivePathBytes, 'x');
    REQUIRE_EQ(superzip::decode_archive_name(at_limit, superzip::ArchivePathEncoding::Utf8), at_limit);
    for (const auto encoding : {superzip::ArchivePathEncoding::Utf8, static_cast<superzip::ArchivePathEncoding>(-1)}) {
        bool rejected = false;
        try {
            (void)superzip::decode_archive_name(at_limit + "x", encoding);
        } catch (const superzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
    bool rejected = false;
    try {
        superzip::validate_archive_name_encoding(static_cast<superzip::ArchivePathEncoding>(-1));
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Keep CLI tokens and registry capabilities aligned without affecting declared-encoding formats.
// Inputs: Every registered archive format and both shared encoding choices.
// Outputs: Requires exact keys, five eligible formats, and refusal of unknown tokens.
TEST_CASE(archive_name_encoding_product_choices) {
    for (const auto& choice : superzip::kArchiveNameEncodingChoices) {
        REQUIRE_EQ(superzip::parse_archive_name_encoding(choice.key), choice.encoding);
    }
    std::size_t enabled = 0;
    for (const auto& format : superzip::archive_format_registry()) {
        if (format.supports_name_encoding) {
            ++enabled;
            REQUIRE_TRUE(format.can_extract);
            REQUIRE_TRUE(
                format.format == superzip::ArchiveFormat::Cpio || format.format == superzip::ArchiveFormat::CpioGzip ||
                format.format == superzip::ArchiveFormat::Ar || format.format == superzip::ArchiveFormat::Deb ||
                format.format == superzip::ArchiveFormat::Rpm);
        }
    }
    REQUIRE_EQ(enabled, 5U);
    bool rejected = false;
    try {
        (void)superzip::parse_archive_name_encoding("guess");
    } catch (const superzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Reject invalid encoding options before any archive or destination access.
// Inputs: Every eligible adapter, a missing input path, and an unsupported enum.
// Outputs: Requires the encoding error instead of file-open errors and no destination creation.
TEST_CASE(archive_name_encoding_invalid_option_precedes_io) {
    using Extractor = superzip::OperationStats (*)(const std::filesystem::path&, const std::filesystem::path&, bool,
                                                   const superzip::ProgressCallback&, superzip::ArchivePathEncoding);
    const std::array<Extractor, 4> extractors{superzip::extract_ar, superzip::extract_cpio, superzip::extract_cpio_gzip,
                                              superzip::extract_rpm};
    const auto root = test_temp_dir("invalid-name-encoding-option");
    for (const auto extract : extractors) {
        bool rejected = false;
        try {
            (void)extract(root / "missing", root / "output", false, {}, static_cast<superzip::ArchivePathEncoding>(-1));
        } catch (const superzip::ArchiveError& error) {
            REQUIRE_EQ(std::string(error.what()), "unsupported archive member name encoding");
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_TRUE(!std::filesystem::exists(root / "output"));
    }
}
