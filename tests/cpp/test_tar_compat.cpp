#include "test_util.hpp"

#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "gzip/gzip_adapter.hpp"
#include "tar/tar_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Purpose: Write a fixed-size TAR string field for handcrafted fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is ASCII metadata.
// Outputs: Copies `value` into the field; throws through the test harness if it cannot fit.
void put_test_tar_string(std::array<char, 512>& header, std::size_t offset, std::size_t length,
                         std::string_view value) {
    REQUIRE_TRUE(value.size() <= length);
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Write a simple octal TAR numeric field for small handcrafted fixtures.
// Inputs: `header` is the mutable block, `offset`/`length` select the field, and `value` is the small integer to
// encode. Outputs: Encodes a NUL-terminated octal field.
void put_test_tar_octal(std::array<char, 512>& header, std::size_t offset, std::size_t length, std::uint64_t value) {
    std::string encoded(length - 1U, '0');
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const auto index = encoded.size() - 1U - i;
        encoded[index] = static_cast<char>('0' + (value & 7U));
        value >>= 3U;
    }
    REQUIRE_TRUE(value == 0);
    std::copy(encoded.begin(), encoded.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
    header[offset + length - 1U] = '\0';
}

// Purpose: Create a small TAR header for parser-security tests.
// Inputs: `path` is the archive entry path, `typeflag` is the TAR entry kind, and `size` is payload length.
// Outputs: Returns a checksummed USTAR header block.
std::array<char, 512> make_test_tar_header(std::string_view path, char typeflag, std::uint64_t size) {
    std::array<char, 512> header{};
    put_test_tar_string(header, 0, 100, path);
    put_test_tar_octal(header, 100, 8, typeflag == '5' ? 0755 : 0644);
    put_test_tar_octal(header, 108, 8, 0);
    put_test_tar_octal(header, 116, 8, 0);
    put_test_tar_octal(header, 124, 12, size);
    put_test_tar_octal(header, 136, 12, 0);
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    header[156] = typeflag;
    put_test_tar_string(header, 257, 6, "ustar");
    put_test_tar_string(header, 263, 2, "00");
    std::uint32_t checksum = 0;
    for (const auto ch : header) {
        checksum += static_cast<unsigned char>(ch);
    }
    put_test_tar_octal(header, 148, 7, checksum);
    header[155] = ' ';
    return header;
}

// Purpose: Write a one-entry TAR archive fixture.
// Inputs: `archive` is the output TAR path, `path` is the entry name, `typeflag` is the TAR type, and `payload` is file
// or link payload data. Outputs: Creates a complete TAR stream with end markers.
void write_one_entry_tar(const std::filesystem::path& archive, std::string_view path, char typeflag,
                         std::string_view payload) {
    std::ofstream output(archive, std::ios::binary);
    const auto header = make_test_tar_header(path, typeflag, typeflag == '5' ? 0 : payload.size());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (typeflag != '5') {
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        const auto padding = (512U - (payload.size() % 512U)) % 512U;
        std::array<char, 512> zero{};
        output.write(zero.data(), static_cast<std::streamsize>(padding));
    }
    std::array<char, 512> zero{};
    output.write(zero.data(), static_cast<std::streamsize>(zero.size()));
    output.write(zero.data(), static_cast<std::streamsize>(zero.size()));
}

}  // namespace

namespace {

// Purpose: Serialize metadata and file records with independent TAR block padding.
// Inputs: An owned output stream, fixture header and bounded payload. Outputs: Complete padded record.
void append_tar_fixture_record(std::ostream& output, const std::array<char, 512>& header,
                               std::string_view payload = {}) {
    output.write(header.data(), header.size());
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    const std::array<char, 512> zero{};
    output.write(zero.data(), static_cast<std::streamsize>((512U - payload.size() % 512U) % 512U));
    REQUIRE_TRUE(output.good());
}

// Purpose: Test rejection before publication while retaining an unrelated existing destination file.
// Inputs: One malformed TAR, its owned destination and optional Gzip transport.
// Outputs: Product error with no new paths or changed bytes.
void require_tar_fixture_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                  bool gzip = false) {
    std::filesystem::create_directories(destination);
    const auto sentinel = destination / "existing.txt";
    {
        std::ofstream output(sentinel);
        output << "retain";
    }
    bool rejected = false;
    try {
        if (gzip) {
            (void)flitzzip::extract_tar_gzip(archive, destination, false);
        } else {
            (void)flitzzip::extract_tar(archive, destination, false);
        }
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(destination)) {
        REQUIRE_EQ(entry.path(), sentinel);
        ++entries;
    }
    REQUIRE_EQ(entries, 1U);
    std::ifstream input(sentinel);
    std::string content;
    input >> content;
    REQUIRE_EQ(content, "retain");
}

// Purpose: Recalculate only a fixture's checksum after independently mutating a wire field.
// Inputs: A complete mutable 512-byte header. Outputs: Valid checksum for the intentionally malformed metadata.
void checksum_tar_fixture(std::array<char, 512>& header) {
    std::fill(header.begin() + 148, header.begin() + 156, ' ');
    std::uint32_t sum = 0;
    for (const unsigned char value : header) {
        sum += value;
    }
    put_test_tar_octal(header, 148, 7, sum);
    header[155] = ' ';
}

// Purpose: Frame independent PAX test data without calling the product writer.
// Inputs: Fixture keyword and value bytes. Outputs: A decimal-length record including its newline.
std::string test_pax_record(std::string_view key, std::string_view value) {
    const auto body = std::string(key) + "=" + std::string(value) + "\n";
    for (auto length = body.size() + 2U;; ++length) {
        const auto prefix = std::to_string(length) + " ";
        if (prefix.size() + body.size() == length) {
            return prefix + body;
        }
    }
}

// Purpose: Check exact output from the seekable reader and both passes of the compressed reader.
// Inputs: Fixture archive, isolated output root and independently expected path/byte pairs.
// Outputs: Requires identical contents, file count and size telemetry from both production entry points.
void require_tar_fixture_contents(const std::filesystem::path& archive, const std::filesystem::path& root,
                                  const std::vector<std::pair<std::filesystem::path, std::string>>& expected) {
    const auto compressed = root / "input.tar.gz";
    (void)flitzzip::compress_gzip_file(archive, compressed);
    for (const bool gzip : {false, true}) {
        const auto destination = root / (gzip ? "filtered" : "plain");
        const auto stats = gzip ? flitzzip::extract_tar_gzip(compressed, destination, false)
                                : flitzzip::extract_tar(archive, destination, false);
        std::uint64_t bytes = 0;
        for (const auto& [name, content] : expected) {
            std::ifstream input(destination / name, std::ios::binary);
            REQUIRE_TRUE(input.is_open());
            REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), content);
            bytes += content.size();
        }
        REQUIRE_EQ(stats.entries, expected.size());
        REQUIRE_EQ(stats.output_bytes, bytes);
        REQUIRE_EQ(static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(destination),
                                                          std::filesystem::directory_iterator{})),
                   expected.size());
    }
}

// Purpose: Prove invalid metadata cannot publish files through either TAR reader.
// Inputs: An owned fixture and output root. Outputs: Both parsers fail while preserving the destination sentinel.
void require_tar_fixture_rejected_by_both(const std::filesystem::path& archive, const std::filesystem::path& root) {
    const auto compressed = root / "input.tar.gz";
    (void)flitzzip::compress_gzip_file(archive, compressed);
    require_tar_fixture_rejected(archive, root / "plain");
    require_tar_fixture_rejected(compressed, root / "filtered", true);
}

}  // namespace

// Purpose: Preserve the POSIX directory layout whose complete path occupies the USTAR prefix field.
// Inputs: Empty name fields, short/full-width prefixes, plain/Gzip readers and a following ordinary file.
// Outputs: Both readers create the exact directory and sibling file with consistent entry and byte counts.
TEST_CASE(tar_prefix_only_directories_preserve_paths_and_following_entries) {
    const auto root = test_temp_dir("tar-prefix-only-directory");
    for (const std::size_t length : {1U, 101U, 155U}) {
        const auto fixture = root / std::to_string(length);
        std::filesystem::create_directories(fixture);
        const std::string name(length, 'd');
        auto directory = make_test_tar_header("", '5', 0U);
        put_test_tar_string(directory, 345, 155, name);
        checksum_tar_fixture(directory);
        const auto archive = fixture / "input.tar";
        {
            std::ofstream output(archive, std::ios::binary);
            append_tar_fixture_record(output, directory);
            append_tar_fixture_record(output, make_test_tar_header("sibling.txt", '0', 3U), "abc");
        }
        const auto compressed = fixture / "input.tar.gz";
        (void)flitzzip::compress_gzip_file(archive, compressed);
        for (const bool gzip : {false, true}) {
            const auto destination = fixture / (gzip ? "filtered" : "plain");
            const auto stats = gzip ? flitzzip::extract_tar_gzip(compressed, destination, false)
                                    : flitzzip::extract_tar(archive, destination, false);
            REQUIRE_EQ(stats.entries, 2U);
            REQUIRE_EQ(stats.output_bytes, 3U);
            REQUIRE_TRUE(std::filesystem::is_directory(destination / name));
            REQUIRE_TRUE(std::filesystem::is_empty(destination / name));
            std::ifstream sibling(destination / "sibling.txt", std::ios::binary);
            REQUIRE_TRUE(sibling.is_open());
            REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(sibling), {}), "abc");
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep directory-only prefix support behind the existing filename and extraction safety boundary.
// Inputs: A regular file with no name, an unnamed directory and unsafe prefix-only directory paths.
// Outputs: Both readers reject every fixture before publication and preserve an unrelated destination file.
TEST_CASE(tar_prefix_only_directory_admission_rejects_unsafe_or_absent_paths) {
    const auto root = test_temp_dir("tar-prefix-only-rejection");
    std::size_t ordinal = 0;
    for (const auto& [type, prefix] : std::array<std::pair<char, std::string_view>, 6>{{{'0', "directory"},
                                                                                        {'5', ""},
                                                                                        {'5', "../escape"},
                                                                                        {'5', "/absolute"},
                                                                                        {'5', "C:/absolute"},
                                                                                        {'5', "trailing "}}}) {
        const auto fixture = root / std::to_string(ordinal++);
        std::filesystem::create_directories(fixture);
        const auto archive = fixture / "input.tar";
        auto header = make_test_tar_header("", type, 0U);
        put_test_tar_string(header, 345, 155, prefix);
        checksum_tar_fixture(header);
        {
            std::ofstream output(archive, std::ios::binary);
            append_tar_fixture_record(output, header);
        }
        require_tar_fixture_rejected_by_both(archive, fixture);
        REQUIRE_TRUE(!std::filesystem::exists(fixture / "escape"));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent silent truncation and wrong header alignment when PAX overrides the USTAR size.
// Inputs: Empty, short and block-crossing files with deliberately conflicting or invalid old size fields.
// Outputs: Plain and compressed TAR readers restore exact bytes and leave the next entry unaffected.
TEST_CASE(tar_pax_sizes_override_headers_without_leaking) {
    const auto root = test_temp_dir("tar-pax-effective-size");
    for (const auto size : {0U, 3U, 511U, 512U, 513U}) {
        const auto work = root / std::to_string(size);
        std::filesystem::create_directories(work);
        const auto archive = work / "input.tar";
        const auto metadata = test_pax_record("size", std::to_string(size));
        const std::string payload(size, 'a');
        {
            std::ofstream output(archive, std::ios::binary);
            append_tar_fixture_record(output, make_test_tar_header("PaxHeader", 'x', metadata.size()), metadata);
            auto header = make_test_tar_header("payload", '0', 1U);
            if (size == 512U) {
                std::fill(header.begin() + 124, header.begin() + 136, '9');
                checksum_tar_fixture(header);
            }
            append_tar_fixture_record(output, header, payload);
            append_tar_fixture_record(output, make_test_tar_header("sibling", '0', 2U), "ok");
        }
        require_tar_fixture_contents(archive, work, {{"payload", payload}, {"sibling", "ok"}});
    }
    std::filesystem::remove_all(root);
}

// Purpose: Apply global sizes until changed or deleted while keeping per-file overrides local.
// Inputs: A global size, local zero/nonzero sizes, another global value and a global deletion.
// Outputs: Both readers agree on every file's independent byte oracle and subsequent header position.
TEST_CASE(tar_pax_global_sizes_follow_scope_and_precedence) {
    const auto root = test_temp_dir("tar-pax-global-sizes");
    const auto archive = root / "input.tar";
    const std::array<std::string_view, 5> records{"9 size=3\n", "9 size=0\n", "9 size=2\n", "9 size=1\n", "8 size=\n"};
    {
        std::ofstream output(archive, std::ios::binary);
        append_tar_fixture_record(output, make_test_tar_header("Global", 'g', records[0].size()), records[0]);
        append_tar_fixture_record(output, make_test_tar_header("first", '0', 1U), "abc");
        append_tar_fixture_record(output, make_test_tar_header("Local", 'x', records[1].size()), records[1]);
        append_tar_fixture_record(output, make_test_tar_header("empty", '0', 1U));
        append_tar_fixture_record(output, make_test_tar_header("Local", 'x', records[2].size()), records[2]);
        append_tar_fixture_record(output, make_test_tar_header("short", '0', 1U), "xy");
        append_tar_fixture_record(output, make_test_tar_header("inherited", '0', 1U), "def");
        append_tar_fixture_record(output, make_test_tar_header("Global", 'g', records[3].size()), records[3]);
        append_tar_fixture_record(output, make_test_tar_header("changed", '0', 0U), "z");
        append_tar_fixture_record(output, make_test_tar_header("Global", 'g', records[4].size()), records[4]);
        append_tar_fixture_record(output, make_test_tar_header("original", '0', 2U), "ok");
    }
    require_tar_fixture_contents(
        archive, root,
        {{"first", "abc"}, {"empty", ""}, {"short", "xy"}, {"inherited", "def"}, {"changed", "z"}, {"original", "ok"}});
    std::filesystem::remove_all(root);
}

// Purpose: Preserve PAX path/charset inheritance without leaking overrides into GNU or ordinary names.
// Inputs: Global UTF-8 path, local replacement, global deletion and a GNU path with a local size override.
// Outputs: Both extraction paths restore only the expected names and bytes.
TEST_CASE(tar_pax_global_paths_and_gnu_names_preserve_encoding) {
    const auto root = test_temp_dir("tar-pax-global-paths");
    const auto archive = root / "input.tar";
    const std::string name = "caf\xC3\xA9.txt";
    const auto global = test_pax_record("path", name) + test_pax_record("hdrcharset", "ISO-IR 10646 2000 UTF-8");
    const auto local = test_pax_record("path", "local.txt");
    const auto reset = test_pax_record("path", "") + test_pax_record("hdrcharset", "");
    const std::string long_name("gnu.txt\0", 8);
    {
        std::ofstream output(archive, std::ios::binary);
        append_tar_fixture_record(output, make_test_tar_header("Global", 'g', global.size()), global);
        append_tar_fixture_record(output, make_test_tar_header("ignored", '0', 1U), "a");
        append_tar_fixture_record(output, make_test_tar_header("Local", 'x', local.size()), local);
        append_tar_fixture_record(output, make_test_tar_header("ignored", '0', 1U), "b");
        append_tar_fixture_record(output, make_test_tar_header("Global", 'g', reset.size()), reset);
        append_tar_fixture_record(output, make_test_tar_header("ordinary", '0', 1U), "c");
        append_tar_fixture_record(output, make_test_tar_header("Local", 'x', 9U), "9 size=2\n");
        append_tar_fixture_record(output, make_test_tar_header("LongName", 'L', long_name.size()), long_name);
        append_tar_fixture_record(output, make_test_tar_header("ignored", '0', 0U), "de");
    }
    require_tar_fixture_contents(
        archive, root,
        {{std::filesystem::path(u8"caf\u00e9.txt"), "a"}, {"local.txt", "b"}, {"ordinary", "c"}, {"gnu.txt", "de"}});
    std::filesystem::remove_all(root);
}

// Purpose: Reject invalid PAX sizes and framing before an earlier valid file can be published.
// Inputs: Decimal overflow, signed/trailing/empty values, duplicate fields and incomplete record bodies.
// Outputs: Both reader paths fail atomically at the validation boundary with the existing sentinel intact.
TEST_CASE(tar_pax_invalid_sizes_and_framing_prevent_publication) {
    const auto root = test_temp_dir("tar-pax-invalid-size");
    const auto archive = root / "input.tar";
    std::vector<std::string> records{
        "4 a\n", "5 =x\n", "1 \n", "8 size=\n", "9 size=3\n9 size=2\n", "21 hdrcharset=BINARY\n21 hdrcharset=BINARY\n"};
    for (const auto value : {"-1", "+3", "3x", "3.0", " 3", "3 ", "18446744073709551616", "18446744073709551615"}) {
        records.push_back(test_pax_record("size", value));
    }
    records.push_back(test_pax_record("size", std::string("3\0x", 3)));
    for (const auto& metadata : records) {
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, make_test_tar_header("before", '0', 1U), "a");
            append_tar_fixture_record(output, make_test_tar_header("PaxHeader", 'x', metadata.size()), metadata);
            append_tar_fixture_record(output, make_test_tar_header("payload", '0', 1U), "b");
        }
        require_tar_fixture_rejected_by_both(archive, root);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject unsupported sparse transformations and malformed global metadata instead of extracting corrupt bytes.
// Inputs: GNU, Schily and Solaris sparse keys plus a malformed record in a global header.
// Outputs: No ordinary or transformed entry is published through either reader.
TEST_CASE(tar_pax_sparse_and_invalid_global_metadata_fail_closed) {
    const auto root = test_temp_dir("tar-pax-unsupported-transform");
    const auto archive = root / "input.tar";
    for (const auto type : {'x', 'g'}) {
        std::vector<std::string> records{"malformed", "4 a\n", test_pax_record("path", "../escape")};
        for (const auto key : {"GNU.sparse", "GNU.sparse.map", "GNU.sparse.major", "GNU.sparse.realsize",
                               "SCHILY.realsize", "SUN.holesdata"}) {
            records.push_back(test_pax_record(key, "3"));
        }
        for (const auto& metadata : records) {
            {
                std::ofstream output(archive, std::ios::binary | std::ios::trunc);
                append_tar_fixture_record(output, make_test_tar_header("before", '0', 1U), "a");
                append_tar_fixture_record(output, make_test_tar_header("Extension", type, metadata.size()), metadata);
                append_tar_fixture_record(output, make_test_tar_header("payload", '0', 1U), "b");
            }
            require_tar_fixture_rejected_by_both(archive, root);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent truncated archives ending in one-file extension metadata from reporting successful extraction.
// Inputs: PAX/GNU metadata followed by EOF or an end marker, after an otherwise valid file.
// Outputs: Both reader paths reject before publishing that earlier file.
TEST_CASE(tar_dangling_local_extensions_are_truncation_errors) {
    const auto root = test_temp_dir("tar-dangling-extension");
    const auto archive = root / "input.tar";
    for (const auto type : {'x', 'L'}) {
        for (const bool end_marker : {false, true}) {
            const std::string metadata = type == 'x' ? "9 size=3\n" : std::string("missing\0", 8);
            {
                std::ofstream output(archive, std::ios::binary | std::ios::trunc);
                append_tar_fixture_record(output, make_test_tar_header("before", '0', 1U), "a");
                append_tar_fixture_record(output, make_test_tar_header("Extension", type, metadata.size()), metadata);
                if (end_marker) {
                    const std::array<char, 1024> end{};
                    output.write(end.data(), end.size());
                }
            }
            require_tar_fixture_rejected_by_both(archive, root);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve GNU filename bytes so control characters cannot be trimmed into an accepted path.
// Inputs: Names with literal trailing newlines, NUL padding, embedded NULs and a valid leading space.
// Outputs: Unsafe names reject without output; both readers restore the exact valid name and payload.
TEST_CASE(tar_gnu_long_names_preserve_significant_whitespace) {
    const auto root = test_temp_dir("tar-gnu-name-bytes");
    const auto archive = root / "input.tar";
    const std::array<std::string, 5> invalid{"unsafe\n", std::string("unsafe\n\0", 8), std::string("unsafe\n\0\0", 9),
                                             std::string("safe\0hidden", 11), "unsafe \n"};
    for (const auto& name : invalid) {
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, make_test_tar_header("LongName", 'L', name.size()), name);
            append_tar_fixture_record(output, make_test_tar_header("ignored", '0', 1U), "x");
        }
        require_tar_fixture_rejected_by_both(archive, root);
    }
    const auto valid_root = root / "valid";
    std::filesystem::create_directories(valid_root);
    const std::string name(" leading.txt\0\0", 14);
    {
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        append_tar_fixture_record(output, make_test_tar_header("LongName", 'L', name.size()), name);
        append_tar_fixture_record(output, make_test_tar_header("ignored", '0', 1U), "x");
    }
    require_tar_fixture_contents(archive, valid_root, {{" leading.txt", "x"}});
    std::filesystem::remove_all(root);
}

// Purpose: Reject malformed PAX framing and ambiguous path policy before any destination publication.
// Inputs: Independently authored lengths, terminators, duplicate/empty paths and unsupported character sets.
// Outputs: Every invalid metadata record fails while retaining an existing destination file.
TEST_CASE(tar_pax_malformed_records_do_not_publish) {
    const auto root = test_temp_dir("tar-pax-malformed");
    const auto archive = root / "input.tar";
    const std::array<std::string_view, 8> metadata{"missing-space", "x path=a\n",
                                                   "0 path=a\n",    "99 path=a\n",
                                                   "10 path=a!",    "10 path=a\n10 path=b\n",
                                                   "8 path=\n",     "22 hdrcharset=UNKNOWN\n"};
    for (const auto value : metadata) {
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        append_tar_fixture_record(output, make_test_tar_header("PaxHeader", 'x', value.size()), value);
        append_tar_fixture_record(output, make_test_tar_header("payload", '0', 1U), "x");
        output.close();
        require_tar_fixture_rejected(archive, root / "output");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep one-entry PAX and GNU path overrides from leaking into later entries.
// Inputs: A metadata record, overridden empty file and a second ordinary empty file.
// Outputs: Only the first file takes the override, with exact expected destination entries.
TEST_CASE(tar_path_extensions_apply_once) {
    const auto root = test_temp_dir("tar-path-one-shot");
    for (const char type : {'x', 'L'}) {
        const auto archive = root / "input.tar";
        const std::string metadata = type == 'x' ? "17 path=long.txt\n" : std::string("long.txt\0", 9U);
        std::ofstream output(archive, std::ios::binary | std::ios::trunc);
        append_tar_fixture_record(output, make_test_tar_header("metadata", type, metadata.size()), metadata);
        append_tar_fixture_record(output, make_test_tar_header("ignored.txt", '0', 0U));
        append_tar_fixture_record(output, make_test_tar_header("second.txt", '0', 0U));
        output.close();
        const auto destination = root / std::string(1U, type);
        const auto stats = flitzzip::extract_tar(archive, destination, false);
        REQUIRE_EQ(stats.output_bytes, 0U);
        REQUIRE_TRUE(std::filesystem::is_regular_file(destination / "long.txt"));
        REQUIRE_TRUE(std::filesystem::is_regular_file(destination / "second.txt"));
        REQUIRE_TRUE(!std::filesystem::exists(destination / "ignored.txt"));
    }
    std::filesystem::remove_all(root);
}

// Purpose: Validate base-256 sizes independently of the writer and reject hostile numeric representations.
// Inputs: One-byte positive base-256 size, overflowing/negative encodings, invalid octal and a corrupt checksum.
// Outputs: Valid bytes restore exactly; malformed headers leave the destination untouched.
TEST_CASE(tar_numeric_headers_are_bounded_and_checksummed) {
    const auto root = test_temp_dir("tar-numeric-headers");
    const auto archive = root / "input.tar";
    auto positive = make_test_tar_header("payload", '0', 0U);
    std::fill(positive.begin() + 124, positive.begin() + 136, '\0');
    positive[124] = static_cast<char>(0x80U);
    positive[135] = 1;
    checksum_tar_fixture(positive);
    {
        std::ofstream output(archive, std::ios::binary);
        append_tar_fixture_record(output, positive, "z");
    }
    const auto stats = flitzzip::extract_tar(archive, root / "valid", false);
    REQUIRE_EQ(stats.output_bytes, 1U);
    {
        std::ifstream input(root / "valid" / "payload");
        REQUIRE_EQ(input.get(), 'z');
        REQUIRE_EQ(input.get(), EOF);
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto invalid = positive;
        if (mutation == 0) {
            invalid[124] = static_cast<char>(0xFFU);
        }
        if (mutation == 1) {
            invalid[125] = 1;
        }
        if (mutation == 2) {
            invalid[124] = '8';
        }
        if (mutation == 4 || mutation == 5) {
            std::fill(invalid.begin() + 124, invalid.begin() + 136, '\0');
            invalid[124] = '1';
            invalid[125] = mutation == 4 ? '\0' : ' ';
            invalid[126] = '7';
        }
        checksum_tar_fixture(invalid);
        if (mutation == 3) {
            invalid[0] ^= 1;
        }
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, invalid, "z");
        }
        require_tar_fixture_rejected(archive, root / "rejected");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent unsupported TAR entry types and empty GNU names from creating filesystem objects.
// Inputs: Link, device, FIFO and unsupported type flags, plus empty path-override metadata.
// Outputs: All cases reject before publication and preserve existing files.
TEST_CASE(tar_special_entries_and_empty_long_names_fail_closed) {
    const auto root = test_temp_dir("tar-special-types");
    const auto archive = root / "input.tar";
    for (const char type : {'1', '2', '3', '4', '6', 'K', 'V', 'L'}) {
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, make_test_tar_header("payload", type, 0U));
        }
        require_tar_fixture_rejected(archive, root / "output");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject oversized and incomplete extension payloads before allocating or publishing ordinary entries.
// Inputs: PAX/local/global/GNU metadata with a size above one MiB, or a declared payload longer than supplied bytes.
// Outputs: Plain and filtered TAR reject while preserving an existing destination and publishing no earlier file.
TEST_CASE(tar_metadata_payload_limits_and_truncation_prevent_publication) {
    const auto root = test_temp_dir("tar-metadata-limits");
    const auto archive = root / "input.tar";
    for (const char type : {'x', 'g', 'L'}) {
        for (const bool oversized : {false, true}) {
            {
                std::ofstream output(archive, std::ios::binary | std::ios::trunc);
                append_tar_fixture_record(output, make_test_tar_header("earlier.txt", '0', 5U), "first");
                const auto header = make_test_tar_header("metadata", type, oversized ? 1024U * 1024U + 1U : 20U);
                output.write(header.data(), static_cast<std::streamsize>(header.size()));
                output << "short";
            }
            require_tar_fixture_rejected_by_both(archive, root);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Enforce retained path bounds for both extension syntaxes without constructing excessive file trees.
// Inputs: Independently framed local/global PAX paths and a GNU name one byte beyond the archive path limit.
// Outputs: Both TAR readers reject before publishing a preceding valid entry or changing an existing file.
TEST_CASE(tar_extension_paths_above_resource_limit_prevent_publication) {
    const auto root = test_temp_dir("tar-extension-path-limits");
    const auto archive = root / "input.tar";
    const std::string long_path(flitzzip::kMaxArchivePathBytes + 1U, 'a');
    for (const char type : {'x', 'g', 'L'}) {
        const auto metadata = type == 'L' ? long_path + '\0' : test_pax_record("path", long_path);
        {
            std::ofstream output(archive, std::ios::binary | std::ios::trunc);
            append_tar_fixture_record(output, make_test_tar_header("earlier.txt", '0', 5U), "first");
            append_tar_fixture_record(output, make_test_tar_header("metadata", type, metadata.size()), metadata);
            append_tar_fixture_record(output, make_test_tar_header("payload", '0', 1U), "z");
        }
        require_tar_fixture_rejected_by_both(archive, root);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Check the production checksum's exact wire width and terminators without reusing its formatter.
// Inputs: A created TAR archive with a normal binary file; the checksum oracle sums bytes with a blanked field.
// Outputs: Requires six ASCII octal digits, NUL and space, and an independently reconstructed checksum.
TEST_CASE(tar_writer_checksum_preserves_exact_wire_encoding) {
    const auto root = test_temp_dir("tar-checksum-wire");
    const auto source = root / "payload.bin";
    {
        std::ofstream output(source, std::ios::binary);
        for (unsigned i = 0; i < 256U; ++i) {
            output.put(static_cast<char>(i));
        }
    }
    const auto archive = root / "output.tar";
    (void)flitzzip::compress_tar({source}, archive);
    {
        std::ifstream input(archive, std::ios::binary);
        std::array<char, 512> header{};
        input.read(header.data(), static_cast<std::streamsize>(header.size()));
        REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(header.size()));
        std::uint32_t encoded = 0;
        for (std::size_t i = 148U; i < 154U; ++i) {
            REQUIRE_TRUE(header[i] >= '0' && header[i] <= '7');
            encoded = encoded * 8U + static_cast<unsigned>(header[i] - '0');
        }
        REQUIRE_EQ(header[154], '\0');
        REQUIRE_EQ(header[155], ' ');
        std::uint32_t expected = 0;
        for (std::size_t i = 0; i < header.size(); ++i) {
            expected += i >= 148U && i < 156U ? 32U : static_cast<unsigned char>(header[i]);
        }
        REQUIRE_EQ(encoded, expected);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve declared PAX encodings while leaving unmarked legacy TAR names unchanged.
// Inputs: An independently serialized PAX record with UTF-8 or BINARY names and an unmarked USTAR control.
// Outputs: Requires exact filename restoration for each declared character set.
TEST_CASE(tar_pax_filename_encoding_contract) {
    const auto root = test_temp_dir("tar-pax-encoding");
    const std::string name = "caf\xC3\xA9.txt";
    for (const auto* charset : {"", "BINARY", "ISO-IR 10646 2000 UTF-8", "legacy"}) {
        const auto archive = root / "input.tar";
        const std::string charset_name(charset);
        std::ofstream output(archive, std::ios::binary);
        if (charset_name != "legacy") {
            const std::string metadata = (charset_name.empty()       ? ""
                                          : charset_name == "BINARY" ? "21 hdrcharset=BINARY\n"
                                                                     : "38 hdrcharset=ISO-IR 10646 2000 UTF-8\n") +
                                         std::string("18 path=") + name + "\n";
            const auto header = make_test_tar_header("PaxHeaders/name", 'x', metadata.size());
            output.write(header.data(), header.size());
            output.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
            const std::array<char, 512> zero{};
            output.write(zero.data(), static_cast<std::streamsize>((512U - metadata.size() % 512U) % 512U));
        }
        const auto header = make_test_tar_header(charset_name == "legacy" ? name : "fallback", '0', 0);
        output.write(header.data(), header.size());
        const std::array<char, 1024> footer{};
        output.write(footer.data(), footer.size());
        output.close();
        const auto destination = root / (charset_name.empty() ? "default" : charset_name);
        (void)flitzzip::extract_tar(archive, destination, false);
        const auto expected = charset_name == "BINARY" || charset_name == "legacy"
                                  ? std::filesystem::path(name)
                                  : std::filesystem::path(u8"caf\u00e9.txt");
        REQUIRE_TRUE(std::filesystem::is_regular_file(destination / expected));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE(tar_roundtrip_extracts_files_and_directories) {
    const auto root = test_temp_dir("tar-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from tar\n";
        std::ofstream(input / "root.txt") << "root file\n";
    }
    const auto archive = root / "sample.tar";
    const auto compress_stats = flitzzip::compress_tar({input}, archive);
    REQUIRE_TRUE(compress_stats.entries >= 3);
    REQUIRE_TRUE(compress_stats.output_bytes > compress_stats.input_bytes);

    const auto output = root / "output";
    const auto extract_stats = flitzzip::extract_tar(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "dir" / "hello.txt"));
    REQUIRE_TRUE(std::filesystem::exists(output / "input" / "root.txt"));
}

TEST_CASE(tar_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("tar-traversal");
    const auto archive = root / "bad.tar";
    write_one_entry_tar(archive, "../escape.txt", '0', "bad");
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_tar(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
}

TEST_CASE(tar_extraction_rejects_link_entries) {
    const auto root = test_temp_dir("tar-link");
    const auto archive = root / "link.tar";
    write_one_entry_tar(archive, "link", '2', "target");
    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_tar(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

TEST_CASE(tar_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("tar-overwrite");
    const auto archive = root / "overwrite.tar";
    write_one_entry_tar(archive, "file.txt", '0', "new");
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_tar(archive, output, false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
}

// Purpose: Keep seekable and filtered TAR admission consistent at partial header/payload/padding boundaries.
// Inputs: Independently constructed one-member TAR truncated at each structural edge.
// Outputs: Both readers reject before publication; an unrelated original file remains unchanged.
TEST_CASE(tar_structural_truncations_reject_in_both_readers) {
    const auto root = test_temp_dir("tar-structural-truncations");
    const auto archive = root / "truncated.tar";
    const auto compressed = root / "truncated.tar.gz";
    for (const std::uintmax_t length : {1U, 511U, 512U, 513U, 514U, 515U, 1023U}) {
        write_one_entry_tar(archive, "payload.txt", '0', "abc");
        std::filesystem::resize_file(archive, length);
        (void)flitzzip::compress_gzip_file(archive, compressed);
        require_tar_fixture_rejected(archive, root / "out");
        require_tar_fixture_rejected(compressed, root / "out", true);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Apply a GNU long filename once while consuming global extension metadata identically in both passes.
// Inputs: Independent TAR records containing a global comment, GNU long name, file and sibling file.
// Outputs: Plain and Gzip readers restore both correct paths and bytes without leaking extension names as entries.
TEST_CASE(tar_global_and_long_name_metadata_match_between_passes) {
    const auto root = test_temp_dir("tar-extensions-both-passes");
    const auto archive = root / "extended.tar";
    const auto compressed = root / "extended.tar.gz";
    const std::string global = "16 comment=note\n";
    const std::string long_name("restored.txt\0", 13);
    {
        std::ofstream output(archive, std::ios::binary);
        append_tar_fixture_record(output, make_test_tar_header("GlobalHead", 'g', global.size()), global);
        append_tar_fixture_record(output, make_test_tar_header("LongName", 'L', long_name.size()), long_name);
        append_tar_fixture_record(output, make_test_tar_header("placeholder", '0', 3), "abc");
        append_tar_fixture_record(output, make_test_tar_header("sibling.txt", '0', 3), "xyz");
        const std::array<char, 1024> end{};
        output.write(end.data(), end.size());
        REQUIRE_TRUE(output.good());
    }
    (void)flitzzip::compress_gzip_file(archive, compressed);
    for (const bool gzip : {false, true}) {
        const auto destination = root / (gzip ? "filtered" : "plain");
        const auto stats = gzip ? flitzzip::extract_tar_gzip(compressed, destination, false)
                                : flitzzip::extract_tar(archive, destination, false);
        REQUIRE_EQ(stats.entries, 2U);
        REQUIRE_EQ(stats.output_bytes, 6U);
        for (const auto& [name, expected] :
             std::array<std::pair<const char*, const char*>, 2>{{{"restored.txt", "abc"}, {"sibling.txt", "xyz"}}}) {
            std::ifstream input(destination / name, std::ios::binary);
            REQUIRE_TRUE(input.is_open());
            const std::string bytes(std::istreambuf_iterator<char>(input), {});
            REQUIRE_EQ(bytes, expected);
        }
        REQUIRE_EQ(
            std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}), 2);
    }
    std::filesystem::remove_all(root);
}
