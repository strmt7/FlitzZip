#include "test_compat_fixture.hpp"
#include "test_util.hpp"

#include "core/archive_format.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "xar/xar_adapter.hpp"

#include "miniz.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Purpose: Append one big-endian unsigned 16-bit field to a byte vector.
// Inputs: `bytes` receives output and `value` is the decoded integer.
// Outputs: Appends two bytes.
void append_be16(std::vector<unsigned char>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Append one big-endian unsigned 32-bit field to a byte vector.
// Inputs: `bytes` receives output and `value` is the decoded integer.
// Outputs: Appends four bytes.
void append_be32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<unsigned char>(value & 0xFFU));
}

// Purpose: Append one big-endian unsigned 64-bit field to a byte vector.
// Inputs: `bytes` receives output and `value` is the decoded integer.
// Outputs: Appends eight bytes.
void append_be64(std::vector<unsigned char>& bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<unsigned char>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
    }
}

// Purpose: Compress a small fixture buffer with the zlib wrapper used by XAR.
// Inputs: `text` is the source payload.
// Outputs: Returns a zlib-compressed byte buffer.
std::vector<unsigned char> zlib_compress(std::string_view text) {
    auto capacity = mz_compressBound(static_cast<mz_ulong>(text.size()));
    std::vector<unsigned char> output(static_cast<std::size_t>(capacity));
    auto output_size = capacity;
    const auto status = mz_compress2(output.data(), &output_size, reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<mz_ulong>(text.size()), MZ_BEST_COMPRESSION);
    REQUIRE_EQ(status, MZ_OK);
    output.resize(static_cast<std::size_t>(output_size));
    return output;
}

// Purpose: Write a complete XAR archive fixture.
// Inputs: `archive` is the destination, `toc` is uncompressed XML, and `heap` is the archive heap.
// Outputs: Creates or replaces the XAR file.
void write_xar_fixture(const std::filesystem::path& archive, const std::string& toc,
                       std::span<const unsigned char> heap, std::uint32_t checksum_algorithm = 0U) {
    const auto compressed_toc = zlib_compress(toc);
    std::vector<unsigned char> bytes;
    bytes.reserve(28U + compressed_toc.size() + heap.size());
    append_be32(bytes, 0x78617221U);
    append_be16(bytes, 28U);
    append_be16(bytes, 1U);
    append_be64(bytes, static_cast<std::uint64_t>(compressed_toc.size()));
    append_be64(bytes, static_cast<std::uint64_t>(toc.size()));
    append_be32(bytes, checksum_algorithm);
    bytes.insert(bytes.end(), compressed_toc.begin(), compressed_toc.end());
    bytes.insert(bytes.end(), heap.begin(), heap.end());

    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Purpose: Create a nested XAR fixture with one directory and one file.
// Inputs: `archive` is the destination and `payload` is the regular-file text.
// Outputs: Writes a zlib-compressed XAR with stored length and decoded size in their standard fields.
void write_nested_xar_fixture(const std::filesystem::path& archive, std::string_view payload) {
    const auto compressed_payload = zlib_compress(payload);
    const std::string toc = "<?xml version=\"1.0\"?>"
                            "<xar><toc>"
                            "<file id=\"1\"><name>subdir</name><type>directory</type>"
                            "<file id=\"2\"><name>hello.txt</name><type>file</type>"
                            "<data><length>" +
                            std::to_string(compressed_payload.size()) +
                            "</length>"
                            "<offset>0</offset>"
                            "<size>" +
                            std::to_string(payload.size()) +
                            "</size>"
                            "<encoding style=\"application/x-gzip\"/>"
                            "</data></file>"
                            "</file>"
                            "</toc></xar>";
    write_xar_fixture(archive, toc, compressed_payload);
}

// Purpose: Read a full text file for extraction assertions.
// Inputs: `path` is the extracted file path.
// Outputs: Returns its full binary text payload.
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Purpose: Count regular files below a directory for no-output assertions.
// Inputs: `root` is the directory to inspect.
// Outputs: Returns zero for missing roots or the number of regular files.
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

}  // namespace

namespace {

// Purpose: Build an independently specified one-byte stored XAR entry for parser boundary tests.
// Inputs: XML filename text and optional raw data fields. Outputs: A complete file element, not an archive.
std::string
stored_xar_entry(std::string_view name,
                 std::string_view data =
                     "<length>1</length><offset>0</offset><size>1</size><encoding style='application/octet-stream'/>") {
    return "<file><name>" + std::string(name) + "</name><type>file</type><data>" + std::string(data) + "</data></file>";
}

// Purpose: Require malformed metadata to fail before modifying an existing destination.
// Inputs: Bounded XML TOC, owned archive and destination paths. Outputs: Rejection and unchanged sentinel bytes.
void require_xar_metadata_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                   const std::string& toc) {
    const std::array<unsigned char, 1> heap{'z'};
    write_xar_fixture(archive, toc, heap);
    std::filesystem::create_directories(destination);
    const auto sentinel = destination / "existing.txt";
    {
        std::ofstream output(sentinel);
        output << "retain";
    }
    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, destination, false);
    } catch (const flitzzip::Error&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(destination), 1U);
    REQUIRE_EQ(read_text_file(sentinel), "retain");
}

}  // namespace

// Purpose: Reject malformed entities and forbidden Unicode scalars before any filesystem publication.
// Inputs: Empty, overflow, surrogate, out-of-range, unknown and unterminated XML references.
// Outputs: All invalid references reject while preserving existing files.
TEST_CASE(xar_invalid_xml_entities_do_not_publish) {
    const auto root = test_temp_dir("xar-entity-rejection");
    for (const auto name : {"&#x;", "&#xD800;", "&#xDFFF;", "&#x110000;", "&#4294967296;", "&#-1;", "&#12q;",
                            "&unknown;", "&amp", "&#0;", "&#47;", "&#92;"}) {
        require_xar_metadata_rejected(root / "input.xar", root / "output",
                                      "<xar><toc>" + stored_xar_entry(name) + "</toc></xar>");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Validate independent two-, three- and four-byte Unicode decoding and numeric XML field normalization.
// Inputs: Known scalar references and whitespace-wrapped decimal payload metadata. Outputs: Exact names and bytes.
TEST_CASE(xar_numeric_entities_preserve_unicode_scalar_widths) {
    const auto root = test_temp_dir("xar-entity-widths");
    const std::array<std::string_view, 3> names{"&#162;.txt", "&#x20AC;.txt", "&#x1F680;.txt"};
    const std::array<std::filesystem::path, 3> expected{u8"\u00a2.txt", u8"\u20ac.txt", u8"\U0001f680.txt"};
    const std::array<unsigned char, 1> heap{'z'};
    const std::string fields = "<length> 1 </length><offset>\t0\n</offset><size> 1 </size>"
                               "<encoding ignored='metadata' style = 'application/octet-stream'/>";
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto destination = root / std::to_string(index);
        write_xar_fixture(root / "input.xar", "<xar><toc>" + stored_xar_entry(names[index], fields) + "</toc></xar>",
                          heap);
        (void)flitzzip::extract_xar(root / "input.xar", destination, false);
        REQUIRE_EQ(count_regular_files(destination), 1U);
        REQUIRE_EQ(read_text_file(destination / expected[index]), "z");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Preserve significant filename whitespace independently from numeric/type field formatting.
// Inputs: Literal/entity leading spaces and XML punctuation in valid Windows names.
// Outputs: Exact names and bytes are restored without silently creating trimmed aliases.
TEST_CASE(xar_filename_text_preserves_whitespace_and_entities) {
    const auto root = test_temp_dir("xar-filename-text");
    const std::array<std::pair<std::string_view, std::string_view>, 4> names{
        {{" leading.txt", " leading.txt"},
         {"&#32;leading.txt", " leading.txt"},
         {"two words.txt", "two words.txt"},
         {"rock&apos;n&amp;roll.txt", "rock'n&roll.txt"}}};
    const std::array<unsigned char, 1> heap{'z'};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto destination = root / std::to_string(index);
        const auto& [xml_name, expected_name] = names[index];
        write_xar_fixture(root / "input.xar", "<xar><toc>" + stored_xar_entry(xml_name) + "</toc></xar>", heap);
        (void)flitzzip::extract_xar(root / "input.xar", destination, false);
        REQUIRE_EQ(count_regular_files(destination), 1U);
        REQUIRE_EQ(read_text_file(destination / expected_name), "z");
        REQUIRE_EQ(std::filesystem::directory_iterator(destination)->path().filename().string(), expected_name);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Apply the same Windows name policy to literal and XML-escaped whitespace and reserved characters.
// Inputs: Trailing spaces, control characters and punctuation that cannot name a safe Windows file.
// Outputs: Every representation rejects before altering the destination sentinel or creating a normalized alias.
TEST_CASE(xar_filename_whitespace_cannot_bypass_path_validation) {
    const auto root = test_temp_dir("xar-filename-policy");
    for (const auto name : {"unsafe.txt ", "unsafe.txt&#32;", "\tunsafe.txt", "unsafe.txt\t", "\nunsafe.txt",
                            "unsafe.txt\n", "\runsafe.txt", "unsafe.txt\r", "&#9;unsafe.txt", "unsafe.txt&#13;",
                            "&lt;unsafe.txt", "&gt;unsafe.txt", "&quot;unsafe.txt", " &#32; "}) {
        require_xar_metadata_rejected(root / "input.xar", root / "output",
                                      "<xar><toc>" + stored_xar_entry(name) + "</toc></xar>");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject ambiguous metadata and hostile byte ranges rather than selecting a convenient field value.
// Inputs: Duplicate/missing fields, decimal overflow, impossible heap extents and malformed encoding attributes.
// Outputs: Every invalid record fails without replacing a sentinel destination file.
TEST_CASE(xar_data_fields_are_unique_and_range_checked) {
    const auto root = test_temp_dir("xar-data-fields");
    const std::string base = "<length>1</length><offset>0</offset><size>1</size>";
    const std::array<std::string, 12> fields{
        base + "<offset>0</offset>",
        base + "<length>1</length>",
        base + "<size>1</size>",
        "<length>1</length><offset>0</offset>",
        "<length>1</length><offset>18446744073709551616</offset><size>1</size>",
        "<length>1</length><offset>18446744073709551615</offset><size>1</size>",
        "<length>1</length><offset>-1</offset><size>1</size>",
        "<length>1</length><offset>0x0</offset><size>1</size>",
        "<length>2</length><offset>0</offset><size>2</size>",
        "<length>1</length><offset>0</offset><size>2</size>",
        base + "<encoding style=unquoted/>",
        base + "<encoding style='application/octet-stream'/><encoding style='application/zlib'/>"};
    for (const auto& data : fields) {
        require_xar_metadata_rejected(root / "input.xar", root / "output",
                                      "<xar><toc>" + stored_xar_entry("payload", data) + "</toc></xar>");
    }
    std::filesystem::remove_all(root);
}

// Purpose: Reject structurally invalid TOCs before exposing any of their entries.
// Inputs: Truncated/mismatched tags, misplaced data, invalid file nesting and missing/duplicate identity metadata.
// Outputs: Every invalid tree fails without changing existing destination bytes.
TEST_CASE(xar_xml_structure_is_validated_before_extraction) {
    const auto root = test_temp_dir("xar-xml-structure");
    const std::array<std::string, 10> entries{"<file/>",
                                              "<data/>",
                                              "<file><name>a</name><type>directory</type></wrong>",
                                              "<file><name>a</name><type>directory</type>",
                                              "<file><name>a</name><name>b</name><type>directory</type></file>",
                                              "<file><name>a</name><type>directory</type><type>file</type></file>",
                                              "<file><type>directory</type></file>",
                                              "<file><name>a</name></file>",
                                              "<file><name>a</name><type>file</type>" + stored_xar_entry("child") +
                                                  "</file>",
                                              "<file><name>a</name><type>socket</type></file>"};
    for (const auto& entry : entries) {
        require_xar_metadata_rejected(root / "input.xar", root / "output", "<xar><toc>" + entry + "</toc></xar>");
    }
    require_xar_metadata_rejected(root / "input.xar", root / "output", "<xar><toc><file");
    std::filesystem::remove_all(root);
}

// Purpose: Exercise XML tokenizer boundaries independently from payload decompression and filesystem publication.
// Inputs: Empty/malformed tags, unterminated attributes, declarations, empty documents, oversized names and totals.
// Outputs: Rejects every hostile grammar/resource case while preserving the destination sentinel.
TEST_CASE(xar_xml_tokenizer_and_resource_boundaries_fail_closed) {
    const auto root = test_temp_dir("xar-xml-tokenizer-boundaries");
    const auto archive = root / "input.xar";
    const auto output = root / "output";
    const std::string prefix = "<xar><toc>";
    const std::string suffix = "</toc></xar>";
    const std::string data = "<length>1</length><offset>0</offset><size>1</size>";

    const std::vector<std::string> invalid{
        prefix + "</>" + suffix,
        prefix + "</ />" + suffix,
        prefix + "<>" + suffix,
        "<!DOCTYPE xar>" + prefix + stored_xar_entry("file.txt") + suffix,
        prefix + suffix,
        prefix + "</file>" + suffix,
        prefix + stored_xar_entry("a//b") + suffix,
        prefix + stored_xar_entry(std::string(flitzzip::kMaxArchivePathComponentBytes + 1U, 'a')) + suffix,
        prefix + stored_xar_entry("file.txt", data + "<encoding style='application/octet-stream/>") + suffix,
        prefix +
            stored_xar_entry("file.txt", "<length>1</length><offset>0</offset><size>" +
                                             std::to_string(flitzzip::kMaxPipelineMemoryBytes + 1U) + "</size>") +
            suffix,
    };
    for (const auto& toc : invalid) {
        require_xar_metadata_rejected(archive, output, toc);
    }

    std::string nested = prefix;
    constexpr std::size_t component_bytes = 1000U;
    const auto components = static_cast<std::size_t>(flitzzip::kMaxArchivePathBytes / component_bytes) + 2U;
    for (std::size_t index = 0; index < components; ++index) {
        nested += "<file><name>" + std::string(component_bytes, static_cast<char>('a' + (index % 26U))) +
                  "</name><type>directory</type>";
    }
    for (std::size_t index = 0; index < components; ++index) {
        nested += "</file>";
    }
    nested += suffix;
    require_xar_metadata_rejected(archive, output, nested);
    std::filesystem::remove_all(root);
}

// Purpose: Accept XML trailing whitespace and encoding attributes without a style while retaining exact bytes.
// Inputs: A complete stored entry followed by XML whitespace and an unrelated encoding attribute.
// Outputs: Exercises absent-attribute and terminal-text paths without relaxing document structure validation.
TEST_CASE(xar_terminal_whitespace_and_absent_encoding_style_extract) {
    const auto root = test_temp_dir("xar-terminal-text");
    const auto archive = root / "input.xar";
    const std::array<unsigned char, 1> heap{'z'};
    const auto data = "<length>1</length><offset>0</offset><size>1</size><encoding other='metadata'/>";
    write_xar_fixture(archive, "<xar><toc>" + stored_xar_entry("file.txt", data) + "</toc></xar> \r\n\t", heap);
    const auto stats = flitzzip::extract_xar(archive, root / "output", false);
    REQUIRE_EQ(stats.entries, 1U);
    REQUIRE_EQ(read_text_file(root / "output" / "file.txt"), "z");
    std::filesystem::remove_all(root);
}

// Purpose: Reject files/fields outside their canonical XAR owners and any non-whitespace bytes outside the root.
// Inputs: Wrong/duplicate roots, misplaced TOCs and file/data scalar fields, plus trailing character data.
// Outputs: Every structurally ambiguous document fails before changing the destination.
TEST_CASE(xar_document_hierarchy_rejects_ambiguous_metadata) {
    const auto root = test_temp_dir("xar-document-hierarchy");
    const auto entry = stored_xar_entry("file.txt");
    const std::vector<std::string> invalid{
        entry,
        "<toc>" + entry + "</toc>",
        "<xar>" + entry + "</xar>",
        "<xar><toc>" + entry + "</toc><toc></toc></xar>",
        "<xar><toc>" + entry + "</toc></xar><xar><toc></toc></xar>",
        "<xar><toc><meta>" + entry + "</meta></toc></xar>",
        "<xar><toc><name>file.txt</name>" + entry + "</toc></xar>",
        "<xar><toc><file><name>file.txt</name><type>file</type><meta><data/></meta></file></toc></xar>",
        "<xar><toc><file><name>file.txt</name><type>file</type><offset>0</offset></file></toc></xar>",
        "<xar><toc>" + entry + "</toc></xar>trailing",
    };
    for (const auto& toc : invalid) {
        require_xar_metadata_rejected(root / "input.xar", root / "output", toc);
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify native `.xar` extraction reads a nested zlib-compressed payload.
// Inputs: A handcrafted XAR archive with `subdir/hello.txt`.
// Outputs: Throws if detection, extraction stats, or payload bytes regress.
TEST_CASE(xar_extraction_reads_nested_zlib_payload) {
    const auto root = test_temp_dir("xar-basic");
    const auto archive = root / "sample.xar";
    write_nested_xar_fixture(archive, "hello xar\n");

    REQUIRE_EQ(flitzzip::detect_archive_format(archive), flitzzip::ArchiveFormat::Xar);
    const auto info = flitzzip::archive_format_info(flitzzip::ArchiveFormat::Xar);
    REQUIRE_TRUE(!info.can_create);
    REQUIRE_TRUE(info.can_extract);
    REQUIRE_TRUE(info.bundled_native);
    REQUIRE_TRUE(!info.gpu_native);

    const auto output = root / "out";
    const auto stats = flitzzip::extract_xar(archive, output, false);
    REQUIRE_EQ(stats.entries, static_cast<std::uint64_t>(2));
    REQUIRE_EQ(stats.output_bytes, static_cast<std::uint64_t>(std::string_view("hello xar\n").size()));
    REQUIRE_TRUE(!stats.gpu_used);
    REQUIRE_EQ(read_text_file(output / "subdir" / "hello.txt"), "hello xar\n");
    flitzzip_test::export_compat_fixture(archive, output);
}

// Purpose: Reject the formerly accepted, nonstandard reversal of XAR stored and decoded extents.
// Inputs: A valid zlib payload whose length and size metadata are deliberately reversed and materially different.
// Outputs: Requires ArchiveError before publishing files; never guesses an alternate metadata interpretation.
TEST_CASE(xar_extraction_rejects_reversed_payload_extents) {
    const auto root = test_temp_dir("xar-reversed-extents");
    const auto archive = root / "reversed.xar";
    const std::string payload(128U * 1024U, 'x');
    const auto compressed = zlib_compress(payload);
    REQUIRE_TRUE(compressed.size() < payload.size());
    const std::string toc = "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
                            "<data><length>" +
                            std::to_string(payload.size()) + "</length><offset>0</offset><size>" +
                            std::to_string(compressed.size()) +
                            "</size><encoding style=\"application/x-gzip\"/></data></file></toc></xar>";
    write_xar_fixture(archive, toc, compressed);
    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, root / "out", false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), 0U);
}

// Purpose: Preserve XML UTF-8 and character-reference names through extraction and replacement.
// Inputs: Stored and zlib fixtures mix raw UTF-8 with numeric and named XML entities.
// Outputs: Checks Unicode filesystem paths, exact content, and unchanged overwrite semantics.
TEST_CASE(xar_unicode_names_extract_and_overwrite) {
    const auto root = test_temp_dir("xar-unicode") / L"\u65e5\u672c";
    std::filesystem::create_directories(root);
    const std::string payload = "xar unicode payload\n";
    for (const bool compressed : {false, true}) {
        const auto archive = root / (compressed ? L"compressed-\u03a9.xar" : L"stored-\u03a9.xar");
        const auto output = root / (compressed ? L"compressed-\u00e9" : L"stored-\u00e9");
        const auto relative = std::filesystem::path(L"caf\u00e9") / L"\u65e5\u672c-\U0001f680&.txt";
        const auto heap =
            compressed ? zlib_compress(payload) : std::vector<unsigned char>(payload.begin(), payload.end());
        const std::string toc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><xar><toc>"
                                "<file id=\"1\"><name>caf\xC3\xA9</name><type>directory</type>"
                                "<file id=\"2\"><name>&#26085;&#x672C;-&#x1F680;&amp;.txt</name><type>file</type>"
                                "<data><length>" +
                                std::to_string(heap.size()) + "</length><offset>0</offset><size>" +
                                std::to_string(payload.size()) + "</size><encoding style=\"" +
                                (compressed ? "application/x-gzip" : "application/octet-stream") +
                                "\"/></data></file></file></toc></xar>";
        write_xar_fixture(archive, toc, heap);
        const auto stats = flitzzip::extract_xar(archive, output, false);
        REQUIRE_EQ(stats.entries, 2U);
        REQUIRE_EQ(stats.output_bytes, payload.size());
        REQUIRE_EQ(read_text_file(output / relative), payload);
        REQUIRE_EQ(count_regular_files(output), 1U);
        std::ofstream(output / relative, std::ios::binary | std::ios::trunc) << "old";
        bool refused = false;
        try {
            (void)flitzzip::extract_xar(archive, output, false);
        } catch (const flitzzip::SecurityError&) {
            refused = true;
        }
        REQUIRE_TRUE(refused);
        REQUIRE_EQ(read_text_file(output / relative), "old");
        (void)flitzzip::extract_xar(archive, output, true);
        REQUIRE_EQ(read_text_file(output / relative), payload);
        if (compressed) {
            flitzzip_test::export_compat_fixture(archive, output);
        }
    }
    std::filesystem::remove_all(root);
}

// Purpose: Verify XAR extraction refuses overwriting existing files unless explicitly allowed.
// Inputs: A nested XAR member and a preexisting destination file of the same name.
// Outputs: Throws `SecurityError` and preserves the original file.
TEST_CASE(xar_extraction_refuses_overwrite_by_default) {
    const auto root = test_temp_dir("xar-overwrite");
    const auto archive = root / "sample.xar";
    write_nested_xar_fixture(archive, "hello xar\n");
    const auto output = root / "out";
    std::filesystem::create_directories(output / "subdir");
    std::ofstream(output / "subdir" / "hello.txt") << "old";

    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, output, false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(read_text_file(output / "subdir" / "hello.txt"), "old");
}

// Purpose: Verify parent-directory XAR paths are rejected before output publication.
// Inputs: A XAR TOC containing `../escape.txt`.
// Outputs: Throws `SecurityError` and produces no extracted files.
TEST_CASE(xar_extraction_rejects_parent_directory_paths) {
    const auto root = test_temp_dir("xar-dotdot");
    const auto archive = root / "dotdot.xar";
    const std::string payload = "bad\n";
    const auto compressed_payload = zlib_compress(payload);
    const std::string toc = "<xar><toc><file id=\"1\"><name>../escape.txt</name><type>file</type>"
                            "<data><length>" +
                            std::to_string(compressed_payload.size()) +
                            "</length><offset>0</offset>"
                            "<size>" +
                            std::to_string(payload.size()) +
                            "</size>"
                            "<encoding style=\"application/x-gzip\"/></data></file></toc></xar>";
    write_xar_fixture(archive, toc, compressed_payload);

    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, root / "out", false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
    REQUIRE_TRUE(!std::filesystem::exists(root / "escape.txt"));
}

// Purpose: Verify XAR symbolic links are rejected instead of materialized.
// Inputs: A XAR TOC entry with type `symlink`.
// Outputs: Throws `SecurityError` and creates no output files.
TEST_CASE(xar_extraction_rejects_symbolic_links) {
    const auto root = test_temp_dir("xar-symlink");
    const auto archive = root / "symlink.xar";
    const std::string toc = "<xar><toc><file id=\"1\"><name>link.txt</name><type>symlink</type></file></toc></xar>";
    const std::array<unsigned char, 1> empty_heap{0};
    write_xar_fixture(archive, toc, std::span<const unsigned char>(empty_heap.data(), 0));

    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, root / "out", false);
    } catch (const flitzzip::SecurityError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify unsupported XAR heap encodings fail closed.
// Inputs: A XAR file entry declaring Bzip2 payload encoding.
// Outputs: Throws `ArchiveError` before output publication.
TEST_CASE(xar_extraction_rejects_unsupported_encoding) {
    const auto root = test_temp_dir("xar-unsupported-encoding");
    const auto archive = root / "unsupported.xar";
    const std::string payload = "plain";
    const std::string toc = "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
                            "<data><length>" +
                            std::to_string(payload.size()) +
                            "</length><offset>0</offset>"
                            "<size>" +
                            std::to_string(payload.size()) +
                            "</size>"
                            "<encoding style=\"application/x-bzip2\"/></data></file></toc></xar>";
    write_xar_fixture(
        archive, toc,
        std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()));

    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, root / "out", false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify XAR archives declaring TOC checksum modes fail closed until checksum verification is implemented.
// Inputs: A valid XAR body whose header declares SHA-1 TOC checksum metadata.
// Outputs: Throws `ArchiveError` before parsing or extracting archive entries.
TEST_CASE(xar_extraction_rejects_unverified_toc_checksum_algorithm) {
    const auto root = test_temp_dir("xar-toc-checksum");
    const auto archive = root / "checksum.xar";
    const std::string payload = "plain";
    const std::string toc = "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
                            "<data><length>" +
                            std::to_string(payload.size()) +
                            "</length><offset>0</offset>"
                            "<size>" +
                            std::to_string(payload.size()) + "</size></data></file></toc></xar>";
    write_xar_fixture(
        archive, toc,
        std::span<const unsigned char>(reinterpret_cast<const unsigned char*>(payload.data()), payload.size()), 1U);

    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, root / "out", false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

// Purpose: Verify corrupt compressed XAR payloads are rejected before publication.
// Inputs: A valid XAR fixture with one heap byte flipped.
// Outputs: Throws `ArchiveError` and creates no regular files.
TEST_CASE(xar_extraction_rejects_corrupt_zlib_payload_before_output) {
    const auto root = test_temp_dir("xar-corrupt-payload");
    const auto archive = root / "corrupt.xar";
    const std::string payload = "hello xar\n";
    auto compressed_payload = zlib_compress(payload);
    compressed_payload.back() ^= 0x01U;
    const std::string toc = "<xar><toc><file id=\"1\"><name>payload.txt</name><type>file</type>"
                            "<data><length>" +
                            std::to_string(compressed_payload.size()) +
                            "</length><offset>0</offset>"
                            "<size>" +
                            std::to_string(payload.size()) +
                            "</size>"
                            "<encoding style=\"application/x-gzip\"/></data></file></toc></xar>";
    write_xar_fixture(archive, toc, compressed_payload);

    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, root / "out", false);
    } catch (const flitzzip::ArchiveError&) {
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(count_regular_files(root / "out"), static_cast<std::uint64_t>(0));
}

namespace {

// Purpose: Write independently mutated XAR bytes without invoking its production encoder.
// Inputs: Small bounded byte string and test-owned path. Outputs: Closed file or an explicit I/O failure.
void write_xar_bytes(const std::filesystem::path& archive, std::string_view bytes) {
    std::ofstream output(archive, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}

// Purpose: Mutate an XAR binary-header field independently of the production parser.
// Inputs: Complete small fixture, offset, width and unsigned value. Outputs: Exact big-endian replacement.
void set_xar_field(std::string& bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    REQUIRE_TRUE(width > 0U && width <= 8U && offset + width <= bytes.size());
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + width - index - 1U] = static_cast<char>(value >> (8U * index));
    }
}

// Purpose: Verify corrupt XAR input fails before publication and extraction progress.
// Inputs: Malformed archive, owned destination and required diagnostic fragment.
// Outputs: Requires rejection, exact original content and no leaked extraction artifacts.
void require_xar_archive_rejected(const std::filesystem::path& archive, const std::filesystem::path& destination,
                                  std::string_view diagnostic) {
    std::filesystem::create_directories(destination);
    write_xar_bytes(destination / "file.txt", "retain");
    unsigned callbacks = 0;
    bool rejected = false;
    try {
        (void)flitzzip::extract_xar(archive, destination, true,
                                    [&](const flitzzip::ProgressSnapshot&) { ++callbacks; });
    } catch (const flitzzip::ArchiveError& error) {
        REQUIRE_TRUE(std::string_view(error.what()).find(diagnostic) != std::string_view::npos);
        rejected = true;
    }
    REQUIRE_TRUE(rejected);
    REQUIRE_EQ(callbacks, 0U);
    REQUIRE_EQ(read_text_file(destination / "file.txt"), "retain");
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(destination), std::filesystem::directory_iterator()),
               1);
}

}  // namespace

// Purpose: Exercise binary XAR framing independently of XML and decompression error handling.
// Inputs: A known-good stored entry and hostile header/TOC sizes, versions and checksum algorithms.
// Outputs: Precise admission errors without filesystem or callback effects, followed by exact valid readback.
TEST_CASE(xar_binary_header_failure_matrix_preserves_destination) {
    const auto root = test_temp_dir("xar-binary-header-matrix");
    const auto archive = root / "header.xar";
    const auto output = root / "out";
    const std::array<unsigned char, 1> heap{'z'};
    write_xar_fixture(archive, "<xar><toc>" + stored_xar_entry("file.txt") + "</toc></xar>", heap);
    const auto valid = read_text_file(archive);
    struct Mutation {
        std::size_t offset;
        std::size_t width;
        std::uint64_t value;
        const char* diagnostic;
    };
    const Mutation mutations[] = {
        {0, 4, 0, "magic"},
        {4, 2, 27, "header size"},
        {4, 2, 0xFFFFU, "header size"},
        {6, 2, 0, "version"},
        {6, 2, 2, "version"},
        {8, 8, 0, "TOC size"},
        {16, 8, 0, "TOC size"},
        {8, 8, 0xFFFFFFFFFFFFFFFFULL, "TOC size"},
        {16, 8, 0xFFFFFFFFFFFFFFFFULL, "TOC size"},
        {8, 8, static_cast<std::uint64_t>(valid.size()), "TOC extends"},
        {16, 8, 1, "inflate XAR TOC"},
        {24, 4, 0xFFFFFFFFU, "checksum algorithms"},
    };
    for (const auto& mutation : mutations) {
        auto bytes = valid;
        set_xar_field(bytes, mutation.offset, mutation.width, mutation.value);
        write_xar_bytes(archive, bytes);
        require_xar_archive_rejected(archive, output, mutation.diagnostic);
    }
    write_xar_bytes(archive, valid);
    const auto stats = flitzzip::extract_xar(archive, output, true);
    REQUIRE_EQ(stats.output_bytes, 1U);
    REQUIRE_EQ(read_text_file(output / "file.txt"), "z");
}

// Purpose: Verify every strict prefix of a complete compressed XAR fails its two-pass admission.
// Inputs: An independently framed nested entry with a zlib payload and all byte truncation positions.
// Outputs: No truncated prefix starts extraction, changes a sentinel or leaves temporary artifacts.
TEST_CASE(xar_all_truncation_boundaries_preserve_destination) {
    const auto root = test_temp_dir("xar-all-truncations");
    const auto archive = root / "truncated.xar";
    write_nested_xar_fixture(archive, "xar payload");
    const auto valid = read_text_file(archive);
    for (std::size_t length = 0; length < valid.size(); ++length) {
        write_xar_bytes(archive, std::string_view(valid).substr(0, length));
        require_xar_archive_rejected(archive, root / "out", "");
    }
    write_xar_bytes(archive, valid);
    const auto stats = flitzzip::extract_xar(archive, root / "out", true);
    REQUIRE_EQ(stats.output_bytes, 11U);
    REQUIRE_EQ(read_text_file(root / "out" / "subdir" / "hello.txt"), "xar payload");
}

// Purpose: Distinguish complete zlib streams from truncated, trailing and incorrect decoded-size payloads.
// Inputs: One valid compressed payload, independent XML sizes, and corruptions inside admitted heap extents.
// Outputs: Validation rejects each malformed stream before publication and accepts the exact original.
TEST_CASE(xar_payload_validation_rejects_inexact_streams_before_output) {
    const auto root = test_temp_dir("xar-payload-inexact-streams");
    const auto archive = root / "payload.xar";
    const auto output = root / "out";
    const std::string payload = "a payload with independently known length";
    const auto valid = zlib_compress(payload);
    for (unsigned scenario = 0; scenario < 5U; ++scenario) {
        auto heap = valid;
        std::uint64_t decoded_size = payload.size();
        if (scenario == 0U) {
            heap.pop_back();
        } else if (scenario == 1U) {
            heap.push_back(0U);
        } else if (scenario == 2U) {
            --decoded_size;
        } else if (scenario == 3U) {
            ++decoded_size;
        }
        const auto data = "<length>" + std::to_string(heap.size()) + "</length><offset>0</offset><size>" +
                          std::to_string(decoded_size) + "</size><encoding style='application/x-gzip'/>";
        write_xar_fixture(archive, "<xar><toc>" + stored_xar_entry("file.txt", data) + "</toc></xar>", heap);
        if (scenario < 4U) {
            require_xar_archive_rejected(archive, output, "");
        } else {
            const auto stats = flitzzip::extract_xar(archive, output, true);
            REQUIRE_EQ(stats.output_bytes, payload.size());
            REQUIRE_EQ(read_text_file(output / "file.txt"), payload);
        }
    }
}
