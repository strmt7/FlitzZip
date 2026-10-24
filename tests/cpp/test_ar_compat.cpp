#include "test_compat_fixture.hpp"
#include "test_util.hpp"
#include "test_legacy_name.hpp"

#include "ar/ar_adapter.hpp"
#include "ar/ar_reader.hpp"
#include "core/archive_format.hpp"
#include "core/result.hpp"
#include "core/resource_limits.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

namespace {

// Purpose: Read a full text file for roundtrip equality checks.
// Inputs: `path` is the file to read.
// Outputs: Returns the complete file payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for no-output assertions.
// Inputs: `root` is the directory tree to inspect.
// Outputs: Returns the number of regular files; missing roots count as zero.
std::uint64_t count_regular_files(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) {
        return 0;
    }
    std::uint64_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

// Purpose: Create one fixed-width AR header field.
// Inputs: `header` is the mutable header, `offset`/`width` select the field, and `value` is ASCII metadata.
// Outputs: Copies the value into a space-padded field.
void put_ar_field(std::array<char, 60>& header, std::size_t offset, std::size_t width, std::string_view value) {
    REQUIRE_TRUE(value.size() <= width);
    std::copy(value.begin(), value.end(), header.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Build one AR member header for handcrafted fixtures.
// Inputs: `name` is the raw 16-byte name field and `size` is the declared member size.
// Outputs: Returns a complete 60-byte AR header.
std::array<char, 60> make_ar_header(std::string_view name, std::uint64_t size) {
    std::array<char, 60> header{};
    std::fill(header.begin(), header.end(), ' ');
    put_ar_field(header, 0, 16, name);
    put_ar_field(header, 16, 12, "0");
    put_ar_field(header, 28, 6, "0");
    put_ar_field(header, 34, 6, "0");
    put_ar_field(header, 40, 8, "100644");
    put_ar_field(header, 48, 10, std::to_string(size));
    header[58] = '`';
    header[59] = '\n';
    return header;
}

// Purpose: Write one AR member with a raw header name.
// Inputs: `output` is the archive stream, `name` is the header name, and `payload` is the member payload.
// Outputs: Appends the member and even-byte padding when needed.
void write_ar_member(std::ostream& output, std::string_view name, std::string_view payload) {
    const auto header = make_ar_header(name, payload.size());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if ((payload.size() % 2U) != 0) {
        output.put('\n');
    }
}

// Purpose: Write one AR member whose size field is supplied as exact fixture bytes.
// Inputs: `output` is the archive, `name`/`payload` form the member, and `size_field` must contain ten bytes.
// Outputs: Appends the independently framed member and payload padding.
void write_ar_member_with_size_field(std::ofstream& output, std::string_view name, std::string_view size_field,
                                     std::string_view payload) {
    REQUIRE_EQ(size_field.size(), 10U);
    auto header = make_ar_header(name, payload.size());
    std::copy(size_field.begin(), size_field.end(), header.begin() + 48);
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if ((payload.size() % 2U) != 0) {
        output.put('\n');
    }
}

// Purpose: Write one BSD long-name AR member for parser-security tests.
// Inputs: `output` is the archive stream, `path` is the embedded filename, and `payload` is the file data.
// Outputs: Appends a complete `#1/<len>` member.
void write_bsd_ar_member(std::ofstream& output, std::string_view path, std::string_view payload) {
    const auto header = make_ar_header("#1/" + std::to_string(path.size()), path.size() + payload.size());
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(path.data(), static_cast<std::streamsize>(path.size()));
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (((path.size() + payload.size()) % 2U) != 0) {
        output.put('\n');
    }
}

// Purpose: Write one complete AR archive with a single BSD long-name member.
// Inputs: `archive` is the output path, `path` is the embedded filename, and `payload` is the file data.
// Outputs: Creates a complete AR file.
void write_one_bsd_ar_archive(const std::filesystem::path& archive, std::string_view path, std::string_view payload) {
    std::ofstream output(archive, std::ios::binary);
    output.write("!<arch>\n", 8);
    write_bsd_ar_member(output, path, payload);
}

// Purpose: Write an AR archive that uses the GNU `//` long-name string table.
// Inputs: `archive` is the output path.
// Outputs: Creates a complete AR file with one extractable GNU long-name member.
void write_gnu_long_name_ar_archive(const std::filesystem::path& archive) {
    std::ofstream output(archive, std::ios::binary);
    output.write("!<arch>\n", 8);
    const std::string table = "dir/gnu-long-name.txt/\n";
    write_ar_member(output, "//", table);
    write_ar_member(output, "/0", "gnu payload");
}

// Purpose: Require an AR archive to fail during preflight without publishing an extraction directory.
// Inputs: `archive` is malformed, `output` is its unused destination, and `diagnostic` is required error text.
// Outputs: Fails the test unless `ArchiveError` contains the diagnostic and the destination remains absent.
void require_ar_rejection_before_output(const std::filesystem::path& archive, const std::filesystem::path& output,
                                        std::string_view diagnostic) {
    bool rejected = false;
    try {
        (void)flitzzip::extract_ar(archive, output, false);
    } catch (const flitzzip::ArchiveError& error) {
        rejected = std::string_view(error.what()).find(diagnostic) != std::string_view::npos;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output));
}

// Purpose: Present a complete owned archive with a controlled read boundary without altering filesystem state.
// Inputs: Bytes own the full archive; boundary is the first unavailable byte; io_error selects bad I/O versus short
// EOF. Outputs: Reads before the boundary behave normally; later reads fail synchronously and positions remain
// queryable.
class ArBoundaryInput final : public std::streambuf {
  public:
    // Purpose: Install a bounded get area after validating its extent.
    // Inputs: Owned bytes, a byte boundary within them and the failure kind.
    // Outputs: Creates a position-aware stream buffer or throws before forming an invalid endpoint.
    ArBoundaryInput(std::string bytes, std::size_t boundary, bool io_error)
        : bytes_(std::move(bytes)), io_error_(io_error) {
        if (boundary > bytes_.size()) {
            throw std::invalid_argument("AR test stream boundary exceeds input");
        }
        setg(bytes_.data(), bytes_.data(), bytes_.data() + boundary);
    }

  protected:
    // Purpose: Distinguish a device-style read failure from a clean short stream at the controlled boundary.
    // Inputs: The exhausted get area.
    // Outputs: Throws an I/O failure or returns EOF; never supplies bytes beyond the boundary.
    int_type underflow() override {
        boundary_read_ = true;
        if (io_error_) {
            throw std::ios_base::failure("controlled AR input read failure");
        }
        return traits_type::eof();
    }

    // Purpose: Preserve normal file-style positioning within the readable prefix.
    // Inputs: Input-mode seek offsets and one of the standard reference positions.
    // Outputs: Moves within the validated get area, or returns the failed-position sentinel before invalid arithmetic.
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode mode) override {
        const auto end = static_cast<off_type>(egptr() - eback());
        off_type base = 0;
        if ((mode & std::ios_base::in) == 0) {
            return pos_type(off_type(-1));
        }
        if (direction == std::ios_base::cur) {
            base = static_cast<off_type>(gptr() - eback());
        } else if (direction == std::ios_base::end) {
            base = end;
        } else if (direction != std::ios_base::beg) {
            return pos_type(off_type(-1));
        }
        if (offset < -base || offset > end - base) {
            return pos_type(off_type(-1));
        }
        const auto position = base + offset;
        setg(eback(), eback() + position, egptr());
        return pos_type(position);
    }

  public:
    // Purpose: Prove a rejection reached the intended read boundary rather than an unrelated unsupported operation.
    // Inputs: The completed parser attempt. Outputs: True only after an actual unavailable-byte read was requested.
    bool boundary_read() const noexcept {
        return boundary_read_;
    }

  private:
    std::string bytes_;
    bool io_error_;
    bool boundary_read_ = false;
};

// Purpose: Build a valid two-member AR fixture and identify its exact first-member boundary independently.
// Inputs: None; the existing raw-header serializer writes only into RAM.
// Outputs: Returns complete bytes and the first position at which a valid prefix could be mistaken for completion.
std::pair<std::string, std::size_t> ar_reader_boundary_fixture() {
    std::ostringstream output(std::ios::out | std::ios::binary);
    output << "!<arch>\n";
    write_ar_member(output, "before.o/", "abc");
    const auto position = output.tellp();
    REQUIRE_TRUE(position != std::ostream::pos_type(-1));
    const auto boundary = static_cast<std::size_t>(position);
    write_ar_member(output, "after.o/", "defgh");
    return {output.str(), boundary};
}

// Purpose: Require failure at every unread position after a valid member, including the next header's first byte.
// Inputs: io_error chooses an injected read error or premature physical EOF; declared bytes remain complete.
// Outputs: Throws a test failure if the production parser accepts any incomplete archive prefix.
void require_ar_reader_boundary_failures(bool io_error) {
    const auto [bytes, boundary] = ar_reader_boundary_fixture();
    for (auto unavailable = boundary; unavailable < boundary + 60U; ++unavailable) {
        ArBoundaryInput buffer(bytes, unavailable, io_error);
        std::istream input(&buffer);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_ar_stream(input, bytes.size(), flitzzip::ArchivePathEncoding::Utf8);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(buffer.boundary_read());
        REQUIRE_TRUE(rejected);
    }
}

}  // namespace

// Purpose: Prevent a non-EOF read error from being classified as a clean AR terminator.
// Inputs: A valid two-member archive whose second member becomes unreadable at each byte boundary.
// Outputs: The actual bounded production parser rejects every failed read after a valid prefix.
TEST_CASE(ar_reader_rejects_read_errors_after_valid_prefix) {
    require_ar_reader_boundary_failures(true);
}

// Purpose: Reject physical truncation against the independently established archive extent.
// Inputs: A valid two-member archive that presents early EOF at every boundary after the first member.
// Outputs: No shortened publication plan is accepted.
TEST_CASE(ar_reader_rejects_premature_eof_after_valid_prefix) {
    require_ar_reader_boundary_failures(false);
}

// Purpose: Keep valid parser behavior and payload locations independent of fault-injection expectations.
// Inputs: An ordinary complete in-memory AR stream with two known payloads and odd-byte padding.
// Outputs: Returns both exact names, lengths and original payload slices.
TEST_CASE(ar_reader_complete_stream_preserves_payload_extents) {
    const auto [bytes, boundary] = ar_reader_boundary_fixture();
    std::istringstream input(bytes, std::ios::in | std::ios::binary);
    const auto plan = flitzzip::detail::scan_ar_stream(input, bytes.size(), flitzzip::ArchivePathEncoding::Utf8);
    REQUIRE_EQ(plan.entries.size(), 2U);
    REQUIRE_EQ(plan.total_file_bytes, 8U);
    REQUIRE_EQ(plan.entries[0].path, "before.o");
    REQUIRE_EQ(plan.entries[1].path, "after.o");
    REQUIRE_EQ(plan.entries[0].payload_offset, 68U);
    REQUIRE_EQ(plan.entries[1].payload_offset, boundary + 60U);
    REQUIRE_EQ(bytes.substr(plan.entries[0].payload_offset, plan.entries[0].size), "abc");
    REQUIRE_EQ(bytes.substr(plan.entries[1].payload_offset, plan.entries[1].size), "defgh");
}

// Purpose: Keep an exact complete extent independent of any subsequent read or underlying EOF probe.
// Inputs: A complete archive whose next byte access deliberately fails as an I/O error.
// Outputs: Both entries validate without requesting any byte beyond the declared archive.
TEST_CASE(ar_reader_complete_extent_does_not_probe_past_end) {
    const auto [bytes, boundary] = ar_reader_boundary_fixture();
    ArBoundaryInput buffer(bytes, bytes.size(), true);
    std::istream input(&buffer);
    const auto plan = flitzzip::detail::scan_ar_stream(input, bytes.size(), flitzzip::ArchivePathEncoding::Utf8);
    REQUIRE_EQ(plan.entries.size(), 2U);
    REQUIRE_TRUE(!buffer.boundary_read());
    REQUIRE_TRUE(input.good());
    REQUIRE_TRUE(boundary < bytes.size());
}

// Purpose: Reject a short declared next-header extent without borrowing bytes outside that extent.
// Inputs: A complete backing stream with a deliberately bounded archive ending inside the second header.
// Outputs: Rejects every partial extent while leaving the stream at the previous member boundary.
TEST_CASE(ar_reader_partial_header_extent_never_reads_beyond_bound) {
    const auto [bytes, boundary] = ar_reader_boundary_fixture();
    for (std::size_t tail = 1; tail < 60U; ++tail) {
        std::istringstream input(bytes, std::ios::in | std::ios::binary);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_ar_stream(input, boundary + tail, flitzzip::ArchivePathEncoding::Utf8);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(input.tellg(), std::istream::pos_type(boundary));
    }
}

// Purpose: Enforce the global-header extent before reading bytes, including an empty valid archive control.
// Inputs: Correct magic with declared lengths below and exactly at its eight-byte width.
// Outputs: Short extents fail without consuming the backing stream; the exact empty archive succeeds.
TEST_CASE(ar_reader_global_header_extent_is_checked_before_read) {
    const std::string magic = "!<arch>\n";
    for (std::size_t extent = 0; extent < magic.size(); ++extent) {
        std::istringstream input(magic, std::ios::in | std::ios::binary);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_ar_stream(input, extent, flitzzip::ArchivePathEncoding::Utf8);
        } catch (const flitzzip::ArchiveError&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(input.tellg(), std::istream::pos_type(0));
    }
    ArBoundaryInput buffer(magic, magic.size(), true);
    std::istream input(&buffer);
    const auto plan = flitzzip::detail::scan_ar_stream(input, magic.size(), flitzzip::ArchivePathEncoding::Utf8);
    REQUIRE_TRUE(plan.entries.empty());
    REQUIRE_TRUE(!buffer.boundary_read());
}

// Purpose: Verify native `.ar` compatibility roundtrip for files in a directory tree.
// Inputs: A temporary source tree compressed through the AR adapter.
// Outputs: Throws if extraction fails, format detection is wrong, or restored contents differ.
TEST_CASE(ar_roundtrip_extracts_files_from_directories) {
    const auto root = test_temp_dir("ar-roundtrip");
    const auto input = root / "input";
    std::filesystem::create_directories(input / "dir");
    {
        std::ofstream(input / "dir" / "hello.txt") << "hello from ar\n";
        std::ofstream(input / "root.txt") << "root file\n";
    }

    const auto archive = root / "sample.ar";
    const auto compress_stats = flitzzip::compress_ar({input}, archive);
    REQUIRE_EQ(compress_stats.entries, static_cast<std::uint64_t>(2));
    REQUIRE_TRUE(compress_stats.output_bytes > compress_stats.input_bytes);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Ar);

    const auto output = root / "output";
    const auto extract_stats = flitzzip::extract_ar(archive, output, false);
    REQUIRE_EQ(extract_stats.output_bytes, compress_stats.input_bytes);
    REQUIRE_EQ(read_text_file(output / "input" / "dir" / "hello.txt"), read_text_file(input / "dir" / "hello.txt"));
    REQUIRE_EQ(read_text_file(output / "input" / "root.txt"), read_text_file(input / "root.txt"));
}

// Purpose: Preserve Unicode AR member names without confusing byte lengths with decoded character lengths.
// Inputs: Production BSD long-name archives containing a supplementary Unicode file and an ASCII sibling.
// Outputs: Requires exact filenames, payloads, and member count after extraction.
TEST_CASE(ar_unicode_member_names_roundtrip) {
    const auto root = test_temp_dir("ar-unicode-members");
    const auto source = root / "source";
    std::filesystem::create_directories(source);
    const auto name = std::filesystem::path(L"\u65e5\u672c-\U0001f680.txt");
    std::ofstream(source / name, std::ios::binary) << "unicode payload";
    std::ofstream(source / "sibling.txt", std::ios::binary) << "sibling payload";
    const auto archive = root / "fixture.ar";
    const auto output = root / "output";
    (void)flitzzip::compress_ar({source}, archive);
    (void)flitzzip::extract_ar(archive, output, false);
    REQUIRE_EQ(read_text_file(output / "source" / name), "unicode payload");
    REQUIRE_EQ(read_text_file(output / "source" / "sibling.txt"), "sibling payload");
    REQUIRE_EQ(count_regular_files(output), 2U);
}

// Purpose: Preserve explicitly selected legacy names without changing raw BSD byte offsets or GNU references.
// Inputs: Independently encoded Windows ANSI names in short, BSD, and GNU members followed by an ASCII sibling.
// Outputs: Requires exact native names, both payloads, and member counts for every layout.
TEST_CASE(ar_legacy_member_names_all_layouts) {
    const auto root = test_temp_dir("ar-legacy-members");
    const auto [raw, expected] = test_legacy_archive_name();
    for (int layout = 0; layout < 3; ++layout) {
        const auto archive = root / (std::to_string(layout) + ".ar");
        {
            std::ofstream stream(archive, std::ios::binary);
            stream.write("!<arch>\n", 8);
            if (layout == 0) {
                write_ar_member(stream, raw + "/", "legacy");
            } else if (layout == 1) {
                write_bsd_ar_member(stream, raw, "legacy");
            } else {
                write_ar_member(stream, "//", raw + "/\n");
                write_ar_member(stream, "/0", "legacy");
            }
            write_ar_member(stream, "sibling.txt/", "next payload");
        }
        const auto output = root / std::to_string(layout);
        const auto stats =
            flitzzip::extract_ar(archive, output, false, {}, flitzzip::ArchivePathEncoding::HostCodePage);
        REQUIRE_EQ(stats.entries, 2U);
        REQUIRE_EQ(read_text_file(output / expected), "legacy");
        REQUIRE_EQ(read_text_file(output / "sibling.txt"), "next payload");
        REQUIRE_EQ(count_regular_files(output), 2U);
        if (layout == 1) {
            flitzzip_test::export_compat_fixture(archive, output);
        }
    }
}

// Purpose: Reject malformed default-encoding metadata during the complete AR preflight.
// Inputs: A valid first member followed by a truncated UTF-8 BSD name.
// Outputs: Requires ArchiveError before the destination exists, including for the earlier valid member.
TEST_CASE(ar_invalid_member_encoding_prevents_all_output) {
    const auto root = test_temp_dir("ar-invalid-name-encoding");
    const auto archive = root / "fixture.ar";
    {
        std::ofstream stream(archive, std::ios::binary);
        stream.write("!<arch>\n", 8);
        write_ar_member(stream, "valid.txt/", "valid");
        write_bsd_ar_member(stream, "\xf0\x90.txt", "invalid");
    }
    const auto output = root / "output";
    bool rejected = false;
    try {
        (void)flitzzip::extract_ar(archive, output, false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(output));
}

// Purpose: Verify GNU string-table AR names are resolved during extraction.
// Inputs: A handcrafted AR archive containing a `//` string table and `/0` member reference.
// Outputs: Throws if the long-name member is not restored at the expected path.
TEST_CASE(ar_extracts_gnu_string_table_names) {
    const auto root = test_temp_dir("ar-gnu-long-name");
    const auto archive = root / "gnu.ar";
    write_gnu_long_name_ar_archive(archive);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_ar(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "dir" / "gnu-long-name.txt"), "gnu payload");
}

// Purpose: Reject GNU long-name references into the middle of a string-table entry.
// Inputs: A valid table with two names and a numeric reference two bytes into the first name.
// Outputs: Requires canonical entry-boundary validation before any extraction directory is published.
TEST_CASE(ar_gnu_long_name_offsets_must_start_at_entry_boundaries) {
    const auto root = test_temp_dir("ar-gnu-long-name-boundary");
    const auto archive = root / "gnu-invalid.ar";
    {
        std::ofstream output(archive, std::ios::binary);
        output.write("!<arch>\n", 8);
        write_ar_member(output, "//", "alpha.txt/\nbeta.txt/\n");
        write_ar_member(output, "/2", "payload");
    }
    require_ar_rejection_before_output(archive, root / "out", "does not identify a GNU string-table entry");
    std::filesystem::remove_all(root);
}

// Purpose: Preserve BSD filename padding as framing, rather than filename or file-content bytes.
// Inputs: Different NUL-padding lengths, an odd payload and a following ordinary member.
// Outputs: Exact decoded names, content, entry count and byte totals without losing the next header.
TEST_CASE(ar_bsd_padded_names_preserve_payload_and_following_members) {
    const auto root = test_temp_dir("ar-bsd-padded-names");
    for (unsigned padding = 0; padding < 8; ++padding) {
        const auto archive = root / "input.ar";
        {
            std::ofstream output(archive, std::ios::binary);
            output.write("!<arch>\n", 8);
            write_bsd_ar_member(output, "long filename.o" + std::string(padding, '\0'), "odd");
            write_ar_member(output, "next.o/", "next payload");
        }
        const auto destination = root / std::to_string(padding);
        const auto stats = flitzzip::extract_ar(archive, destination, false);
        REQUIRE_EQ(stats.entries, 2U);
        REQUIRE_EQ(stats.output_bytes, 15U);
        REQUIRE_EQ(read_text_file(destination / "long filename.o"), "odd");
        REQUIRE_EQ(read_text_file(destination / "next.o"), "next payload");
        REQUIRE_EQ(count_regular_files(destination), 2U);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep symbol-table metadata nonextracting after every supported filename resolution.
// Inputs: Leading GNU markers and BSD/Darwin 32/64-bit indexes in short, unpadded and padded BSD layouts.
// Outputs: Only the exact before/after files become visible; metadata bytes never count as extracted content.
TEST_CASE(ar_resolved_symbol_tables_do_not_become_regular_files) {
    const auto root = test_temp_dir("ar-resolved-symbol-tables");
    unsigned ordinal = 0;
    for (const std::string name :
         {"__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED", "/", "/SYM64/"}) {
        for (unsigned layout = 0; layout < 3; ++layout) {
            if (layout != 0 && name.starts_with('/')) {
                continue;  // GNU index markers are raw header syntax, not BSD filenames.
            }
            if (layout == 0 && name.size() > 16U) {
                continue;  // The fixed header physically cannot contain the longer Darwin name.
            }
            const auto archive = root / "input.ar";
            {
                std::ofstream output(archive, std::ios::binary);
                output.write("!<arch>\n", 8);
                if (layout == 0) {
                    write_ar_member(output, name, "symbol metadata");
                } else {
                    write_bsd_ar_member(output, name + std::string(layout == 2 ? 3U : 0U, '\0'), "symbol metadata");
                }
                write_ar_member(output, "before.o/", "before");
                write_ar_member(output, "after.o/", "after");
            }
            const auto destination = root / std::to_string(ordinal++);
            const auto stats = flitzzip::extract_ar(archive, destination, false);
            REQUIRE_EQ(stats.entries, 2U);
            REQUIRE_EQ(stats.output_bytes, 11U);
            REQUIRE_EQ(read_text_file(destination / "before.o"), "before");
            REQUIRE_EQ(read_text_file(destination / "after.o"), "after");
            REQUIRE_EQ(count_regular_files(destination), 2U);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent name-only classification from dropping a regular member after the leading archive index slot.
// Inputs: A preceding file, every BSD/Darwin symbol-like basename in short/BSD/GNU layouts and a following file.
// Outputs: All three files retain exact bytes and sizes, including when a GNU string table precedes the name.
TEST_CASE(ar_later_symbol_like_names_remain_ordinary_members) {
    const auto root = test_temp_dir("ar-later-symbol-names");
    const std::string content = "ordinary named data";
    unsigned ordinal = 0;
    for (const std::string name : {"__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"}) {
        for (unsigned layout = 0; layout < 3; ++layout) {
            if (layout == 0 && name.size() > 16U) {
                continue;
            }
            const auto archive = root / "input.ar";
            {
                std::ofstream output(archive, std::ios::binary);
                output.write("!<arch>\n", 8);
                write_ar_member(output, "before.o/", "before");
                if (layout == 0) {
                    write_ar_member(output, name, content);
                } else if (layout == 1) {
                    write_bsd_ar_member(output, name + std::string(3, '\0'), content);
                } else {
                    write_ar_member(output, "//", name + "/\n");
                    write_ar_member(output, "/0", content);
                }
                write_ar_member(output, "after.o/", "after");
            }
            const auto destination = root / std::to_string(ordinal++);
            const auto stats = flitzzip::extract_ar(archive, destination, false);
            REQUIRE_EQ(stats.entries, 3U);
            REQUIRE_EQ(stats.output_bytes, 11U + content.size());
            REQUIRE_EQ(read_text_file(destination / "before.o"), "before");
            REQUIRE_EQ(read_text_file(destination / name), content);
            REQUIRE_EQ(read_text_file(destination / "after.o"), "after");
            REQUIRE_EQ(count_regular_files(destination), 3U);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Prevent BSD padding normalization from hiding embedded invalid bytes or unsafe paths.
// Inputs: Interior NUL data, empty/traversal names and GNU markers embedded as BSD names, first or after a file.
// Outputs: Complete preflight rejection before any destination is created.
TEST_CASE(ar_bsd_padding_does_not_hide_invalid_name_bytes) {
    const auto root = test_temp_dir("ar-bsd-invalid-padding");
    for (const auto& name :
         {std::string("file\0unexpected", 15), std::string(12, '\0'), std::string("../escape\0\0", 11),
          std::string("/\0\0", 3), std::string("/SYM64/\0\0", 9)}) {
        for (const bool leading : {false, true}) {
            const auto archive = root / "input.ar";
            {
                std::ofstream output(archive, std::ios::binary);
                output.write("!<arch>\n", 8);
                if (!leading) {
                    write_ar_member(output, "before.o/", "before");
                }
                write_bsd_ar_member(output, name, "payload");
            }
            const auto destination = root / "output";
            bool rejected = false;
            try {
                (void)flitzzip::extract_ar(archive, destination, false);
            } catch (const flitzzip::Error&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_TRUE(!std::filesystem::exists(destination));
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Keep a GNU archive's first regular file distinct from a BSD leading index slot.
// Inputs: A GNU string table first, its symbol-like filename reference second, and an ordinary sibling.
// Outputs: Both exact files remain extractable; nonextracting metadata still occupies its physical member slot.
TEST_CASE(ar_gnu_string_table_does_not_turn_a_file_into_a_bsd_index) {
    const auto root = test_temp_dir("ar-gnu-first-symbol-name");
    unsigned ordinal = 0;
    for (const std::string name : {"__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"}) {
        const auto archive = root / "input.ar";
        {
            std::ofstream output(archive, std::ios::binary);
            output.write("!<arch>\n", 8);
            write_ar_member(output, "//", name + "/\n");
            write_ar_member(output, "/0", "ordinary data");
            write_ar_member(output, "after.o/", "after");
        }
        const auto destination = root / std::to_string(ordinal++);
        const auto stats = flitzzip::extract_ar(archive, destination, false);
        REQUIRE_EQ(stats.entries, 2U);
        REQUIRE_EQ(stats.output_bytes, 18U);
        REQUIRE_EQ(read_text_file(destination / name), "ordinary data");
        REQUIRE_EQ(read_text_file(destination / "after.o"), "after");
        REQUIRE_EQ(count_regular_files(destination), 2U);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Bound parser work even when every member is nonextracting metadata.
// Inputs: Exactly the real member limit, then one additional independently framed zero-length symbol table.
// Outputs: Admits the exact boundary, refuses its successor before output, and writes less than 16 MiB of fixtures.
TEST_CASE(ar_metadata_members_consume_the_archive_entry_budget) {
    const auto root = test_temp_dir("ar-metadata-member-budget");
    const auto archive = root / "metadata.ar";
    const auto header = make_ar_header("/", 0);
    constexpr std::size_t batch_members = 1000;
    std::array<char, 60U * batch_members> batch{};
    for (std::size_t member = 0; member < batch_members; ++member) {
        std::copy(header.begin(), header.end(), batch.begin() + static_cast<std::ptrdiff_t>(member * 60U));
    }
    static_assert(60ULL * flitzzip::kMaxArchiveEntries + 68U < 16ULL * 1024U * 1024U);
    {
        std::ofstream output(archive, std::ios::binary);
        output.write("!<arch>\n", 8);
        std::size_t remaining = flitzzip::kMaxArchiveEntries;
        while (remaining) {
            const auto members = std::min(remaining, batch_members);
            output.write(batch.data(), static_cast<std::streamsize>(members * 60U));
            remaining -= members;
        }
        output.close();
        REQUIRE_TRUE(output.good());
    }
    const auto stats = flitzzip::extract_ar(archive, root / "at-limit", false);
    REQUIRE_EQ(stats.entries, 0U);
    REQUIRE_EQ(stats.output_bytes, 0U);
    REQUIRE_TRUE(std::filesystem::is_empty(root / "at-limit"));
    {
        std::ofstream output(archive, std::ios::binary | std::ios::app);
        output.write(header.data(), header.size());
        output.close();
        REQUIRE_TRUE(output.good());
    }
    require_ar_rejection_before_output(archive, root / "over-limit", "too many members");
    std::filesystem::remove_all(root);
}

// Purpose: Refuse a reserved leading AR name instead of creating an archive that drops user bytes.
// Inputs: Each leading BSD/Darwin symbol name, an existing output, and later/nested ordinary-file controls.
// Outputs: Refusal preserves existing output; later and nested files with the same basename still round trip.
TEST_CASE(ar_writer_rejects_leading_symbol_names_before_publication) {
    const auto root = test_temp_dir("ar-reserved-writer-names");
    const auto archive = root / "output.ar";
    {
        std::ofstream output(archive, std::ios::binary);
        output << "retain old output";
    }
    for (const std::string name : {"__.SYMDEF", "__.SYMDEF SORTED", "__.SYMDEF_64", "__.SYMDEF_64 SORTED"}) {
        const auto source = root / name;
        {
            std::ofstream output(source, std::ios::binary);
            output << "caller data";
        }
        bool rejected = false;
        try {
            (void)flitzzip::compress_ar({source}, archive);
        } catch (const flitzzip::Error&) {
            rejected = true;
        }
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(read_text_file(archive), "retain old output");
    }
    const auto folder = root / "nested";
    std::filesystem::create_directory(folder);
    {
        std::ofstream output(folder / "__.SYMDEF", std::ios::binary);
        output << "ordinary nested data";
    }
    (void)flitzzip::compress_ar({folder}, archive);
    (void)flitzzip::extract_ar(archive, root / "restored", false);
    REQUIRE_EQ(read_text_file(root / "restored" / "nested" / "__.SYMDEF"), "ordinary nested data");
    const auto first = root / "0-first.o";
    {
        std::ofstream output(first, std::ios::binary);
        output << "lead";
    }
    (void)flitzzip::compress_ar({first, root / "__.SYMDEF"}, archive);
    const auto later = flitzzip::extract_ar(archive, root / "later", false);
    REQUIRE_EQ(later.entries, 2U);
    REQUIRE_EQ(read_text_file(root / "later" / "0-first.o"), "lead");
    REQUIRE_EQ(read_text_file(root / "later" / "__.SYMDEF"), "caller data");
    std::filesystem::remove_all(root);
}

// Purpose: Verify `.deb` files use the native AR extraction path for outer members.
// Inputs: A Debian-package-named AR container with one regular member.
// Outputs: Throws if detection, registry support, or extraction routing assumptions regress.
TEST_CASE(deb_outer_container_extracts_with_native_ar_adapter) {
    const auto root = test_temp_dir("deb-outer-container");
    const auto archive = root / "package.deb";
    write_one_bsd_ar_archive(archive, "debian-binary", "2.0\n");

    const auto info = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Deb);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.bundled_native);
    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Deb);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_ar(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(1));
    REQUIRE_EQ(read_text_file(output / "debian-binary"), "2.0\n");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Verify AR extraction rejects traversal metadata during the validation pass.
// Inputs: A handcrafted BSD long-name AR member containing `../escape.txt`.
// Outputs: Throws if extraction accepts the unsafe path or writes any output.
TEST_CASE(ar_extraction_rejects_traversal_before_output) {
    const auto root = test_temp_dir("ar-traversal");
    const auto archive = root / "bad.ar";
    write_one_bsd_ar_archive(archive, "../escape.txt", "bad");

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_ar(archive, root / "out", false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify AR scanning rejects declared member extents past EOF before creating output.
// Inputs: A handcrafted archive whose header declares more bytes than the file contains.
// Outputs: Throws `ArchiveError` and leaves the extraction root without regular files.
TEST_CASE(ar_extraction_rejects_truncated_member_before_output) {
    const auto root = test_temp_dir("ar-truncated-member");
    const auto archive = root / "truncated.ar";
    {
        std::ofstream output(archive, std::ios::binary);
        output.write("!<arch>\n", 8);
        const auto header = make_ar_header("file.txt/", 100);
        output.write(header.data(), static_cast<std::streamsize>(header.size()));
        output.write("tiny", 4);
    }

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_ar(archive, root / "out", false));
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Reject ambiguous, signed, empty, control-containing, and non-ASCII AR member sizes.
// Inputs: A valid first member followed by malformed exact-width size fields and a twelve-byte payload.
// Outputs: Requires strict decimal diagnostics before any destination or earlier member is published.
TEST_CASE(ar_decimal_size_fields_require_contiguous_ascii_digits) {
    const auto root = test_temp_dir("ar-malformed-size-fields");
    const std::array<std::string, 7> invalid_fields = {
        "1 2       ",
        "+12       ",
        "-12       ",
        "12\t       ",
        "12\n       ",
        "          ",
        std::string("\xff") + "         ",
    };
    for (std::size_t index = 0; index < invalid_fields.size(); ++index) {
        const auto archive = root / (std::to_string(index) + ".ar");
        {
            std::ofstream stream(archive, std::ios::binary);
            stream.write("!<arch>\n", 8);
            write_ar_member(stream, "valid.txt/", "prefix");
            write_ar_member_with_size_field(stream, "invalid.txt/", invalid_fields[index], "abcdefghijkl");
        }
        const auto output = root / ("output-" + std::to_string(index));
        const auto diagnostic =
            invalid_fields[index] == "          " ? "empty AR decimal field: size" : "malformed AR decimal field: size";
        require_ar_rejection_before_output(archive, output, diagnostic);
    }
}

// Purpose: Preserve valid outer-space padding, zero padding, and zero-length AR members.
// Inputs: Handcrafted members using leading/trailing spaces, ten decimal digits, and a padded zero.
// Outputs: Requires exact payloads, an empty file, and complete extraction statistics.
TEST_CASE(ar_decimal_size_fields_accept_canonical_padding_and_zero) {
    const auto root = test_temp_dir("ar-valid-size-fields");
    const auto archive = root / "valid.ar";
    {
        std::ofstream stream(archive, std::ios::binary);
        stream.write("!<arch>\n", 8);
        write_ar_member_with_size_field(stream, "outer.txt/", "  6       ", "outer!");
        write_ar_member_with_size_field(stream, "zeroes.txt/", "0000000006", "digits");
        write_ar_member_with_size_field(stream, "empty.txt/", "0         ", "");
    }
    const auto output = root / "output";
    const auto stats = flitzzip::extract_ar(archive, output, false);
    REQUIRE_EQ(stats.entries, 3U);
    REQUIRE_EQ(stats.output_bytes, 12U);
    REQUIRE_EQ(read_text_file(output / "outer.txt"), "outer!");
    REQUIRE_EQ(read_text_file(output / "zeroes.txt"), "digits");
    REQUIRE_TRUE(std::filesystem::is_regular_file(output / "empty.txt"));
    REQUIRE_EQ(std::filesystem::file_size(output / "empty.txt"), 0U);
}

// Purpose: Reject non-decimal bytes in BSD name lengths and GNU string-table offsets.
// Inputs: A valid first member followed by long-name metadata containing spaces, controls, or non-ASCII bytes.
// Outputs: Requires malformed-metadata rejection before any extraction directory is published.
TEST_CASE(ar_long_name_numeric_fields_require_ascii_digits) {
    const auto root = test_temp_dir("ar-malformed-long-name-numbers");
    const std::array<std::string, 4> invalid_names = {
        "#1/1 2",
        "#1/\t1",
        "/1 2",
        std::string("/") + "\xff",
    };
    for (std::size_t index = 0; index < invalid_names.size(); ++index) {
        const auto archive = root / (std::to_string(index) + ".ar");
        {
            std::ofstream stream(archive, std::ios::binary);
            stream.write("!<arch>\n", 8);
            write_ar_member(stream, "valid.txt/", "prefix");
            write_ar_member(stream, invalid_names[index], "abcdefghijkl");
        }
        require_ar_rejection_before_output(archive, root / ("output-" + std::to_string(index)), "malformed AR");
    }
}

// Purpose: Reject malformed AR framing and GNU/BSD name extents before any valid prefix can be published.
// Inputs: Independent headers and name tables, including empty, out-of-table and both overlong-name boundaries.
// Outputs: RAM scanning and filesystem extraction agree on actionable errors and leave all output absent.
TEST_CASE(ar_framing_and_long_name_extent_matrix_rejects_before_output) {
    const auto root = test_temp_dir("ar-name-extent-matrix");
    struct Case {
        std::string name;
        std::string table;
        std::string payload;
        std::string_view diagnostic;
    };
    const Case cases[] = {
        {"#1/", "", "abc", "malformed AR BSD name length"},
        {"#1/0", "", "abc", "AR BSD long-name size is invalid"},
        {"#1/4", "", "abc", "AR BSD long-name size is invalid"},
        {"#1/" + std::to_string(flitzzip::kMaxArchivePathBytes + 1U), "",
         std::string(flitzzip::kMaxArchivePathBytes + 1U, 'x'), "AR BSD long-name size is invalid"},
        {"/0", "", "abc", "AR long-name offset is outside the GNU string table"},
        {"/6", "ok.o/\n", "abc", "AR long-name offset is outside the GNU string table"},
        {"/99999999999999", "ok.o/\n", "abc", "AR long-name offset is outside the GNU string table"},
        {"/0", "/\n", "abc", "AR GNU long-name entry is empty"},
        {"/0", "\n", "abc", "AR GNU long-name entry is empty"},
        {"/0", std::string(flitzzip::kMaxArchivePathBytes + 1U, 'x'), "abc",
         "AR GNU long name exceeds FlitzZip path length limit"},
        {"/0", std::string(flitzzip::kMaxArchivePathBytes + 2U, 'x') + "/\n", "abc",
         "AR GNU long name exceeds FlitzZip path length limit"},
    };
    std::vector<std::pair<std::string, std::string_view>> fixtures;
    for (const auto& item : cases) {
        std::ostringstream output(std::ios::out | std::ios::binary);
        output << "!<arch>\n";
        write_ar_member(output, "valid.txt/", "prefix");
        write_ar_member(output, "//", item.table);
        write_ar_member(output, item.name, item.payload);
        fixtures.emplace_back(output.str(), item.diagnostic);
    }
    auto malformed_header = make_ar_header("last.txt/", 0);
    malformed_header[59] = 'x';
    fixtures.emplace_back("!<arch>\n" + std::string(malformed_header.data(), malformed_header.size()),
                          "malformed AR member terminator");
    fixtures.emplace_back("!<arcH>\n", "unsupported or malformed AR global header");
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        const auto& [bytes, diagnostic] = fixtures[index];
        std::istringstream input(bytes, std::ios::in | std::ios::binary);
        bool rejected = false;
        std::string actual_error;
        try {
            (void)flitzzip::detail::scan_ar_stream(input, bytes.size(), flitzzip::ArchivePathEncoding::Utf8);
        } catch (const flitzzip::ArchiveError& error) {
            actual_error = error.what();
            rejected = std::string_view(error.what()).find(diagnostic) != std::string_view::npos;
        }
        if (!rejected) {
            throw std::runtime_error("AR name-extent fixture " + std::to_string(index) + " expected " +
                                     std::string(diagnostic) + "; received " + actual_error);
        }
        const auto archive = root / (std::to_string(index) + ".ar");
        {
            std::ofstream output(archive, std::ios::binary);
            output.exceptions(std::ios::badbit | std::ios::failbit);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
        }
        require_ar_rejection_before_output(archive, root / ("output-" + std::to_string(index)), diagnostic);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify AR extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: An AR file entry and a preexisting output file of the same name.
// Outputs: Throws if extraction overwrites while `overwrite` is false.
TEST_CASE(ar_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("ar-overwrite");
    const auto archive = root / "overwrite.ar";
    write_one_bsd_ar_archive(archive, "file.txt", "new");
    const auto output = root / "out";
    std::filesystem::create_directories(output);
    std::ofstream(output / "file.txt") << "old";

    bool rejected = false;
    try {
        static_cast<void>(flitzzip::extract_ar(archive, output, false));
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "old");
}

namespace {

enum class ArPositionFailure { MemberOffset, PayloadOffset, PayloadSeek, PaddingSeek };

// Purpose: Fail one real AR stream positioning operation while preserving all ordinary reads and seeks.
// Inputs: Complete independently framed bytes and the selected first-member query/seek boundary.
// Outputs: A one-shot failed position, explicit observation evidence and the same reusable owned stream.
class ArPositionFailureBuffer final : public std::stringbuf {
  public:
    // Purpose: Preserve an ordinary seekable source. Inputs: Complete bytes and failure selection. Outputs: Armed copy.
    ArPositionFailureBuffer(const std::string& bytes, ArPositionFailure failure)
        : std::stringbuf(bytes, std::ios::in | std::ios::binary), failure_(failure) {}

    // Purpose: Restore valid external operations. Inputs: None. Outputs: Same bytes/position with failure disabled.
    void disarm() noexcept {
        armed_ = false;
    }

    // Purpose: Verify the parser reached the chosen external operation. Inputs: None. Outputs: One-shot evidence.
    bool observed() const noexcept {
        return observed_;
    }

  protected:
    // Purpose: Reject exactly one position query or relative seek, forwarding every other request unchanged.
    // Inputs: Standard stream offset, reference origin and mode. Outputs: Real position or failed-position sentinel.
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode mode) override {
        const auto current = std::stringbuf::seekoff(0, std::ios::cur, mode);
        if (armed_ && !observed_ && direction == std::ios::cur && (mode & std::ios::in) != 0) {
            bool query_failure = false;
            if (offset == 0 && current == pos_type(68)) {
                ++member_position_queries_;
                query_failure = (failure_ == ArPositionFailure::MemberOffset && member_position_queries_ == 1U) ||
                                (failure_ == ArPositionFailure::PayloadOffset && member_position_queries_ == 2U);
            }
            const bool seek_failure =
                (failure_ == ArPositionFailure::PayloadSeek && current == pos_type(68) && offset == 3) ||
                (failure_ == ArPositionFailure::PaddingSeek && current == pos_type(71) && offset == 1);
            if (query_failure || seek_failure) {
                observed_ = true;
                return pos_type(off_type(-1));
            }
        }
        return std::stringbuf::seekoff(offset, direction, mode);
    }

  private:
    ArPositionFailure failure_;
    unsigned member_position_queries_ = 0;
    bool armed_ = true;
    bool observed_ = false;
};

}  // namespace

// Purpose: Refuse incomplete AR publication plans when a real stream position or payload/padding seek fails.
// Inputs: Independent two-member bytes, four one-shot external operation refusals and the exact same reused stream.
// Outputs: Specific ArchiveError at each observed boundary, unchanged bytes and complete exact metadata after recovery.
TEST_CASE(ar_reader_position_failures_reject_incomplete_plans_and_allow_same_stream_recovery) {
    const auto [bytes, boundary] = ar_reader_boundary_fixture();
    REQUIRE_EQ(boundary, 72U);
    REQUIRE_EQ(bytes.size(), 138U);
    struct Case {
        ArPositionFailure failure;
        std::string_view diagnostic;
    };
    const std::array cases{
        Case{ArPositionFailure::MemberOffset, "failed to read AR member offset"},
        Case{ArPositionFailure::PayloadOffset, "failed to read AR payload offset"},
        Case{ArPositionFailure::PayloadSeek, "AR member payload is truncated"},
        Case{ArPositionFailure::PaddingSeek, "AR member padding is truncated"},
    };
    for (const auto& item : cases) {
        ArPositionFailureBuffer buffer(bytes, item.failure);
        std::istream input(&buffer);
        bool rejected = false;
        try {
            (void)flitzzip::detail::scan_ar_stream(input, bytes.size(), flitzzip::ArchivePathEncoding::Utf8);
        } catch (const flitzzip::ArchiveError& error) {
            REQUIRE_TRUE(std::string_view(error.what()).find(item.diagnostic) != std::string_view::npos);
            rejected = true;
        }
        REQUIRE_TRUE(buffer.observed());
        REQUIRE_TRUE(rejected);
        REQUIRE_EQ(buffer.str(), bytes);
        buffer.disarm();
        input.clear();
        input.seekg(0, std::ios::beg);
        REQUIRE_TRUE(input.good());
        const auto recovered =
            flitzzip::detail::scan_ar_stream(input, bytes.size(), flitzzip::ArchivePathEncoding::Utf8);
        REQUIRE_EQ(recovered.entries.size(), 2U);
        REQUIRE_EQ(recovered.total_file_bytes, 8U);
        REQUIRE_EQ(recovered.entries[0].path, "before.o");
        REQUIRE_EQ(recovered.entries[0].payload_offset, 68U);
        REQUIRE_EQ(recovered.entries[0].size, 3U);
        REQUIRE_EQ(recovered.entries[1].path, "after.o");
        REQUIRE_EQ(recovered.entries[1].payload_offset, 132U);
        REQUIRE_EQ(recovered.entries[1].size, 5U);
        REQUIRE_EQ(bytes.substr(recovered.entries[0].payload_offset, recovered.entries[0].size), "abc");
        REQUIRE_EQ(bytes.substr(recovered.entries[1].payload_offset, recovered.entries[1].size), "defgh");
        REQUIRE_EQ(input.tellg(), std::istream::pos_type(138));
    }
}
