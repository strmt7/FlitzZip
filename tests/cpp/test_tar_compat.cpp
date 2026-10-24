#include "test_util.hpp"

#include "core/result.hpp"
#include "core/resource_limits.hpp"
#include "core/path_text.hpp"
#include "gzip/gzip_adapter.hpp"
#include "tar/tar_adapter.hpp"
#include "tar/tar_reader.hpp"
#include "tar/tar_header_fields.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
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

// Purpose: Preserve the security error and publication state for an existing Unicode extraction target.
// Inputs: Plain or Gzip transport, an independent one-file TAR and an owned supplementary-Unicode destination.
// Outputs: Both repeated refusals preserve the old bytes; explicit overwrite succeeds with no staging debris.
void require_tar_unicode_overwrite_contract(bool gzip) {
    const auto root = test_temp_dir(gzip ? "tar-unicode-gzip-overwrite" : "tar-unicode-overwrite");
    const auto plain = root / "input.tar";
    write_one_entry_tar(plain, "file.txt", '0', "replacement");
    const auto archive = gzip ? root / "input.tar.gz" : plain;
    if (gzip) {
        (void)flitzzip::compress_gzip_file(plain, archive);
    }
    const auto destination = root / std::filesystem::path(L"\u65e5\u672c-\U0001F4C1");
    std::filesystem::create_directory(destination);
    const auto target = destination / "file.txt";
    {
        std::ofstream output(target, std::ios::binary);
        output << "original";
        REQUIRE_TRUE(output.good());
    }
    const auto extract = [&](bool overwrite) {
        return gzip ? flitzzip::extract_tar_gzip(archive, destination, overwrite)
                    : flitzzip::extract_tar(archive, destination, overwrite);
    };
    const auto expected =
        "refusing to overwrite existing TAR extraction target: " + flitzzip::path_diagnostic_utf8(target);
    const auto expected_regular = "refusing to overwrite existing TAR extraction target: " +
                                  flitzzip::path_diagnostic_utf8(flitzzip::windows_regular_path(target));
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        try {
            (void)extract(false);
        } catch (const flitzzip::SecurityError& error) {
            // Both native Windows aliases identify this exact file; diagnostic policy does not fix the prefix.
            if (std::string(error.what()) != expected && std::string(error.what()) != expected_regular) {
                throw std::runtime_error("unexpected TAR refusal text; expected [" + expected + "]; actual [" +
                                         error.what() + "]");
            }
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        std::ifstream input(target, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "original");
        REQUIRE_EQ(static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(destination),
                                                          std::filesystem::directory_iterator{})),
                   1U);
    }
    const auto stats = extract(true);
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(stats.output_bytes, 11U);
    std::ifstream input(target, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), "replacement");
    REQUIRE_EQ(static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(destination),
                                                      std::filesystem::directory_iterator{})),
               1U);
}

}  // namespace

// Purpose: Check Unicode refusals and recovery in the seekable TAR reader.
// Inputs: Independent bounded filesystem fixture. Outputs: Exact security diagnostic and byte-preserving refusal.
TEST_CASE(tar_unicode_overwrite_seekable_preserves_error_and_recovery) {
    require_tar_unicode_overwrite_contract(false);
}

// Purpose: Check Unicode refusals and recovery in both passes of the Gzip-filtered TAR reader.
// Inputs: Independent bounded filesystem fixture. Outputs: Exact security diagnostic and byte-preserving refusal.
TEST_CASE(tar_unicode_overwrite_filtered_preserves_error_and_recovery) {
    require_tar_unicode_overwrite_contract(true);
}

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

// Purpose: Preserve normal file-style seeking while injecting an unavailable-byte boundary in RAM.
// Inputs: Complete fixture bytes, the readable prefix extent and a choice of physical EOF or I/O failure.
// Outputs: Supplies the prefix normally, records the attempted boundary read and optionally throws on that read.
class TarBoundaryInput final : public std::stringbuf {
  public:
    // Purpose: Install a genuinely short readable stream without changing its independently declared archive extent.
    // Inputs: Fixture bytes, an in-bounds readable boundary and the failure kind.
    // Outputs: Owns a position-aware prefix or throws if the test geometry is invalid.
    TarBoundaryInput(const std::string& bytes, std::size_t boundary, bool io_error)
        : std::stringbuf(bytes.substr(0, boundary), std::ios::in | std::ios::binary), io_error_(io_error) {
        REQUIRE_TRUE(boundary <= bytes.size());
    }

    // Purpose: Prove a rejection reached the injected read rather than failing an unrelated operation.
    // Inputs: The completed parser attempt. Outputs: True after a read requests unavailable bytes.
    bool boundary_read() const noexcept {
        return boundary_read_;
    }

  protected:
    // Purpose: Fail exactly when a real stream read crosses the readable prefix.
    // Inputs: Writable caller storage and the requested byte count.
    // Outputs: Returns the actual prefix count or throws an I/O error, as basic_istream expects.
    std::streamsize xsgetn(char* destination, std::streamsize count) override {
        const auto obtained = std::stringbuf::xsgetn(destination, count);
        if (obtained < count) {
            boundary_read_ = true;
            if (io_error_) {
                throw std::ios_base::failure("controlled TAR input read failure");
            }
        }
        return obtained;
    }

  private:
    bool io_error_;
    bool boundary_read_ = false;
};

// Purpose: Independently serialize two file records with no optional end markers.
// Inputs: None; payload and padding are bounded in-memory fixture data.
// Outputs: Complete TAR bytes; the second header starts at byte 1024.
std::string tar_reader_boundary_fixture() {
    std::ostringstream output(std::ios::out | std::ios::binary);
    append_tar_fixture_record(output, make_test_tar_header("before.txt", '0', 3), "abc");
    REQUIRE_EQ(output.tellp(), std::ostream::pos_type(1024));
    append_tar_fixture_record(output, make_test_tar_header("after.txt", '0', 5), "defgh");
    return output.str();
}

// Purpose: Reject every incomplete next-header position after a valid entry without filesystem writes.
// Inputs: A seekable or filtered reader and either physical EOF or a non-EOF input failure.
// Outputs: Fails if the actual production scanner admits any incomplete publication plan.
void require_tar_reader_boundary_failures(bool seekable, bool io_error) {
    const auto bytes = tar_reader_boundary_fixture();
    for (std::size_t unavailable = 1024U; unavailable < 1536U; ++unavailable) {
        TarBoundaryInput buffer(bytes, unavailable, io_error);
        std::istream input(&buffer);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_tar_stream(
                input, seekable, seekable ? std::optional<std::uint64_t>(bytes.size()) : std::nullopt);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(buffer.boundary_read());
        REQUIRE_TRUE(rejected);
    }
}

// Purpose: Require a real TAR reader attempt to reject without changing the selected destination.
// Inputs: An owned destination and a synchronous zero-argument extraction callback, borrowed only during this call.
// Outputs: Product rejection, no new paths or changed sentinel bytes; test failure otherwise.
template <typename Attempt>
void require_tar_rejected_without_publication(const std::filesystem::path& destination, Attempt&& attempt) {
    std::filesystem::create_directories(destination);
    const auto sentinel = destination / "existing.txt";
    {
        std::ofstream output(sentinel);
        output << "retain";
    }
    bool rejected = false;
    try {
        attempt();
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

// Purpose: Test rejection before publication through either public TAR extraction adapter.
// Inputs: One malformed TAR, its owned destination and optional Gzip transport.
// Outputs: Product error with no new paths or changed bytes.
void require_tar_fixture_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                  bool gzip = false) {
    require_tar_rejected_without_publication(destination, [&] {
        if (gzip) {
            (void)flitzzip::extract_tar_gzip(archive, destination, false);
        } else {
            (void)flitzzip::extract_tar(archive, destination, false);
        }
    });
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

namespace {

// Purpose: Inspect the actual writer at POSIX name/prefix boundaries without using the product parser as an oracle.
// Inputs: One native relative filename, its independently expected PAX requirement and an isolated fixture ordinal.
// Outputs: Checks raw pathname records, exact payload/footer/stats and extracted bytes; removes the owned fixture.
void require_tar_writer_path_boundary(const std::filesystem::path& relative, bool requires_pax, unsigned ordinal) {
    const auto root = test_temp_dir("tar-writer-path-" + std::to_string(ordinal));
    const auto tree = root / "tree";
    auto native_relative = relative;
    native_relative.make_preferred();
    const auto source = tree / native_relative;
    std::filesystem::create_directories(source.parent_path());
    const std::string content = "filename boundary payload";
    {
        std::ofstream output(source, std::ios::binary);
        output << content;
        output.close();
        REQUIRE_TRUE(output.good());
    }
    const auto archive = root / "output.tar";
    const auto stats = flitzzip::compress_tar({tree}, archive);
    const auto utf8 = relative.generic_u8string();
    const std::string expected = "tree/" + std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    bool file_seen = false;
    std::uint64_t entries = 0;
    {
        std::ifstream input(archive, std::ios::binary);
        std::optional<std::string> extended_path;
        for (;;) {
            std::array<char, 512> header{};
            input.read(header.data(), header.size());
            REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(header.size()));
            if (std::ranges::all_of(header, [](char value) { return value == 0; })) {
                input.read(header.data(), header.size());
                REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(header.size()));
                REQUIRE_TRUE(std::ranges::all_of(header, [](char value) { return value == 0; }));
                REQUIRE_EQ(input.peek(), std::char_traits<char>::eof());
                REQUIRE_TRUE(!extended_path);
                break;
            }
            std::size_t size = 0;
            for (std::size_t i = 124; i < 136 && header[i] != '\0'; ++i) {
                REQUIRE_TRUE(header[i] >= '0' && header[i] <= '7');
                size = size * 8U + static_cast<unsigned>(header[i] - '0');
            }
            REQUIRE_TRUE(size < 512U);
            std::array<char, 512> payload{};
            if (size) {
                input.read(payload.data(), payload.size());
                REQUIRE_EQ(input.gcount(), static_cast<std::streamsize>(payload.size()));
                REQUIRE_TRUE(std::all_of(payload.begin() + static_cast<std::ptrdiff_t>(size), payload.end(),
                                         [](char value) { return value == 0; }));
            }
            if (header[156] == 'x') {
                REQUIRE_TRUE(!extended_path);
                const std::string_view record(payload.data(), size);
                const auto separator = record.find(' ');
                REQUIRE_TRUE(separator != std::string_view::npos);
                std::size_t declared_size = 0;
                const auto parsed = std::from_chars(record.data(), record.data() + separator, declared_size);
                REQUIRE_TRUE(parsed.ec == std::errc{} && parsed.ptr == record.data() + separator);
                REQUIRE_EQ(declared_size, size);
                REQUIRE_TRUE(record.substr(separator + 1U).starts_with("path=") && record.back() == '\n');
                extended_path = std::string(record.substr(separator + 6U, size - separator - 7U));
                continue;
            }
            REQUIRE_TRUE(header[156] == '0' || header[156] == '5');
            ++entries;
            if (header[156] == '0') {
                REQUIRE_TRUE(!file_seen);
                file_seen = true;
                REQUIRE_EQ(extended_path.has_value(), requires_pax);
                const auto name_end = std::find(header.begin(), header.begin() + 100, '\0');
                const auto prefix_end = std::find(header.begin() + 345, header.begin() + 500, '\0');
                const std::string prefix(header.begin() + 345, prefix_end);
                const auto wire_path = prefix.empty() ? std::string(header.begin(), name_end)
                                                      : prefix + '/' + std::string(header.begin(), name_end);
                REQUIRE_EQ(extended_path.value_or(wire_path), expected);
                REQUIRE_EQ(std::string(payload.data(), size), content);
            }
            extended_path.reset();
        }
    }
    REQUIRE_TRUE(file_seen);
    REQUIRE_EQ(stats.entries, entries);
    REQUIRE_EQ(stats.input_bytes, content.size());
    REQUIRE_EQ(stats.output_bytes, std::filesystem::file_size(archive));
    const auto destination = root / "extracted";
    const auto restored = flitzzip::extract_tar(archive, destination, false);
    REQUIRE_EQ(restored.entries, entries);
    REQUIRE_EQ(restored.output_bytes, content.size());
    {
        std::ifstream input(destination / "tree" / native_relative, std::ios::binary);
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(input), {}), content);
    }
    std::filesystem::remove_all(root);
}

}  // namespace

// Purpose: Prevent byte-width truncation, unnecessary PAX fallback and leaked directory overrides in writer output.
// Inputs: Exact 100-byte names, 155-byte prefixes, earlier split points, over-limit and Unicode filenames.
// Outputs: Every fixture retains its independently expected representation and exact payload after extraction.
TEST_CASE(tar_writer_preserves_ustar_and_pax_filename_boundaries) {
    const std::vector<std::pair<std::filesystem::path, bool>> cases{
        {std::string(95, 'a'), false},
        {std::string(96, 'a'), false},
        {std::string(100, 'a'), false},
        {std::string(101, 'a'), true},
        {std::string(150, 'p') + '/' + std::string(100, 'a'), false},
        {std::string(151, 'p') + '/' + std::string(100, 'a'), true},
        {std::string(150, 'p') + '/' + std::string(60, 'q') + '/' + std::string(30, 'r'), false},
        {std::filesystem::path(u8"caf\u00e9.txt"), true}};
    unsigned ordinal = 0;
    for (const auto& [relative, requires_pax] : cases) {
        require_tar_writer_path_boundary(relative, requires_pax, ordinal++);
    }
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

// Purpose: Prevent physical EOF from shortening a known TAR publication plan after a valid prefix.
// Inputs: Every byte position in a second header, with the complete extent independently retained.
// Outputs: All incomplete prefixes are rejected by the filesystem scanner's actual production implementation.
TEST_CASE(tar_scan_rejects_premature_next_header_eof) {
    require_tar_reader_boundary_failures(true, false);
}

// Purpose: Reject non-EOF header read failures in both filesystem and filtered TAR validation.
// Inputs: Controlled I/O failure at each next-header position after a complete first entry.
// Outputs: Neither reader returns a shortened successful plan.
TEST_CASE(tar_scan_rejects_next_header_io_failure) {
    require_tar_reader_boundary_failures(true, true);
    require_tar_reader_boundary_failures(false, true);
}

// Purpose: Preserve supported clean termination independently of fault rejection.
// Inputs: Two complete entries followed by no marker, one zero block or the canonical pair.
// Outputs: Both readers retain every entry, exact payload extents and the complete total.
TEST_CASE(tar_scan_preserves_clean_header_boundary_endings) {
    for (const auto zero_blocks : {0U, 1U, 2U}) {
        auto bytes = tar_reader_boundary_fixture();
        bytes.append(zero_blocks * 512U, '\0');
        for (const bool seekable : {false, true}) {
            std::istringstream input(bytes, std::ios::in | std::ios::binary);
            const auto plan = flitzzip::detail::scan_tar_stream(
                input, seekable, seekable ? std::optional<std::uint64_t>(bytes.size()) : std::nullopt);
            REQUIRE_EQ(plan.entries.size(), 2U);
            REQUIRE_EQ(plan.total_file_bytes, 8U);
            REQUIRE_EQ(plan.entries[0].path, "before.txt");
            REQUIRE_EQ(plan.entries[1].path, "after.txt");
            REQUIRE_EQ(plan.entries[0].size, 3U);
            REQUIRE_EQ(plan.entries[1].size, 5U);
            REQUIRE_EQ(plan.entries[1].payload_offset, seekable ? 1536U : 0U);
        }
    }
}

// Purpose: Reject stale stream state before it can be mistaken for an empty successful archive.
// Inputs: Independently valid TAR bytes with each preexisting failure-state bit.
// Outputs: No unreadable stream produces a publication plan in either reader mode.
TEST_CASE(tar_scan_rejects_preexisting_stream_failure) {
    const auto bytes = tar_reader_boundary_fixture();
    for (const auto state : {std::ios::badbit, std::ios::failbit, std::ios::eofbit}) {
        for (const bool seekable : {false, true}) {
            std::istringstream input(bytes, std::ios::in | std::ios::binary);
            input.setstate(state);
            bool rejected = false;
            try {
                (void)flitzzip::detail::scan_tar_stream(
                    input, seekable, seekable ? std::optional<std::uint64_t>(bytes.size()) : std::nullopt);
            } catch (const flitzzip::ArchiveError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
        }
    }
}

// Purpose: Require independently established bounds even when a readable file has no ordinary entries.
// Inputs: An empty stream with missing bounds and a complete zero header with understated bounds.
// Outputs: Seekable validation rejects absent or insufficient extents instead of admitting an empty plan.
TEST_CASE(tar_scan_requires_truthful_seekable_bounds) {
    for (const auto extent :
         {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0U}, std::optional<std::uint64_t>{511U}}) {
        std::istringstream input(extent ? std::string(512U, '\0') : std::string{}, std::ios::in | std::ios::binary);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_tar_stream(input, true, extent);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
    }
}

namespace {

enum class TarSeekFault { HeaderPosition, PositionPastExtent, PayloadPosition, PayloadSkip };

// Purpose: Fail a real seekable-stream operation without changing the valid TAR bytes or exhausting host resources.
// Inputs: Independently framed bytes and the position query or payload seek to reject once.
// Outputs: Owns normal readable bytes, records one actual refusal and then permits valid stream reuse.
class TarSeekFaultInput final : public std::stringbuf {
  public:
    // Purpose: Preserve valid read/seek behavior until the selected boundary is actually requested.
    // Inputs: Complete fixture bytes and one controlled seek failure mode. Outputs: Owns the unchanged fixture.
    TarSeekFaultInput(const std::string& bytes, TarSeekFault fault)
        : std::stringbuf(bytes, std::ios::in | std::ios::binary), fault_(fault), extent_(bytes.size()) {}

    // Purpose: Prove the parser reached the injected external failure rather than an unrelated rejection.
    // Inputs: None. Outputs: True only after the chosen query or seek was intercepted.
    bool fault_observed() const noexcept {
        return fault_observed_;
    }

  protected:
    // Purpose: Intercept only the selected query/seek, forwarding all other stream operations unchanged.
    // Inputs: Standard stream offset, origin and access mode. Outputs: Valid position or one controlled failure.
    pos_type seekoff(off_type offset, std::ios_base::seekdir origin, std::ios_base::openmode mode) override {
        const auto position = std::stringbuf::seekoff(0, std::ios::cur, std::ios::in);
        if (!fault_observed_ && origin == std::ios::cur) {
            if (offset == 0 && position == pos_type(0) &&
                (fault_ == TarSeekFault::HeaderPosition || fault_ == TarSeekFault::PositionPastExtent)) {
                fault_observed_ = true;
                return fault_ == TarSeekFault::HeaderPosition ? pos_type(off_type(-1)) : pos_type(extent_ + 1U);
            }
            if (position == pos_type(512) && ((offset == 0 && fault_ == TarSeekFault::PayloadPosition) ||
                                              (offset == 512 && fault_ == TarSeekFault::PayloadSkip))) {
                fault_observed_ = true;
                return pos_type(off_type(-1));
            }
        }
        return std::stringbuf::seekoff(offset, origin, mode);
    }

  private:
    TarSeekFault fault_;
    std::size_t extent_;
    bool fault_observed_ = false;
};

}  // namespace

// Purpose: Refuse untrustworthy stream positions before returning an apparently validated archive plan.
// Inputs: The same two-file TAR with controlled header-position, extent, payload-position and payload-seek failures.
// Outputs: Exact boundary errors, unchanged source bytes and successful reuse of the same stream after each fault.
TEST_CASE(tar_seek_failures_reject_incomplete_plans_and_allow_same_stream_recovery) {
    const auto bytes = tar_reader_boundary_fixture();
    for (const auto fault : {TarSeekFault::HeaderPosition, TarSeekFault::PositionPastExtent,
                             TarSeekFault::PayloadPosition, TarSeekFault::PayloadSkip}) {
        TarSeekFaultInput buffer(bytes, fault);
        std::istream input(&buffer);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_tar_stream(input, true, bytes.size());
        } catch (const flitzzip::ArchiveError& error) {
            const std::string_view expected = fault == TarSeekFault::PayloadPosition
                                                  ? "seekable TAR payload has no valid offset or archive-size bound"
                                              : fault == TarSeekFault::PayloadSkip
                                                  ? "TAR payload is truncated"
                                                  : "TAR header has no valid archive-bounded offset";
            REQUIRE_EQ(std::string_view(error.what()), expected);
            rejected = true;
        }
        REQUIRE_TRUE(buffer.fault_observed());
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(buffer.str(), bytes);
        input.clear();
        input.seekg(0);
        const auto admitted = flitzzip::detail::scan_tar_stream(input, true, bytes.size());
        REQUIRE_EQ(admitted.entries.size(), 2U);
        REQUIRE_EQ(admitted.entries[0].path, std::string("before.txt"));
        REQUIRE_EQ(admitted.entries[0].payload_offset, 512U);
        REQUIRE_EQ(admitted.entries[1].path, std::string("after.txt"));
        REQUIRE_EQ(admitted.entries[1].payload_offset, 1536U);
        REQUIRE_EQ(admitted.total_file_bytes, 8U);
    }
}

// Purpose: Refuse second-pass metadata changes before the first file is published.
// Inputs: An admitted two-file plan, independently replaced path/kind/size/encoding and forbidden entry types.
// Outputs: Every changed stream rejects while preserving the original destination and leaving no staging files.
TEST_CASE(tar_stream_extract_rejects_changed_metadata_before_publication) {
    const auto root = test_temp_dir("tar-second-pass-metadata");
    const auto original = tar_reader_boundary_fixture();
    std::istringstream scan_input(original, std::ios::in | std::ios::binary);
    const auto plan = flitzzip::detail::scan_tar_stream(scan_input, false);
    std::vector<std::string> changed;
    for (const auto& header : {make_test_tar_header("changed.txt", '0', 3), make_test_tar_header("before.txt", '5', 3),
                               make_test_tar_header("before.txt", '0', 2), make_test_tar_header("before.txt", '1', 3),
                               make_test_tar_header("before.txt", '2', 3), make_test_tar_header("before.txt", '3', 3),
                               make_test_tar_header("before.txt", '4', 3), make_test_tar_header("before.txt", '6', 3),
                               make_test_tar_header("before.txt", '?', 3)}) {
        auto bytes = original;
        std::copy(header.begin(), header.end(), bytes.begin());
        changed.push_back(std::move(bytes));
    }
    std::ostringstream metadata(std::ios::out | std::ios::binary);
    const auto pax = test_pax_record("path", "before.txt") + test_pax_record("hdrcharset", "ISO-IR 10646 2000 UTF-8");
    append_tar_fixture_record(metadata, make_test_tar_header("PaxHeaders/change", 'x', pax.size()), pax);
    changed.push_back(metadata.str() + original);
    for (std::size_t index = 0; index < changed.size(); ++index) {
        const auto destination = root / std::to_string(index);
        std::istringstream input(changed[index], std::ios::in | std::ios::binary);
        require_tar_rejected_without_publication(
            destination, [&] { flitzzip::detail::extract_validated_tar_stream(input, plan, destination, false, {}); });
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject a changed second-pass inventory before publication, including apparently clean end markers.
// Inputs: An admitted one-file plan and empty, extra or dangling-extension replacement streams.
// Outputs: Every mismatch throws, preserving the destination sentinel and leaving no unpublished temporary files.
TEST_CASE(tar_stream_extract_rejects_missing_extra_and_dangling_entries) {
    const auto root = test_temp_dir("tar-second-pass-inventory");
    std::ostringstream valid(std::ios::out | std::ios::binary);
    append_tar_fixture_record(valid, make_test_tar_header("before.txt", '0', 3), "abc");
    std::istringstream scan(valid.str(), std::ios::in | std::ios::binary);
    const auto plan = flitzzip::detail::scan_tar_stream(scan, false);
    std::vector<std::string> replacements{"", std::string(1024U, '\0')};
    for (const auto type : {'x', 'L'}) {
        std::ostringstream extension(std::ios::out | std::ios::binary);
        const auto payload = type == 'x' ? test_pax_record("path", "before.txt") : std::string("before.txt\0", 11U);
        append_tar_fixture_record(extension, make_test_tar_header("extension", type, payload.size()), payload);
        replacements.push_back(extension.str());
    }
    for (std::size_t index = 0; index < replacements.size(); ++index) {
        std::istringstream changed(replacements[index], std::ios::in | std::ios::binary);
        require_tar_rejected_without_publication(root / std::to_string(index), [&] {
            flitzzip::detail::extract_validated_tar_stream(changed, plan, root / std::to_string(index), false, {});
        });
    }
    std::istringstream empty_scan("", std::ios::in | std::ios::binary);
    const auto empty_plan = flitzzip::detail::scan_tar_stream(empty_scan, false);
    std::istringstream extra(valid.str(), std::ios::in | std::ios::binary);
    require_tar_rejected_without_publication(root / "extra", [&] {
        flitzzip::detail::extract_validated_tar_stream(extra, empty_plan, root / "extra", false, {});
    });
    std::filesystem::remove_all(root);
}

// Purpose: Preserve validated directory records and following file alignment in the non-seekable extraction pass.
// Inputs: An independent directory/file TAR with a legal directory payload that must be skipped, not published.
// Outputs: The exact directory and file bytes, correct admitted totals and no directory payload file.
TEST_CASE(tar_stream_extract_directory_payload_does_not_shift_following_file) {
    const auto root = test_temp_dir("tar-directory-stream-alignment");
    std::ostringstream wire(std::ios::out | std::ios::binary);
    append_tar_fixture_record(wire, make_test_tar_header("nested/", '5', 3), "pad");
    append_tar_fixture_record(wire, make_test_tar_header("nested/after.txt", '0', 5), "after");
    std::istringstream scan(wire.str(), std::ios::in | std::ios::binary);
    const auto plan = flitzzip::detail::scan_tar_stream(scan, false);
    REQUIRE_EQ(plan.entries.size(), 2U);
    REQUIRE_EQ(plan.total_file_bytes, 5U);
    std::istringstream input(wire.str(), std::ios::in | std::ios::binary);
    flitzzip::detail::extract_validated_tar_stream(input, plan, root, false, {});
    {
        std::ifstream restored(root / "nested" / "after.txt", std::ios::binary);
        REQUIRE_TRUE(restored.is_open());
        REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), "after");
    }
    REQUIRE_EQ(
        std::distance(std::filesystem::directory_iterator(root / "nested"), std::filesystem::directory_iterator{}), 1);
    std::filesystem::remove_all(root);
}

// Purpose: Exercise real second-pass header/payload failures and cleanup after a reservation has been acquired.
// Inputs: A valid admitted plan and physical EOF or I/O failure at each first-header/payload edge.
// Outputs: No incomplete file, reservation or changed sentinel survives; valid reuse restores both exact payloads.
TEST_CASE(tar_stream_extract_read_failure_cleans_unpublished_file) {
    const auto root = test_temp_dir("tar-second-pass-input-failure");
    const auto bytes = tar_reader_boundary_fixture();
    std::istringstream scan_input(bytes, std::ios::in | std::ios::binary);
    const auto plan = flitzzip::detail::scan_tar_stream(scan_input, false);
    for (const bool io_error : {false, true}) {
        for (const std::size_t boundary : {0U, 1U, 511U, 512U, 513U, 514U}) {
            TarBoundaryInput buffer(bytes, boundary, io_error);
            std::istream input(&buffer);
            const auto destination = root / (std::to_string(io_error) + "-" + std::to_string(boundary));
            require_tar_rejected_without_publication(destination, [&] {
                flitzzip::detail::extract_validated_tar_stream(input, plan, destination, false, {});
            });
            REQUIRE_TRUE(buffer.boundary_read());
            std::istringstream recovery(bytes, std::ios::in | std::ios::binary);
            flitzzip::detail::extract_validated_tar_stream(recovery, plan, destination, false, {});
            for (const auto& [name, expected] :
                 std::array<std::pair<const char*, const char*>, 2>{{{"before.txt", "abc"}, {"after.txt", "defgh"}}}) {
                std::ifstream restored(destination / name, std::ios::binary);
                REQUIRE_TRUE(restored.is_open());
                REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(restored), {}), expected);
            }
            REQUIRE_EQ(
                std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator{}),
                3);
        }
    }
    std::filesystem::remove_all(root);
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

namespace {

// Purpose: Reject unsupported TAR numeric widths during overload resolution, before any header can be written.
// Inputs: A field's static extent. Outputs: Whether the production encoder admits that width.
template <std::size_t Width>
concept TarNumericWidth =
    requires(std::span<char, Width> field) { flitzzip::tar_detail::put_tar_number(field, std::uint64_t{0}); };
static_assert(TarNumericWidth<7> && TarNumericWidth<8> && TarNumericWidth<12>);
static_assert(!TarNumericWidth<0> && !TarNumericWidth<1> && !TarNumericWidth<6> && !TarNumericWidth<13>);

// Purpose: Compare actual wire bytes with independent GNU TAR vectors while guarding both field boundaries.
// Inputs: Unsigned metadata and a Python-standard-library-generated literal, including all field bytes.
// Outputs: Exact octal/base-256 field and unchanged surrounding header bytes.
template <std::size_t Size> void require_tar_numeric_wire(std::uint64_t value, const char (&expected)[Size]) {
    constexpr std::size_t width = Size - 1U;
    std::array<char, width + 4U> buffer;
    buffer.fill('!');
    auto field = std::span(buffer).template subspan<2, width>();
    flitzzip::tar_detail::put_tar_number(field, value);
    for (std::size_t index = 0; index < width; ++index) {
        REQUIRE_EQ(static_cast<unsigned char>(field[index]), static_cast<unsigned char>(expected[index]));
    }
    REQUIRE_TRUE(buffer.front() == '!' && buffer[1] == '!');
    REQUIRE_TRUE(buffer[width + 2U] == '!' && buffer.back() == '!');
}

// Purpose: Preserve header state when a valid unsigned value cannot fit a narrower GNU binary field.
// Inputs: Actual TAR field width and an out-of-range value. Outputs: Refusal before writes and valid same-field reuse.
template <std::size_t Width> void require_tar_numeric_overflow(std::uint64_t value) {
    std::array<char, Width + 4U> buffer;
    buffer.fill('!');
    const auto before = buffer;
    auto field = std::span(buffer).template subspan<2, Width>();
    bool refused = false;
    try {
        flitzzip::tar_detail::put_tar_number(field, value);
    } catch (const flitzzip::ArchiveError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_TRUE(buffer == before);
    flitzzip::tar_detail::put_tar_number(field, 0);
    REQUIRE_TRUE(std::all_of(field.begin(), field.end() - 1, [](char byte) { return byte == '0'; }));
    REQUIRE_EQ(field.back(), '\0');
    REQUIRE_TRUE(buffer.front() == '!' && buffer[1] == '!');
    REQUIRE_TRUE(buffer[Width + 2U] == '!' && buffer.back() == '!');
}

}  // namespace

// Purpose: Qualify every production numeric width, its octal/binary boundary and the complete unsigned size domain.
// Inputs: Golden fields independently captured from CPython 3.13 tarfile.itn with GNU_FORMAT.
// Outputs: Exact bytes with no large files, fabricated buffer extents or shared writer/reader oracle.
TEST_CASE(tar_header_numeric_fields_match_independent_gnu_vectors) {
    require_tar_numeric_wire(0ULL, "\x30\x30\x30\x30\x30\x30\x00");
    require_tar_numeric_wire(262143ULL, "\x37\x37\x37\x37\x37\x37\x00");
    require_tar_numeric_wire(262144ULL, "\x80\x00\x00\x00\x04\x00\x00");
    require_tar_numeric_wire(281474976710655ULL, "\x80\xff\xff\xff\xff\xff\xff");
    require_tar_numeric_wire(0ULL, "\x30\x30\x30\x30\x30\x30\x30\x00");
    require_tar_numeric_wire(7ULL, "\x30\x30\x30\x30\x30\x30\x37\x00");
    require_tar_numeric_wire(8ULL, "\x30\x30\x30\x30\x30\x31\x30\x00");
    require_tar_numeric_wire(2097151ULL, "\x37\x37\x37\x37\x37\x37\x37\x00");
    require_tar_numeric_wire(2097152ULL, "\x80\x00\x00\x00\x00\x20\x00\x00");
    require_tar_numeric_wire(4294967295ULL, "\x80\x00\x00\x00\xff\xff\xff\xff");
    require_tar_numeric_wire(72057594037927935ULL, "\x80\xff\xff\xff\xff\xff\xff\xff");
    require_tar_numeric_wire(0ULL, "\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x30\x00");
    require_tar_numeric_wire(8589934591ULL, "\x37\x37\x37\x37\x37\x37\x37\x37\x37\x37\x37\x00");
    require_tar_numeric_wire(8589934592ULL, "\x80\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00");
    require_tar_numeric_wire(9223372036854775808ULL, "\x80\x00\x00\x00\x80\x00\x00\x00\x00\x00\x00\x00");
    require_tar_numeric_wire(18446744073709551615ULL, "\x80\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff");
}

// Purpose: Keep failed metadata encoding from corrupting a reusable bounded header.
// Inputs: Values at and beyond the seven/eight-byte binary limit. Outputs: Unchanged bytes, then correct zero encoding.
TEST_CASE(tar_header_numeric_overflow_preserves_field_and_allows_reuse) {
    require_tar_numeric_overflow<7>(std::uint64_t{1} << 48U);
    require_tar_numeric_overflow<7>(std::numeric_limits<std::uint64_t>::max());
    require_tar_numeric_overflow<8>(std::uint64_t{1} << 56U);
    require_tar_numeric_overflow<8>(std::numeric_limits<std::uint64_t>::max());
}

// Purpose: Enforce actual USTAR filename byte bounds and preserve neighboring header fields on rejection.
// Inputs: An exact-width name, one excess byte and a subsequent UTF-8 value. Outputs: Exact copies or unchanged bytes.
TEST_CASE(tar_header_text_fields_preserve_bounds_and_reject_before_writing) {
    std::array<char, 104> buffer;
    buffer.fill('!');
    auto field = std::span(buffer).subspan<2, 100>();
    const std::string exact(100, 'x');
    flitzzip::tar_detail::put_tar_string(field, exact);
    REQUIRE_TRUE(std::equal(field.begin(), field.end(), exact.begin()));
    const auto before = buffer;
    bool refused = false;
    try {
        flitzzip::tar_detail::put_tar_string(field, exact + "x");
    } catch (const flitzzip::ArchiveError&) {
        refused = true;
    }
    REQUIRE_TRUE(refused);
    REQUIRE_TRUE(buffer == before);
    std::fill(field.begin(), field.end(), '\0');
    const std::string unicode = "\xE6\x97\xA5";
    flitzzip::tar_detail::put_tar_string(field, unicode);
    REQUIRE_TRUE(std::equal(unicode.begin(), unicode.end(), field.begin()));
    REQUIRE_TRUE(std::all_of(field.begin() + unicode.size(), field.end(), [](char byte) { return byte == '\0'; }));
    REQUIRE_TRUE(buffer.front() == '!' && buffer[1] == '!' && buffer[102] == '!' && buffer.back() == '!');
}
