#include "core/archive_format.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kArchiveProbeBytes = 0x8806U;

constexpr std::array<ArchiveFormatInfo, 24> kFormatRegistry{{
    {ArchiveFormat::Unknown, "unknown", "Unknown archive", "", false, false, false, false},
    {ArchiveFormat::Auto, "auto", "Automatic detection", "", false, true, false, false},
    {ArchiveFormat::SuperZip, "suzip", "SuperZip GPU (.suzip)", ".suzip", true, true, true, true},
    {ArchiveFormat::Zip, "zip", "ZIP (.zip)", ".zip", true, true, false, true},
    {ArchiveFormat::SevenZip, "7z", "7-Zip (.7z)", ".7z", false, false, false, false},
    {ArchiveFormat::Rar, "rar", "RAR (.rar)", ".rar", false, false, false, false},
    {ArchiveFormat::Tar, "tar", "TAR (.tar)", ".tar", true, true, false, true},
    {ArchiveFormat::TarGzip, "tar.gz", "TAR + Gzip (.tar.gz, .tgz)", ".tar.gz,.tgz", false, false, false, false},
    {ArchiveFormat::TarBzip2, "tar.bz2", "TAR + Bzip2 (.tar.bz2, .tbz2)", ".tar.bz2,.tbz2", false, false, false, false},
    {ArchiveFormat::TarXz, "tar.xz", "TAR + XZ (.tar.xz, .txz)", ".tar.xz,.txz", false, false, false, false},
    {ArchiveFormat::TarZstd, "tar.zst", "TAR + Zstandard (.tar.zst, .tzst)", ".tar.zst,.tzst", false, false, false, false},
    {ArchiveFormat::Gzip, "gz", "Gzip stream (.gz)", ".gz", false, false, false, false},
    {ArchiveFormat::Bzip2, "bz2", "Bzip2 stream (.bz2)", ".bz2", false, false, false, false},
    {ArchiveFormat::Xz, "xz", "XZ stream (.xz)", ".xz", false, false, false, false},
    {ArchiveFormat::Zstd, "zst", "Zstandard stream (.zst)", ".zst", false, false, false, false},
    {ArchiveFormat::Cab, "cab", "CAB (.cab)", ".cab", false, false, false, false},
    {ArchiveFormat::Iso, "iso", "ISO image (.iso)", ".iso", false, false, false, false},
    {ArchiveFormat::Cpio, "cpio", "CPIO (.cpio)", ".cpio", false, false, false, false},
    {ArchiveFormat::Arj, "arj", "ARJ (.arj)", ".arj", false, false, false, false},
    {ArchiveFormat::Lha, "lha", "LHA/LZH (.lha, .lzh)", ".lha,.lzh", false, false, false, false},
    {ArchiveFormat::Wim, "wim", "Windows Imaging (.wim)", ".wim,.swm", false, false, false, false},
    {ArchiveFormat::Xar, "xar", "XAR (.xar)", ".xar", false, false, false, false},
    {ArchiveFormat::Deb, "deb", "Debian package (.deb)", ".deb", false, false, false, false},
    {ArchiveFormat::Rpm, "rpm", "RPM package (.rpm)", ".rpm", false, false, false, false},
}};

// Purpose: Convert a string to lowercase ASCII for extension and token matching.
// Inputs: `value` is an archive token or filename.
// Outputs: Returns a lowercased copy; non-ASCII bytes are preserved except C-locale ASCII folds.
std::string ascii_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// Purpose: Test whether a string ends in a case-normalized suffix.
// Inputs: `value` and `suffix` are lowercase strings.
// Outputs: Returns true when `suffix` is a byte suffix of `value`.
bool ends_with_lower(const std::string& value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Purpose: Detect ZIP-container document/package aliases that are intentionally not archive formats.
// Inputs: `path` is the candidate archive path.
// Outputs: Returns true for known ZIP-based aliases that SuperZip should not expose as archive formats.
bool has_excluded_zip_container_extension(const std::filesystem::path& path) {
    const auto name = ascii_lower(path.filename().string());
    constexpr std::array excluded = {
        ".docx", ".pptx", ".xlsx",
        ".odt", ".ods", ".odp",
        ".jar", ".war", ".ear",
        ".apk", ".ipa", ".xpi",
        ".cbz",
    };
    return std::ranges::any_of(excluded, [&](std::string_view extension) {
        return ends_with_lower(name, extension);
    });
}

// Purpose: Read bounded leading bytes for format magic detection.
// Inputs: `path` is the archive file to probe.
// Outputs: Returns up to `kArchiveProbeBytes`; returns empty on open/read failure.
std::vector<unsigned char> read_probe_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::vector<unsigned char> bytes(kArchiveProbeBytes);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, input.gcount())));
    return bytes;
}

// Purpose: Match a fixed byte signature at the start of a probe buffer.
// Inputs: `bytes` is the file probe and `signature` is the magic sequence.
// Outputs: Returns true when all signature bytes match from offset zero.
bool starts_with_signature(std::span<const unsigned char> bytes, std::initializer_list<unsigned char> signature) {
    if (bytes.size() < signature.size()) {
        return false;
    }
    return std::equal(signature.begin(), signature.end(), bytes.begin());
}

// Purpose: Match a fixed byte signature at an arbitrary offset.
// Inputs: `bytes` is the file probe, `offset` is the byte offset, and `signature` is the magic sequence.
// Outputs: Returns true when all signature bytes match at `offset`.
bool matches_signature_at(
    std::span<const unsigned char> bytes,
    std::size_t offset,
    std::initializer_list<unsigned char> signature) {
    if (offset > bytes.size() || bytes.size() - offset < signature.size()) {
        return false;
    }
    return std::equal(signature.begin(), signature.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Detect archive formats with stable magic bytes before extension fallback.
// Inputs: `bytes` contains a bounded prefix probe and `path` supplies extension context for ambiguous container magics.
// Outputs: Returns a detected format or unknown.
ArchiveFormat detect_by_magic(std::span<const unsigned char> bytes, const std::filesystem::path& path) {
    if (starts_with_signature(bytes, {'P', 'K', 0x03, 0x04}) ||
        starts_with_signature(bytes, {'P', 'K', 0x05, 0x06}) ||
        starts_with_signature(bytes, {'P', 'K', 0x07, 0x08})) {
        return ArchiveFormat::Zip;
    }
    if (starts_with_signature(bytes, {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C})) {
        return ArchiveFormat::SevenZip;
    }
    if (starts_with_signature(bytes, {'R', 'a', 'r', '!', 0x1A, 0x07, 0x00}) ||
        starts_with_signature(bytes, {'R', 'a', 'r', '!', 0x1A, 0x07, 0x01, 0x00})) {
        return ArchiveFormat::Rar;
    }
    if (starts_with_signature(bytes, {0x1F, 0x8B})) {
        return ArchiveFormat::Gzip;
    }
    if (starts_with_signature(bytes, {'B', 'Z', 'h'})) {
        return ArchiveFormat::Bzip2;
    }
    if (starts_with_signature(bytes, {0xFD, '7', 'z', 'X', 'Z', 0x00})) {
        return ArchiveFormat::Xz;
    }
    if (starts_with_signature(bytes, {0x28, 0xB5, 0x2F, 0xFD})) {
        return ArchiveFormat::Zstd;
    }
    if (starts_with_signature(bytes, {'M', 'S', 'C', 'F'})) {
        return ArchiveFormat::Cab;
    }
    if (starts_with_signature(bytes, {'0', '7', '0', '7', '0', '1'}) ||
        starts_with_signature(bytes, {'0', '7', '0', '7', '0', '2'}) ||
        starts_with_signature(bytes, {'0', '7', '0', '7', '0', '7'})) {
        return ArchiveFormat::Cpio;
    }
    if (starts_with_signature(bytes, {0x60, 0xEA})) {
        return ArchiveFormat::Arj;
    }
    if (matches_signature_at(bytes, 2U, {'-', 'l', 'h'})) {
        return ArchiveFormat::Lha;
    }
    if (starts_with_signature(bytes, {'M', 'S', 'W', 'I', 'M', 0x00, 0x00, 0x00})) {
        return ArchiveFormat::Wim;
    }
    if (starts_with_signature(bytes, {'x', 'a', 'r', '!'})) {
        return ArchiveFormat::Xar;
    }
    if (starts_with_signature(bytes, {0xED, 0xAB, 0xEE, 0xDB})) {
        return ArchiveFormat::Rpm;
    }
    if (starts_with_signature(bytes, {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'})) {
        const auto lower_name = ascii_lower(path.filename().string());
        return ends_with_lower(lower_name, ".deb") ? ArchiveFormat::Deb : ArchiveFormat::Unknown;
    }
    if (matches_signature_at(bytes, 257U, {'u', 's', 't', 'a', 'r'})) {
        return ArchiveFormat::Tar;
    }
    if (matches_signature_at(bytes, 0x8001U, {'C', 'D', '0', '0', '1'}) ||
        matches_signature_at(bytes, 0x8801U, {'C', 'D', '0', '0', '1'})) {
        return ArchiveFormat::Iso;
    }
    return ArchiveFormat::Unknown;
}

// Purpose: Detect archive format from common single and compound extensions.
// Inputs: `path` is the archive path being classified.
// Outputs: Returns a recognized extension format or unknown.
ArchiveFormat detect_by_extension(const std::filesystem::path& path) {
    const auto name = ascii_lower(path.filename().string());
    if (ends_with_lower(name, ".suzip")) {
        return ArchiveFormat::SuperZip;
    }
    if (ends_with_lower(name, ".zip")) {
        return ArchiveFormat::Zip;
    }
    if (ends_with_lower(name, ".7z")) {
        return ArchiveFormat::SevenZip;
    }
    if (ends_with_lower(name, ".rar")) {
        return ArchiveFormat::Rar;
    }
    if (ends_with_lower(name, ".tar")) {
        return ArchiveFormat::Tar;
    }
    if (ends_with_lower(name, ".tar.gz") || ends_with_lower(name, ".tgz")) {
        return ArchiveFormat::TarGzip;
    }
    if (ends_with_lower(name, ".tar.bz2") || ends_with_lower(name, ".tbz") || ends_with_lower(name, ".tbz2")) {
        return ArchiveFormat::TarBzip2;
    }
    if (ends_with_lower(name, ".tar.xz") || ends_with_lower(name, ".txz")) {
        return ArchiveFormat::TarXz;
    }
    if (ends_with_lower(name, ".tar.zst") || ends_with_lower(name, ".tzst")) {
        return ArchiveFormat::TarZstd;
    }
    if (ends_with_lower(name, ".gz")) {
        return ArchiveFormat::Gzip;
    }
    if (ends_with_lower(name, ".bz2")) {
        return ArchiveFormat::Bzip2;
    }
    if (ends_with_lower(name, ".xz")) {
        return ArchiveFormat::Xz;
    }
    if (ends_with_lower(name, ".zst") || ends_with_lower(name, ".zstd")) {
        return ArchiveFormat::Zstd;
    }
    if (ends_with_lower(name, ".cab")) {
        return ArchiveFormat::Cab;
    }
    if (ends_with_lower(name, ".iso")) {
        return ArchiveFormat::Iso;
    }
    if (ends_with_lower(name, ".cpio")) {
        return ArchiveFormat::Cpio;
    }
    if (ends_with_lower(name, ".arj")) {
        return ArchiveFormat::Arj;
    }
    if (ends_with_lower(name, ".lha") || ends_with_lower(name, ".lzh")) {
        return ArchiveFormat::Lha;
    }
    if (ends_with_lower(name, ".wim") || ends_with_lower(name, ".swm")) {
        return ArchiveFormat::Wim;
    }
    if (ends_with_lower(name, ".xar")) {
        return ArchiveFormat::Xar;
    }
    if (ends_with_lower(name, ".deb")) {
        return ArchiveFormat::Deb;
    }
    if (ends_with_lower(name, ".rpm")) {
        return ArchiveFormat::Rpm;
    }
    return ArchiveFormat::Unknown;
}

}  // namespace

std::span<const ArchiveFormatInfo> archive_format_registry() {
    return kFormatRegistry;
}

const ArchiveFormatInfo& archive_format_info(ArchiveFormat format) {
    const auto it = std::ranges::find_if(kFormatRegistry, [format](const ArchiveFormatInfo& info) {
        return info.format == format;
    });
    return it == kFormatRegistry.end() ? kFormatRegistry.front() : *it;
}

std::optional<ArchiveFormat> parse_archive_format_token(std::string_view token) {
    auto lowered = ascii_lower(std::string(token));
    if (lowered == "tgz") {
        lowered = "tar.gz";
    } else if (lowered == "tbz" || lowered == "tbz2") {
        lowered = "tar.bz2";
    } else if (lowered == "txz") {
        lowered = "tar.xz";
    } else if (lowered == "tzst") {
        lowered = "tar.zst";
    } else if (lowered == "zstd") {
        lowered = "zst";
    } else if (lowered == "lzh") {
        lowered = "lha";
    }
    const auto it = std::ranges::find_if(kFormatRegistry, [&](const ArchiveFormatInfo& info) {
        return lowered == info.key;
    });
    if (it == kFormatRegistry.end()) {
        return std::nullopt;
    }
    return it->format;
}

ArchiveFormat detect_archive_format(const std::filesystem::path& archive_path) {
    if (has_excluded_zip_container_extension(archive_path)) {
        return ArchiveFormat::Unknown;
    }
    const auto by_extension = detect_by_extension(archive_path);
    if (by_extension == ArchiveFormat::TarGzip ||
        by_extension == ArchiveFormat::TarBzip2 ||
        by_extension == ArchiveFormat::TarXz ||
        by_extension == ArchiveFormat::TarZstd) {
        return by_extension;
    }
    const auto by_magic = detect_by_magic(read_probe_bytes(archive_path), archive_path);
    if (by_magic != ArchiveFormat::Unknown) {
        return by_magic;
    }
    return by_extension;
}

std::string archive_format_key_list(bool include_auto) {
    std::ostringstream out;
    bool first = true;
    for (const auto& info : kFormatRegistry) {
        if (info.format == ArchiveFormat::Unknown || (!include_auto && info.format == ArchiveFormat::Auto)) {
            continue;
        }
        if (!first) {
            out << ", ";
        }
        out << info.key;
        first = false;
    }
    return out.str();
}

}  // namespace superzip
