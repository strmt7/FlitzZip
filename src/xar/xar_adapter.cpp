#include "xar/xar_adapter.hpp"

#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/path_text.hpp"
#include "core/resource_limit_checks.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include "miniz.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace superzip {
namespace {

constexpr std::uint32_t kXarMagic = 0x78617221U;
constexpr std::uint16_t kXarHeaderBytes = 28U;
constexpr std::uint16_t kXarVersion = 1U;
constexpr std::size_t kXarBufferBytes = 64U * 1024U;
constexpr std::uint64_t kMaxXarTocBytes = kMaxArchiveIndexBytes;
constexpr std::uint64_t kMaxXarTotalFileBytes = kMaxPipelineMemoryBytes;
constexpr std::size_t kMaxXarXmlTagBytes = 1024U;
constexpr std::size_t kMaxXarScalarTextBytes = 256U;
constexpr std::uint32_t kMaxXarXmlDepth = kMaxArchivePathComponents + 32U;
constexpr std::uint64_t kMaxXarXmlElements = static_cast<std::uint64_t>(kMaxArchiveEntries) * 32U;

static_assert(kMaxXarTocBytes <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()));
static_assert(kMaxXarTocBytes <= static_cast<std::uint64_t>(std::numeric_limits<mz_ulong>::max()));

enum class XarEncoding {
    Stored,
    Zlib,
};

struct XarHeader {
    std::uint16_t header_size = 0;
    std::uint16_t version = 0;
    std::uint64_t toc_compressed_size = 0;
    std::uint64_t toc_uncompressed_size = 0;
    std::uint32_t checksum_algorithm = 0;
    std::uint64_t heap_offset = 0;
};

struct XarEntry {
    std::string path;
    bool directory = false;
    std::uint64_t offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t size = 0;
    XarEncoding encoding = XarEncoding::Stored;
};

struct XarMetadata {
    XarHeader header;
    std::vector<XarEntry> entries;
    std::uint64_t total_file_bytes = 0;
    std::uint64_t path_metadata_bytes = 0;
};

struct XarFileContext {
    std::string parent_path;
    std::string name;
    std::string path;
    std::string type;
    bool has_name = false;
    bool has_type = false;
    bool in_data = false;
    bool has_offset = false;
    bool has_size = false;
    bool has_length = false;
    std::uint64_t offset = 0;
    std::uint64_t compressed_size = 0;
    std::uint64_t size = 0;
    std::string encoding_style;
    bool has_encoding_style = false;
};

// Purpose: Return true for XML whitespace characters used in XAR TOCs.
// Inputs: `ch` is a candidate character.
// Outputs: Returns whether the character is ASCII XML whitespace.
bool is_ascii_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

// Purpose: Trim ASCII whitespace around XML text nodes and attribute values.
// Inputs: `text` is a raw XML substring.
// Outputs: Returns a view without leading or trailing ASCII whitespace.
std::string_view trim_ascii(std::string_view text) {
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Purpose: Append one Unicode scalar as UTF-8.
// Inputs: `output` receives bytes and `codepoint` is a decoded XML numeric entity.
// Outputs: Appends UTF-8 bytes or throws when the scalar is invalid.
void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
        throw SecurityError("XAR TOC contains a surrogate XML entity");
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        throw SecurityError("XAR TOC contains an invalid XML entity");
    }
}

// Purpose: Parse an XML numeric entity body.
// Inputs: `entity` excludes leading `&` and trailing `;`.
// Outputs: Returns the decoded scalar value or throws on malformed input.
std::uint32_t parse_xml_numeric_entity(std::string_view entity) {
    if (entity.size() < 2U || entity.front() != '#') {
        throw SecurityError("XAR TOC contains an unsupported XML entity");
    }
    int base = 10;
    entity.remove_prefix(1);
    if (!entity.empty() && (entity.front() == 'x' || entity.front() == 'X')) {
        base = 16;
        entity.remove_prefix(1);
    }
    if (entity.empty()) {
        throw SecurityError("XAR TOC contains an empty XML numeric entity");
    }
    std::uint32_t value = 0;
    const auto* first = entity.data();
    const auto* last = entity.data() + entity.size();
    const auto result = std::from_chars(first, last, value, base);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw SecurityError("XAR TOC contains a malformed XML numeric entity");
    }
    return value;
}

// Purpose: Decode XML text for the limited XAR TOC parser.
// Inputs: `text` is trimmed or untrimmed XML character data.
// Outputs: Returns decoded UTF-8 text; rejects unsupported entities.
std::string decode_xml_text(std::string_view text) {
    text = trim_ascii(text);
    std::string output;
    output.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] != '&') {
            output.push_back(text[index]);
            ++index;
            continue;
        }
        const auto semicolon = text.find(';', index + 1U);
        if (semicolon == std::string_view::npos) {
            throw SecurityError("XAR TOC contains an unterminated XML entity");
        }
        const auto entity = text.substr(index + 1U, semicolon - index - 1U);
        if (entity == "amp") {
            output.push_back('&');
        } else if (entity == "lt") {
            output.push_back('<');
        } else if (entity == "gt") {
            output.push_back('>');
        } else if (entity == "quot") {
            output.push_back('"');
        } else if (entity == "apos") {
            output.push_back('\'');
        } else {
            append_utf8(output, parse_xml_numeric_entity(entity));
        }
        index = semicolon + 1U;
    }
    return output;
}

// Purpose: Parse an unsigned decimal integer from XAR XML text.
// Inputs: `text` is the raw field value and `field` names diagnostics.
// Outputs: Returns the decoded integer or throws on malformed/overflowing input.
std::uint64_t parse_decimal_u64(std::string_view text, const char* field) {
    const auto decoded = decode_xml_text(text);
    if (decoded.empty() || !std::ranges::all_of(decoded, [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
        throw ArchiveError(std::string("XAR ") + field + " is not a decimal integer");
    }
    std::uint64_t value = 0;
    const auto* first = decoded.data();
    const auto* last = decoded.data() + decoded.size();
    const auto result = std::from_chars(first, last, value, 10);
    if (result.ec != std::errc{} || result.ptr != last) {
        throw ArchiveError(std::string("XAR ") + field + " overflows");
    }
    return value;
}

// Purpose: Decode a big-endian unsigned 16-bit field.
// Inputs: `bytes` points to at least two bytes.
// Outputs: Returns the decoded integer.
std::uint16_t read_be16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

// Purpose: Decode a big-endian unsigned 32-bit field.
// Inputs: `bytes` points to at least four bytes.
// Outputs: Returns the decoded integer.
std::uint32_t read_be32(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) | (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

// Purpose: Decode a big-endian unsigned 64-bit field.
// Inputs: `bytes` points to at least eight bytes.
// Outputs: Returns the decoded integer.
std::uint64_t read_be64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
    }
    return value;
}

// Purpose: Convert an unsigned offset to a stream offset after bounds checking.
// Inputs: `value` is a byte offset and `context` identifies the caller.
// Outputs: Returns a signed stream offset or throws when the host cannot represent it.
std::streamoff to_streamoff(std::uint64_t value, const char* context) {
    const auto max_streamoff = static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max());
    if (value > max_streamoff) {
        throw ArchiveError(std::string("XAR ") + context + " offset exceeds host stream limits");
    }
    return static_cast<std::streamoff>(value);
}

// Purpose: Add two byte counts while rejecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counts; `context` identifies diagnostics.
// Outputs: Returns the sum or throws before overflow.
std::uint64_t checked_add_u64(std::uint64_t lhs, std::uint64_t rhs, const char* context) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(std::string("XAR ") + context + " overflows");
    }
    return lhs + rhs;
}

// Purpose: Read a bounded byte range from a stream.
// Inputs: `input` is open, `offset`/`size` define the range, and `context` names diagnostics.
// Outputs: Returns exactly `size` bytes or throws on seek/read/resource errors.
std::vector<unsigned char> read_range(std::ifstream& input, std::uint64_t offset, std::uint64_t size,
                                      const char* context) {
    if (size > kMaxXarTocBytes) {
        throw ArchiveError(std::string("XAR ") + context + " exceeds resource limits");
    }
    input.clear();
    input.seekg(to_streamoff(offset, context), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek XAR ") + context);
    }
    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (static_cast<std::size_t>(input.gcount()) != data.size()) {
            throw ArchiveError(std::string("XAR ") + context + " is truncated");
        }
    }
    return data;
}

// Purpose: Inflate a bounded zlib buffer to an exact size.
// Inputs: `compressed` is the zlib stream, `expected_size` is the required output size, and `context` names
// diagnostics. Outputs: Returns exactly `expected_size` bytes or throws on decompression/resource errors.
std::vector<unsigned char> inflate_zlib_buffer(std::span<const unsigned char> compressed, std::uint64_t expected_size,
                                               const char* context) {
    if (expected_size == 0U || expected_size > kMaxXarTocBytes) {
        throw ArchiveError(std::string("XAR ") + context + " uncompressed size exceeds resource limits");
    }
    std::vector<unsigned char> output(static_cast<std::size_t>(expected_size));
    auto output_size = static_cast<mz_ulong>(output.size());
    auto input_size = static_cast<mz_ulong>(compressed.size());
    const int status = mz_uncompress2(output.data(), &output_size, compressed.data(), &input_size);
    if (status != MZ_OK || output_size != static_cast<mz_ulong>(output.size()) ||
        input_size != static_cast<mz_ulong>(compressed.size())) {
        throw ArchiveError(std::string("failed to inflate XAR ") + context);
    }
    return output;
}

// Purpose: Read and validate the fixed XAR binary header.
// Inputs: `input` is open and `archive_size` is the full file size.
// Outputs: Returns decoded header fields or throws on malformed metadata.
XarHeader read_xar_header(std::ifstream& input, std::uint64_t archive_size) {
    if (archive_size < kXarHeaderBytes) {
        throw ArchiveError("XAR archive is too small");
    }
    std::array<unsigned char, kXarHeaderBytes> bytes{};
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        throw ArchiveError("failed to read XAR header");
    }
    if (read_be32(bytes.data()) != kXarMagic) {
        throw ArchiveError("invalid XAR magic");
    }
    XarHeader header;
    header.header_size = read_be16(bytes.data() + 4U);
    header.version = read_be16(bytes.data() + 6U);
    header.toc_compressed_size = read_be64(bytes.data() + 8U);
    header.toc_uncompressed_size = read_be64(bytes.data() + 16U);
    header.checksum_algorithm = read_be32(bytes.data() + 24U);
    if (header.header_size < kXarHeaderBytes || header.header_size > archive_size) {
        throw ArchiveError("XAR header size is invalid");
    }
    if (header.version != kXarVersion) {
        throw ArchiveError("unsupported XAR version");
    }
    if (header.toc_compressed_size == 0U || header.toc_uncompressed_size == 0U ||
        header.toc_compressed_size > kMaxXarTocBytes || header.toc_uncompressed_size > kMaxXarTocBytes) {
        throw ArchiveError("XAR TOC size exceeds resource limits");
    }
    header.heap_offset = checked_add_u64(header.header_size, header.toc_compressed_size, "TOC end");
    if (header.heap_offset > archive_size) {
        throw ArchiveError("XAR TOC extends beyond archive size");
    }
    if (header.checksum_algorithm != 0U) {
        throw ArchiveError("XAR TOC checksum algorithms are not yet supported");
    }
    return header;
}

// Purpose: Extract the element name from an XML tag body.
// Inputs: `tag` is the text between `<` and `>`.
// Outputs: Returns the tag name without slash/attributes, or throws on malformed input.
std::string xml_tag_name(std::string_view tag) {
    tag = trim_ascii(tag);
    if (!tag.empty() && tag.front() == '/') {
        tag.remove_prefix(1);
        tag = trim_ascii(tag);
    }
    if (tag.empty()) {
        throw ArchiveError("XAR TOC contains an empty XML tag");
    }
    std::size_t end = 0;
    while (end < tag.size() && !is_ascii_space(tag[end]) && tag[end] != '/') {
        ++end;
    }
    if (end == 0U) {
        throw ArchiveError("XAR TOC contains a malformed XML tag");
    }
    return std::string(tag.substr(0, end));
}

// Purpose: Read one XML attribute value from a tag body.
// Inputs: `tag` is the text between `<` and `>`, and `name` is the attribute to read.
// Outputs: Returns a decoded attribute value, or an empty string when absent.
std::string xml_attribute(std::string_view tag, std::string_view name) {
    std::size_t index = 0;
    while (index < tag.size()) {
        while (index < tag.size() && (is_ascii_space(tag[index]) || tag[index] == '/')) {
            ++index;
        }
        const auto name_start = index;
        while (index < tag.size() && !is_ascii_space(tag[index]) && tag[index] != '=' && tag[index] != '/') {
            ++index;
        }
        const auto attr_name = tag.substr(name_start, index - name_start);
        while (index < tag.size() && is_ascii_space(tag[index])) {
            ++index;
        }
        if (index >= tag.size() || tag[index] != '=') {
            continue;
        }
        ++index;
        while (index < tag.size() && is_ascii_space(tag[index])) {
            ++index;
        }
        if (index >= tag.size() || (tag[index] != '"' && tag[index] != '\'')) {
            throw ArchiveError("XAR TOC contains a malformed XML attribute");
        }
        const char quote = tag[index++];
        const auto value_start = index;
        while (index < tag.size() && tag[index] != quote) {
            ++index;
        }
        if (index >= tag.size()) {
            throw ArchiveError("XAR TOC contains an unterminated XML attribute");
        }
        const auto value = tag.substr(value_start, index - value_start);
        ++index;
        if (attr_name == name) {
            return decode_xml_text(value);
        }
    }
    return {};
}

// Purpose: Convert XAR's encoding style string to an implemented decoder.
// Inputs: `style` is the decoded `encoding` style attribute.
// Outputs: Returns the internal encoding mode, or throws on unsupported algorithms.
XarEncoding xar_encoding_from_style(std::string_view style) {
    if (style.empty() || style == "application/octet-stream") {
        return XarEncoding::Stored;
    }
    if (style == "application/x-gzip" || style == "application/zlib" || style == "application/x-zlib") {
        return XarEncoding::Zlib;
    }
    throw ArchiveError("unsupported XAR payload encoding: " + std::string(style));
}

// Purpose: Decode and retain one bounded XAR filename while incrementally constructing its full path.
// Inputs: `context` is the active file, `text` is raw XML name content, and `metadata` owns the aggregate path budget.
// Outputs: Sets the context name/path once or throws before oversized, nested, or duplicate metadata is allocated.
void set_xar_file_name(XarFileContext& context, std::string_view text, XarMetadata& metadata) {
    if (context.has_name) {
        throw ArchiveError("XAR file entry contains duplicate name metadata");
    }
    if (trim_ascii(text).size() > kMaxArchivePathComponentBytes) {
        throw ArchiveError("XAR filename exceeds SuperZip component length limit");
    }
    auto name = decode_xml_text(text);
    if (name.empty() || name.find_first_of("/\\") != std::string::npos) {
        throw SecurityError("XAR filename is empty or contains a path separator");
    }
    const auto normalized_name = normalize_archive_path_key(name);
    if (normalized_name != name) {
        throw SecurityError("XAR filename is not a single canonical path component");
    }
    const auto separator_bytes = context.parent_path.empty() ? 0U : 1U;
    const auto required_bytes = name.size() + separator_bytes;
    if (required_bytes > kMaxArchivePathBytes || context.parent_path.size() > kMaxArchivePathBytes - required_bytes) {
        throw ArchiveError("XAR path exceeds SuperZip path length limit");
    }
    std::string path = context.parent_path;
    if (!path.empty()) {
        path.push_back('/');
    }
    path.append(name);
    metadata.path_metadata_bytes = checked_add_archive_path_metadata_bytes(
        metadata.path_metadata_bytes, static_cast<std::uint64_t>(name.size()) + path.size(),
        "XAR active path metadata");
    context.name = std::move(name);
    context.path = std::move(path);
    context.has_name = true;
}

// Purpose: Add an entry's decoded size to the archive total with resource limits.
// Inputs: `total` is the running byte count and `size` is the next file size.
// Outputs: Returns the updated total or throws on overflow/resource exhaustion.
std::uint64_t checked_add_xar_bytes(std::uint64_t total, std::uint64_t size) {
    total = checked_add_u64(total, size, "decoded size total");
    if (total > kMaxXarTotalFileBytes) {
        throw ArchiveError("XAR decoded payload exceeds SuperZip resource limit");
    }
    return total;
}

// Purpose: Finalize the current XAR file context into product metadata.
// Inputs: `stack` contains active contexts and `metadata` receives finished entries.
// Outputs: Appends one directory/file entry or throws on unsupported metadata.
void finalize_xar_file(std::vector<XarFileContext>& stack, XarMetadata& metadata) {
    if (stack.empty()) {
        throw ArchiveError("XAR TOC closes a file entry without opening one");
    }
    if (metadata.entries.size() >= kMaxArchiveEntries) {
        throw ArchiveError("XAR archive contains too many entries");
    }
    const auto& current = stack.back();
    if (!current.has_name || current.path.empty()) {
        throw ArchiveError("XAR file entry is missing a name");
    }
    const auto path = normalize_archive_path_key(current.path);
    metadata.path_metadata_bytes = checked_add_archive_path_metadata_bytes(metadata.path_metadata_bytes, path.size(),
                                                                           "XAR retained entry path metadata");
    if (current.type == "directory") {
        metadata.entries.push_back(XarEntry{.path = path, .directory = true});
    } else if (current.type == "file") {
        if (!current.has_offset || !current.has_size || !current.has_length) {
            throw ArchiveError("XAR file entry is missing data offset, size, or length");
        }
        metadata.total_file_bytes = checked_add_xar_bytes(metadata.total_file_bytes, current.size);
        metadata.entries.push_back(XarEntry{
            .path = path,
            .directory = false,
            .offset = current.offset,
            .compressed_size = current.compressed_size,
            .size = current.size,
            .encoding = xar_encoding_from_style(current.encoding_style),
        });
    } else if (current.type == "symlink" || current.type == "hardlink") {
        throw SecurityError("XAR symbolic and hard links are not supported");
    } else if (!current.has_type || current.type.empty()) {
        throw ArchiveError("XAR file entry is missing a type");
    } else {
        throw SecurityError("unsupported XAR file entry type: " + current.type);
    }
    stack.pop_back();
}

// Purpose: Apply decoded XML text to the active XAR file context.
// Inputs: `tag` is the current element name, `text` is raw XML character data, `stack` contains active files, and
// `metadata` owns aggregate path accounting.
// Outputs: Mutates the current context for supported fields.
void apply_xar_text(std::string_view tag, std::string_view text, std::vector<XarFileContext>& stack,
                    XarMetadata& metadata) {
    if (stack.empty()) {
        return;
    }
    auto& current = stack.back();
    if (!current.in_data && tag == "name") {
        set_xar_file_name(current, text, metadata);
    } else if (!current.in_data && tag == "type") {
        if (current.has_type || trim_ascii(text).size() > kMaxXarScalarTextBytes) {
            throw ArchiveError("XAR file entry contains duplicate or oversized type metadata");
        }
        current.type = decode_xml_text(text);
        current.has_type = true;
    } else if (current.in_data && tag == "offset") {
        if (current.has_offset || trim_ascii(text).size() > kMaxXarScalarTextBytes) {
            throw ArchiveError("XAR payload offset metadata is duplicate or oversized");
        }
        current.offset = parse_decimal_u64(text, "payload offset");
        current.has_offset = true;
    } else if (current.in_data && tag == "size") {
        if (current.has_size || trim_ascii(text).size() > kMaxXarScalarTextBytes) {
            throw ArchiveError("XAR payload size metadata is duplicate or oversized");
        }
        current.compressed_size = parse_decimal_u64(text, "payload encoded size");
        current.has_size = true;
    } else if (current.in_data && tag == "length") {
        if (current.has_length || trim_ascii(text).size() > kMaxXarScalarTextBytes) {
            throw ArchiveError("XAR payload length metadata is duplicate or oversized");
        }
        current.size = parse_decimal_u64(text, "payload decoded length");
        current.has_length = true;
    }
}

// Purpose: Validate all XAR file payload byte ranges.
// Inputs: `metadata` contains parsed entries and `archive_size` is the full file size.
// Outputs: Throws when any declared heap range is impossible or resource-exhaustive.
void validate_xar_ranges(const XarMetadata& metadata, std::uint64_t archive_size) {
    for (const auto& entry : metadata.entries) {
        if (entry.directory) {
            continue;
        }
        const auto start = checked_add_u64(metadata.header.heap_offset, entry.offset, "payload offset");
        const auto end = checked_add_u64(start, entry.compressed_size, "payload end");
        if (end > archive_size) {
            throw ArchiveError("XAR payload range extends beyond archive size: " + entry.path);
        }
        if (entry.encoding == XarEncoding::Stored && entry.compressed_size != entry.size) {
            throw ArchiveError("XAR stored payload size does not match decoded length: " + entry.path);
        }
    }
}

struct XarTocParseState {
    XarMetadata metadata;
    std::vector<XarFileContext> file_stack;
    std::vector<std::string> tag_stack;
    std::uint64_t tag_stack_bytes = 0;
    std::uint64_t element_count = 0;
};

// Purpose: Apply one bounded XML text interval to the active XAR element.
// Inputs: `xml`, `start`, and `end` delimit text; `state` owns active tag/file context.
// Outputs: Updates active metadata or returns without effect when no element can consume text.
void consume_xar_toc_text(std::string_view xml, std::size_t start, std::size_t end, XarTocParseState& state) {
    if (end > start && !state.tag_stack.empty()) {
        apply_xar_text(state.tag_stack.back(), xml.substr(start, end - start), state.file_stack, state.metadata);
    }
}

// Purpose: Close one validated XML element and its XAR file/data state.
// Inputs: `name` is the canonical closing tag name and `state` owns active stacks.
// Outputs: Pops matching state or throws on mismatched or context-invalid XML.
void close_xar_toc_tag(const std::string& name, XarTocParseState& state) {
    if (state.tag_stack.empty() || state.tag_stack.back() != name) {
        throw ArchiveError("XAR TOC contains mismatched XML tags");
    }
    if (name == "file") {
        finalize_xar_file(state.file_stack, state.metadata);
    } else if (name == "data") {
        if (state.file_stack.empty()) {
            throw ArchiveError("XAR TOC closes data outside a file entry");
        }
        state.file_stack.back().in_data = false;
    }
    state.tag_stack_bytes -= state.tag_stack.back().size();
    state.tag_stack.pop_back();
}

// Purpose: Open one validated XML element and update active XAR file/data state.
// Inputs: `tag` is raw bounded tag text, `name` is canonical, `self_closing` is syntax state, and `state` is mutable.
// Outputs: Pushes/finalizes bounded context or throws before invalid nesting or duplicate metadata is retained.
void open_xar_toc_tag(std::string_view tag, const std::string& name, bool self_closing, XarTocParseState& state) {
    if (name == "file") {
        if (state.file_stack.size() >= kMaxArchivePathComponents ||
            state.metadata.entries.size() >= kMaxArchiveEntries) {
            throw ArchiveError("XAR file nesting or entry count exceeds SuperZip limits");
        }
        XarFileContext context;
        if (!state.file_stack.empty()) {
            const auto& parent = state.file_stack.back();
            if (!parent.has_name || parent.path.empty() || !parent.has_type || parent.type != "directory") {
                throw ArchiveError("XAR file entry is nested below incomplete or non-directory metadata");
            }
            state.metadata.path_metadata_bytes = checked_add_archive_path_metadata_bytes(
                state.metadata.path_metadata_bytes, parent.path.size(), "XAR nested parent path metadata");
            context.parent_path = parent.path;
        }
        state.file_stack.push_back(std::move(context));
    } else if (name == "data") {
        if (state.file_stack.empty()) {
            throw ArchiveError("XAR TOC opens data outside a file entry");
        }
        state.file_stack.back().in_data = !self_closing;
    } else if (name == "encoding" && !state.file_stack.empty() && state.file_stack.back().in_data) {
        const auto style = xml_attribute(tag, "style");
        if (!style.empty()) {
            if (style.size() > kMaxXarScalarTextBytes || state.file_stack.back().has_encoding_style) {
                throw ArchiveError("XAR payload encoding metadata is duplicate or oversized");
            }
            state.file_stack.back().encoding_style = style;
            state.file_stack.back().has_encoding_style = true;
        }
    }
    if (!self_closing) {
        if (state.tag_stack.size() >= kMaxXarXmlDepth) {
            throw ArchiveError("XAR XML nesting exceeds SuperZip limits");
        }
        state.tag_stack_bytes =
            checked_add_archive_path_metadata_bytes(state.tag_stack_bytes, name.size(), "XAR active XML tag metadata");
        state.tag_stack.push_back(name);
    } else if (name == "file") {
        finalize_xar_file(state.file_stack, state.metadata);
    }
}

// Purpose: Apply final aggregate path validation to parsed XAR entries.
// Inputs: `metadata` owns all complete entry paths and the running metadata-byte budget.
// Outputs: Updates validation-work accounting and throws on duplicates, conflicts, or unsafe paths.
void validate_xar_toc_paths(XarMetadata& metadata) {
    std::vector<ArchivePathValidationEntry> validation_entries;
    validation_entries.reserve(metadata.entries.size());
    for (const auto& entry : metadata.entries) {
        metadata.path_metadata_bytes = checked_add_archive_path_metadata_bytes(
            metadata.path_metadata_bytes, entry.path.size(), "XAR path validation metadata");
        validation_entries.push_back({entry.path, entry.directory});
    }
    validate_archive_path_set(validation_entries);
}

// Purpose: Parse the decompressed XAR XML TOC into archive metadata.
// Inputs: `toc_bytes` contains UTF-8 XML and `header` contains binary TOC bounds.
// Outputs: Returns validated entry metadata or throws on unsupported TOC constructs.
XarMetadata parse_xar_toc(std::span<const unsigned char> toc_bytes, const XarHeader& header) {
    const std::string_view xml(reinterpret_cast<const char*>(toc_bytes.data()), toc_bytes.size());
    XarTocParseState state;
    state.metadata.header = header;
    std::size_t position = 0;
    std::size_t text_start = 0;
    while (position < xml.size()) {
        const auto open = xml.find('<', position);
        if (open == std::string_view::npos) {
            consume_xar_toc_text(xml, text_start, xml.size(), state);
            break;
        }
        consume_xar_toc_text(xml, text_start, open, state);
        const auto close = xml.find('>', open + 1U);
        if (close == std::string_view::npos) {
            throw ArchiveError("XAR TOC contains an unterminated XML tag");
        }
        if (close - open - 1U > kMaxXarXmlTagBytes || ++state.element_count > kMaxXarXmlElements) {
            throw ArchiveError("XAR TOC XML element exceeds SuperZip metadata limits");
        }
        const auto tag = trim_ascii(xml.substr(open + 1U, close - open - 1U));
        if (tag.empty()) {
            throw ArchiveError("XAR TOC contains an empty XML tag");
        }
        if (tag.front() == '!') {
            throw SecurityError("XAR TOC comments, DTDs, and declarations are not supported");
        }
        if (tag.front() != '?') {
            const bool closing = tag.front() == '/';
            const bool self_closing = !closing && tag.back() == '/';
            const auto name = xml_tag_name(tag);
            if (closing) {
                close_xar_toc_tag(name, state);
            } else {
                open_xar_toc_tag(tag, name, self_closing, state);
            }
        }
        position = close + 1U;
        text_start = position;
    }
    if (!state.tag_stack.empty() || !state.file_stack.empty()) {
        throw ArchiveError("XAR TOC ended with unclosed XML elements");
    }
    if (state.metadata.entries.empty()) {
        throw ArchiveError("XAR archive contains no extractable entries");
    }
    validate_xar_toc_paths(state.metadata);
    return std::move(state.metadata);
}

// Purpose: Read and parse XAR metadata from disk.
// Inputs: `archive_path` is the host archive path and `archive_size` is its full size.
// Outputs: Returns decoded and validated metadata.
XarMetadata read_xar_metadata(const std::filesystem::path& archive_path, std::uint64_t archive_size) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open XAR archive: " + path_diagnostic_utf8(archive_path));
    }
    const auto header = read_xar_header(input, archive_size);
    const auto compressed_toc = read_range(input, header.header_size, header.toc_compressed_size, "TOC");
    const auto toc = inflate_zlib_buffer(compressed_toc, header.toc_uncompressed_size, "TOC");
    auto metadata = parse_xar_toc(toc, header);
    validate_xar_ranges(metadata, archive_size);
    return metadata;
}

// Purpose: Consume one stored XAR payload range.
// Inputs: `input` is open, `start`/`size` define the payload, and `sink` receives bytes.
// Outputs: Returns decoded bytes or throws on read/sink failure.
std::uint64_t process_stored_payload(std::ifstream& input, std::uint64_t start, std::uint64_t size,
                                     const std::function<void(std::span<const unsigned char>)>& sink) {
    input.clear();
    input.seekg(to_streamoff(start, "payload"), std::ios::beg);
    if (!input) {
        throw ArchiveError("failed to seek XAR stored payload");
    }
    std::array<unsigned char, kXarBufferBytes> buffer{};
    std::uint64_t remaining = size;
    std::uint64_t decoded = 0;
    while (remaining > 0U) {
        const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(to_read));
        if (static_cast<std::size_t>(input.gcount()) != to_read) {
            throw ArchiveError("XAR stored payload is truncated");
        }
        remaining -= to_read;
        decoded = checked_add_u64(decoded, to_read, "stored decoded bytes");
        sink(std::span<const unsigned char>(buffer.data(), to_read));
    }
    return decoded;
}

// Purpose: Inflate and consume one zlib-compressed XAR payload range.
// Inputs: `input` is open, `start`/`encoded_size` bound the compressed stream, `expected_size` is the required decoded
// length, and `sink` receives decoded bytes. Outputs: Returns decoded bytes or throws on malformed streams, trailing
// data, or sink failure.
std::uint64_t process_zlib_payload(std::ifstream& input, std::uint64_t start, std::uint64_t encoded_size,
                                   std::uint64_t expected_size,
                                   const std::function<void(std::span<const unsigned char>)>& sink) {
    input.clear();
    input.seekg(to_streamoff(start, "payload"), std::ios::beg);
    if (!input) {
        throw ArchiveError("failed to seek XAR zlib payload");
    }
    mz_stream stream{};
    if (mz_inflateInit(&stream) != MZ_OK) {
        throw ArchiveError("failed to initialize XAR zlib decoder");
    }
    bool stream_active = true;
    std::array<unsigned char, kXarBufferBytes> input_buffer{};
    std::array<unsigned char, kXarBufferBytes> output_buffer{};
    std::uint64_t remaining = encoded_size;
    std::uint64_t decoded = 0;
    int status = MZ_OK;
    try {
        while (remaining > 0U && status != MZ_STREAM_END) {
            const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(input_buffer.size(), remaining));
            input.read(reinterpret_cast<char*>(input_buffer.data()), static_cast<std::streamsize>(to_read));
            if (static_cast<std::size_t>(input.gcount()) != to_read) {
                throw ArchiveError("XAR zlib payload is truncated");
            }
            remaining -= to_read;
            stream.next_in = input_buffer.data();
            stream.avail_in = static_cast<unsigned int>(to_read);
            do {
                stream.next_out = output_buffer.data();
                stream.avail_out = static_cast<unsigned int>(output_buffer.size());
                status = mz_inflate(&stream, MZ_NO_FLUSH);
                if (status != MZ_OK && status != MZ_STREAM_END) {
                    throw ArchiveError("XAR zlib payload failed to decompress");
                }
                const auto produced = output_buffer.size() - stream.avail_out;
                if (produced > 0U) {
                    decoded = checked_add_u64(decoded, produced, "zlib decoded bytes");
                    if (decoded > expected_size) {
                        throw ArchiveError("XAR zlib payload exceeds declared decoded length");
                    }
                    sink(std::span<const unsigned char>(output_buffer.data(), produced));
                }
            } while ((stream.avail_out == 0U || stream.avail_in > 0U) && status != MZ_STREAM_END);
        }
        if (status != MZ_STREAM_END || stream.avail_in != 0U || remaining != 0U) {
            throw ArchiveError("XAR zlib payload has trailing or incomplete compressed data");
        }
        if (decoded != expected_size) {
            throw ArchiveError("XAR zlib payload decoded length mismatch");
        }
        mz_inflateEnd(&stream);
        stream_active = false;
    } catch (...) {
        if (stream_active) {
            mz_inflateEnd(&stream);
        }
        throw;
    }
    return decoded;
}

// Purpose: Process one XAR file payload through validation or publication.
// Inputs: `input` is open, `metadata` supplies heap base, `entry` describes one file, and `sink` receives decoded
// bytes. Outputs: Returns decoded bytes or throws on malformed payloads.
std::uint64_t process_xar_payload(std::ifstream& input, const XarMetadata& metadata, const XarEntry& entry,
                                  const std::function<void(std::span<const unsigned char>)>& sink) {
    const auto start = checked_add_u64(metadata.header.heap_offset, entry.offset, "payload start");
    if (entry.encoding == XarEncoding::Stored) {
        return process_stored_payload(input, start, entry.compressed_size, sink);
    }
    return process_zlib_payload(input, start, entry.compressed_size, entry.size, sink);
}

// Purpose: Validate every XAR file payload before destination writes start.
// Inputs: `archive_path` is the XAR file and `metadata` is the parsed metadata table.
// Outputs: Throws on decode, size, or trailing-data failures.
void validate_xar_payloads(const std::filesystem::path& archive_path, const XarMetadata& metadata) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open XAR archive: " + path_diagnostic_utf8(archive_path));
    }
    const auto discard = [](std::span<const unsigned char>) {};
    for (const auto& entry : metadata.entries) {
        if (entry.directory) {
            continue;
        }
        const auto decoded = process_xar_payload(input, metadata, entry, discard);
        if (decoded != entry.size) {
            throw ArchiveError("XAR payload size mismatch: " + entry.path);
        }
    }
}

}  // namespace

// Purpose: Extract the supported XAR subset after TOC and complete payload prevalidation.
// Inputs: `archive_path`, `destination`, overwrite policy, and optional progress callback describe the operation.
// Outputs: Returns extraction telemetry or throws before retaining any unverified file.
OperationStats extract_xar(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                           bool overwrite, const ProgressCallback& progress_callback) {
    const auto started = std::chrono::steady_clock::now();
    const auto archive_source = pin_source_file(archive_path);
    const auto archive_size = archive_source.size();
    const auto metadata = read_xar_metadata(archive_source.path(), archive_size);
    validate_xar_payloads(archive_source.path(), metadata);

    create_verified_directories(destination);
    ProgressState progress;
    progress.start(OperationKind::Extract, metadata.total_file_bytes,
                   static_cast<std::uint64_t>(metadata.entries.size()));
    publish_progress(progress, progress_callback);

    std::ifstream input(archive_source.path(), std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open XAR archive: " + path_diagnostic_utf8(archive_path));
    }
    for (const auto& entry : metadata.entries) {
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path, ArchivePathEncoding::Utf8);
        if (entry.directory) {
            create_verified_directories(target);
            progress.finish_entry();
            publish_progress(progress, progress_callback);
            continue;
        }
        if (!overwrite && std::filesystem::exists(target)) {
            throw SecurityError("refusing to overwrite existing XAR extraction target: " +
                                path_diagnostic_utf8(target));
        }
        const auto temporary = reserve_file_publish_target(target);
        bool temporary_active = true;
        try {
            std::ofstream output(temporary.file, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw ArchiveError("failed to create temporary XAR extraction target: " + path_diagnostic_utf8(target));
            }
            const auto sink = [&](std::span<const unsigned char> bytes) {
                if (!bytes.empty()) {
                    output.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()));
                    if (!output) {
                        throw ArchiveError("failed to write temporary XAR extraction target: " +
                                           path_diagnostic_utf8(target));
                    }
                    progress.add_bytes(static_cast<std::uint64_t>(bytes.size()));
                    publish_progress(progress, progress_callback);
                }
            };
            const auto decoded = process_xar_payload(input, metadata, entry, sink);
            if (decoded != entry.size) {
                throw ArchiveError("XAR payload size mismatch: " + entry.path);
            }
            output.close();
            if (!output) {
                throw ArchiveError("failed to finalize temporary XAR extraction target: " +
                                   path_diagnostic_utf8(target));
            }
            commit_verified_file(temporary, target, overwrite);
            cleanup_file_publish_target(temporary);
            temporary_active = false;
        } catch (...) {
            if (temporary_active) {
                cleanup_file_publish_target(temporary);
            }
            throw;
        }
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }

    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = metadata.total_file_bytes;
    stats.entries = static_cast<std::uint64_t>(metadata.entries.size());
    stats.gpu_used = false;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

}  // namespace superzip
