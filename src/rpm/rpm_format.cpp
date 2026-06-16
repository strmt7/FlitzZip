#include "rpm/rpm_format.hpp"

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kRpmLeadBytes = 96U;
constexpr std::size_t kRpmHeaderPrefixBytes = 16U;
constexpr std::size_t kRpmIndexEntryBytes = 16U;
constexpr std::uint32_t kRpmMaxHeaderEntries = 65'536U;
constexpr std::uint32_t kRpmMaxHeaderStoreBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kRpmTagPayloadFormat = 1124U;
constexpr std::uint32_t kRpmTagPayloadCompressor = 1125U;
constexpr std::uint32_t kRpmTypeString = 6U;
constexpr std::array<unsigned char, 4U> kRpmLeadMagic{0xED, 0xAB, 0xEE, 0xDB};
constexpr std::array<unsigned char, 3U> kRpmHeaderMagic{0x8E, 0xAD, 0xE8};

struct RpmHeaderStringEntry {
    std::uint32_t tag = 0;
    std::string value;
};

struct RpmHeaderScan {
    std::uint64_t end_offset = 0;
    std::vector<RpmHeaderStringEntry> strings;
};

// Purpose: Add two RPM byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_rpm_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Multiply two RPM byte counters while detecting unsigned wraparound.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the product or throws `ArchiveError` before wraparound.
std::uint64_t checked_mul_rpm_bytes(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (lhs != 0U && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw ArchiveError(message);
    }
    return lhs * rhs;
}

// Purpose: Read exactly one byte range from an RPM stream.
// Inputs: `input` is the RPM stream, `buffer` receives bytes, and `label` names the region.
// Outputs: Fills the buffer or throws when the file is truncated.
void read_exact(std::istream& input, char* buffer, std::size_t size, const char* label) {
    input.read(buffer, static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw ArchiveError(std::string(label) + " is truncated");
    }
}

// Purpose: Seek an RPM stream to a bounded absolute byte offset.
// Inputs: `input` is seekable, `offset` is the target byte offset, and `label` names diagnostics.
// Outputs: Positions the stream or throws when the seek cannot be represented/performed.
void seek_rpm_offset(std::ifstream& input, std::uint64_t offset, const char* label) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw ArchiveError(std::string(label) + " offset is too large to seek safely");
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input) {
        throw ArchiveError(std::string("failed to seek ") + label);
    }
}

// Purpose: Decode one big-endian RPM 32-bit field.
// Inputs: `bytes` points to at least four bytes.
// Outputs: Returns the decoded unsigned integer.
std::uint32_t read_be32(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

// Purpose: Convert ASCII metadata to lowercase for RPM payload token matching.
// Inputs: `value` is a header string or inferred token.
// Outputs: Returns a lowercased copy; non-ASCII bytes are preserved except C-locale ASCII folds.
std::string ascii_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// Purpose: Return a string tag value from a scanned RPM header.
// Inputs: `header` contains string entries and `tag` is the RPM tag id.
// Outputs: Returns the first matching value, or empty when absent.
std::string find_header_string(const RpmHeaderScan& header, std::uint32_t tag) {
    const auto it = std::ranges::find_if(header.strings, [tag](const RpmHeaderStringEntry& entry) {
        return entry.tag == tag;
    });
    return it == header.strings.end() ? std::string{} : it->value;
}

// Purpose: Align an RPM section end to the next eight-byte boundary.
// Inputs: `offset` is the current absolute byte offset.
// Outputs: Returns the aligned offset or throws before unsigned wraparound.
std::uint64_t align_rpm_eight(std::uint64_t offset) {
    const auto remainder = offset % 8U;
    if (remainder == 0U) {
        return offset;
    }
    return checked_add_rpm_bytes(offset, 8U - remainder, "RPM alignment overflow");
}

// Purpose: Read one RPM header section and collect string tags relevant to payload decoding.
// Inputs: `input` is positioned at the header, `archive_size` bounds section lengths, and `label` names diagnostics.
// Outputs: Returns the byte offset immediately after the header and collected string entries.
RpmHeaderScan read_rpm_header(std::ifstream& input, std::uint64_t archive_size, const char* label) {
    const auto start_pos = input.tellg();
    if (start_pos == std::istream::pos_type(-1)) {
        throw ArchiveError(std::string("failed to read RPM ") + label + " header offset");
    }
    const auto start_offset = static_cast<std::uint64_t>(start_pos);

    std::array<unsigned char, kRpmHeaderPrefixBytes> prefix{};
    read_exact(input, reinterpret_cast<char*>(prefix.data()), prefix.size(), label);
    if (!std::equal(kRpmHeaderMagic.begin(), kRpmHeaderMagic.end(), prefix.begin()) || prefix[3] != 1U) {
        throw ArchiveError(std::string("malformed RPM ") + label + " header magic");
    }
    if (prefix[4] != 0U || prefix[5] != 0U || prefix[6] != 0U || prefix[7] != 0U) {
        throw ArchiveError(std::string("malformed RPM ") + label + " header reserved bytes");
    }
    const auto index_count = read_be32(prefix.data() + 8U);
    const auto store_size = read_be32(prefix.data() + 12U);
    if (index_count > kRpmMaxHeaderEntries) {
        throw ArchiveError(std::string("RPM ") + label + " header contains too many entries");
    }
    if (store_size > kRpmMaxHeaderStoreBytes) {
        throw ArchiveError(std::string("RPM ") + label + " header store exceeds SuperZip limits");
    }

    const auto index_bytes = checked_mul_rpm_bytes(index_count, kRpmIndexEntryBytes, "RPM index byte count overflow");
    const auto header_bytes = checked_add_rpm_bytes(
        checked_add_rpm_bytes(kRpmHeaderPrefixBytes, index_bytes, "RPM header byte count overflow"),
        store_size,
        "RPM header byte count overflow");
    const auto end_offset = checked_add_rpm_bytes(start_offset, header_bytes, "RPM header end overflow");
    if (end_offset > archive_size) {
        throw ArchiveError(std::string("RPM ") + label + " header extends past end of file");
    }

    std::vector<std::array<unsigned char, kRpmIndexEntryBytes>> entries(index_count);
    for (auto& entry : entries) {
        read_exact(input, reinterpret_cast<char*>(entry.data()), entry.size(), "RPM header index");
    }
    std::vector<unsigned char> store(store_size);
    if (!store.empty()) {
        read_exact(input, reinterpret_cast<char*>(store.data()), store.size(), "RPM header store");
    }

    RpmHeaderScan result;
    result.end_offset = end_offset;
    for (const auto& entry : entries) {
        const auto tag = read_be32(entry.data());
        const auto type = read_be32(entry.data() + 4U);
        const auto offset = read_be32(entry.data() + 8U);
        const auto count = read_be32(entry.data() + 12U);
        if (offset > store.size()) {
            throw ArchiveError(std::string("RPM ") + label + " header entry offset is outside the store");
        }
        if (type != kRpmTypeString || count == 0U) {
            continue;
        }
        const auto begin = store.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto nul = std::find(begin, store.end(), 0U);
        if (nul == store.end()) {
            throw ArchiveError(std::string("RPM ") + label + " string entry is not NUL-terminated");
        }
        result.strings.push_back(RpmHeaderStringEntry{
            .tag = tag,
            .value = std::string(begin, nul),
        });
    }
    return result;
}

// Purpose: Read and validate the fixed 96-byte RPM lead.
// Inputs: `input` is positioned at the file start.
// Outputs: Throws when the file is not an RPM lead supported by SuperZip.
void read_rpm_lead(std::ifstream& input) {
    std::array<unsigned char, kRpmLeadBytes> lead{};
    read_exact(input, reinterpret_cast<char*>(lead.data()), lead.size(), "RPM lead");
    if (!std::equal(kRpmLeadMagic.begin(), kRpmLeadMagic.end(), lead.begin())) {
        throw ArchiveError("malformed RPM lead magic");
    }
    if (lead[4] != 3U || lead[5] != 0U) {
        throw ArchiveError("unsupported RPM lead version");
    }
}

// Purpose: Infer payload compression from RPM header metadata.
// Inputs: `compressor` is a lowercase payload compressor tag value.
// Outputs: Returns a recognized compression value, empty when absent/unknown, or throws for explicitly unsupported algorithms.
std::optional<RpmPayloadCompression> compression_from_header(const std::string& compressor) {
    if (compressor.empty()) {
        return std::nullopt;
    }
    if (compressor == "none") {
        return RpmPayloadCompression::None;
    }
    if (compressor == "gzip" || compressor == "gz") {
        return RpmPayloadCompression::Gzip;
    }
    if (compressor == "bzip2" || compressor == "bz2") {
        return RpmPayloadCompression::Bzip2;
    }
    if (compressor == "xz") {
        return RpmPayloadCompression::Xz;
    }
    if (compressor == "zstd" || compressor == "zst") {
        return RpmPayloadCompression::Zstd;
    }
    throw ArchiveError("unsupported RPM payload compressor: " + compressor);
}

// Purpose: Infer payload compression from payload magic bytes.
// Inputs: `magic` contains up to the first six payload bytes.
// Outputs: Returns a recognized compression value, or empty when the bytes are not enough to decide.
std::optional<RpmPayloadCompression> compression_from_magic(const std::array<unsigned char, 6U>& magic) {
    if (magic[0] == '0' && magic[1] == '7' && magic[2] == '0' && magic[3] == '7') {
        return RpmPayloadCompression::None;
    }
    if (magic[0] == 0x1FU && magic[1] == 0x8BU) {
        return RpmPayloadCompression::Gzip;
    }
    if (magic[0] == 'B' && magic[1] == 'Z' && magic[2] == 'h') {
        return RpmPayloadCompression::Bzip2;
    }
    if (magic[0] == 0xFDU && magic[1] == '7' && magic[2] == 'z' && magic[3] == 'X' && magic[4] == 'Z' && magic[5] == 0x00U) {
        return RpmPayloadCompression::Xz;
    }
    if (magic[0] == 0x28U && magic[1] == 0xB5U && magic[2] == 0x2FU && magic[3] == 0xFDU) {
        return RpmPayloadCompression::Zstd;
    }
    return std::nullopt;
}

// Purpose: Read the leading payload bytes for compression detection.
// Inputs: `input` is the RPM stream, `payload_offset` is the absolute start, and `payload_size` bounds readable bytes.
// Outputs: Returns a zero-filled six-byte buffer containing available leading payload bytes.
std::array<unsigned char, 6U> read_payload_magic(std::ifstream& input, std::uint64_t payload_offset, std::uint64_t payload_size) {
    std::array<unsigned char, 6U> magic{};
    const auto to_read = static_cast<std::size_t>(std::min<std::uint64_t>(magic.size(), payload_size));
    if (to_read == 0U) {
        return magic;
    }
    seek_rpm_offset(input, payload_offset, "RPM payload");
    read_exact(input, reinterpret_cast<char*>(magic.data()), to_read, "RPM payload magic");
    return magic;
}

}  // namespace

RpmPayloadInfo scan_rpm_payload(const std::filesystem::path& archive_path) {
    const auto archive_size = std::filesystem::file_size(archive_path);
    if (archive_size < kRpmLeadBytes) {
        throw ArchiveError("RPM file is too small");
    }

    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open RPM package: " + archive_path.string());
    }

    read_rpm_lead(input);
    const auto signature = read_rpm_header(input, archive_size, "signature");
    const auto main_header_offset = align_rpm_eight(signature.end_offset);
    if (main_header_offset > archive_size) {
        throw ArchiveError("RPM signature padding extends past end of file");
    }
    seek_rpm_offset(input, main_header_offset, "RPM package header");
    const auto package_header = read_rpm_header(input, archive_size, "package");
    const auto payload_offset = package_header.end_offset;
    if (payload_offset >= archive_size) {
        throw ArchiveError("RPM payload is empty");
    }

    auto payload_format = ascii_lower(find_header_string(package_header, kRpmTagPayloadFormat));
    auto payload_compressor = ascii_lower(find_header_string(package_header, kRpmTagPayloadCompressor));
    if (!payload_format.empty() && payload_format != "cpio") {
        throw ArchiveError("unsupported RPM payload format: " + payload_format);
    }

    const auto payload_size = archive_size - payload_offset;
    const auto from_header = compression_from_header(payload_compressor);
    const auto from_magic = compression_from_magic(read_payload_magic(input, payload_offset, payload_size));
    if (!from_header.has_value() && !from_magic.has_value()) {
        throw ArchiveError("unsupported or unrecognized RPM payload compression");
    }
    if (from_header.has_value() && from_magic.has_value() && *from_header != *from_magic) {
        throw ArchiveError("RPM payload compressor does not match payload magic");
    }

    return RpmPayloadInfo{
        .offset = payload_offset,
        .size = payload_size,
        .compression = from_header.value_or(*from_magic),
        .payload_format = std::move(payload_format),
        .payload_compressor = std::move(payload_compressor),
    };
}

}  // namespace superzip
