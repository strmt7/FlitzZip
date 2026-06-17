#include "core/archive_format.hpp"

#include "core/archive_index.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace superzip {
namespace {

constexpr std::size_t kArchiveProbeBytes = 0x8806U;
constexpr std::uint64_t kSuperZipFooterBytes = 24U;

struct ExtensionFormatMapping {
    std::string_view extension;
    ArchiveFormat format = ArchiveFormat::Unknown;
};

constexpr std::array<ArchiveFormatInfo, 35> kFormatRegistry{{
    {ArchiveFormat::Unknown, "unknown", "Unknown archive", "", false, false, false, false},
    {ArchiveFormat::Auto, "auto", "Automatic detection", "", false, true, false, false},
    {ArchiveFormat::SuperZip, "suzip", "SuperZip GPU (.suzip)", ".suzip", true, true, true, true},
    {ArchiveFormat::Zip, "zip", "ZIP (.zip)", ".zip", true, true, false, true},
    {ArchiveFormat::Zipx, "zipx", "ZIPX (.zipx)", ".zipx", false, true, false, true},
    {ArchiveFormat::SevenZip, "7z", "7-Zip (.7z)", ".7z", false, true, false, true},
    {ArchiveFormat::Rar, "rar", "RAR (.rar)", ".rar", false, false, false, false},
    {ArchiveFormat::Tar, "tar", "TAR (.tar)", ".tar", true, true, false, true},
    {ArchiveFormat::TarGzip, "tar.gz", "TAR + Gzip (.tar.gz, .tgz)", ".tar.gz,.tgz", true, true, false, true},
    {ArchiveFormat::TarBzip2, "tar.bz2", "TAR + Bzip2 (.tar.bz2, .tbz, .tbz2)", ".tar.bz2,.tbz,.tbz2", true, true, false, true},
    {ArchiveFormat::TarXz, "tar.xz", "TAR + XZ (.tar.xz, .txz)", ".tar.xz,.txz", false, true, false, true},
    {ArchiveFormat::TarLzip, "tar.lz", "TAR + lzip (.tar.lz, .tlz)", ".tar.lz,.tlz", false, true, false, true},
    {ArchiveFormat::TarZstd, "tar.zst", "TAR + Zstandard (.tar.zst, .tzst)", ".tar.zst,.tzst", true, true, false, true},
    {ArchiveFormat::Gzip, "gz", "Gzip stream (.gz)", ".gz", true, true, false, true},
    {ArchiveFormat::UnixCompress, "z", "Unix Compress (.Z)", ".Z", true, true, false, true},
    {ArchiveFormat::Base64, "b64", "Base64 encoded file (.b64)", ".b64", true, true, false, true},
    {ArchiveFormat::Bzip2, "bz2", "Bzip2 stream (.bz2)", ".bz2", true, true, false, true},
    {ArchiveFormat::Xz, "xz", "XZ stream (.xz)", ".xz", false, true, false, true},
    {ArchiveFormat::Lzma, "lzma", "LZMA stream (.lzma)", ".lzma", false, true, false, true},
    {ArchiveFormat::Lzip, "lz", "lzip stream (.lz)", ".lz", false, true, false, true},
    {ArchiveFormat::Zstd, "zst", "Zstandard stream (.zst)", ".zst,.zstd", true, true, false, true},
    {ArchiveFormat::Cab, "cab", "CAB (.cab)", ".cab", false, true, false, true},
    {ArchiveFormat::Iso, "iso", "ISO image (.iso)", ".iso", false, true, false, true},
    {ArchiveFormat::Cpio, "cpio", "CPIO (.cpio)", ".cpio", true, true, false, true},
    {ArchiveFormat::CpioGzip, "cpio.gz", "CPIO + Gzip (.cpio.gz, .cpgz)", ".cpio.gz,.cpgz", true, true, false, true},
    {ArchiveFormat::Ar, "ar", "Unix AR (.ar)", ".ar", true, true, false, true},
    {ArchiveFormat::Arj, "arj", "ARJ (.arj)", ".arj", false, true, false, true},
    {ArchiveFormat::Arc, "arc", "SEA ARC/ARK (.arc, .ark)", ".arc,.ark", false, true, false, true},
    {ArchiveFormat::Uue, "uue", "UUencoded file (.uue, .uu)", ".uue,.uu", true, true, false, true},
    {ArchiveFormat::Lha, "lha", "LHA/LZH (.lha, .lzh)", ".lha,.lzh", false, true, false, true},
    {ArchiveFormat::Wim, "wim", "Windows Imaging (.wim)", ".wim", false, true, false, true},
    {ArchiveFormat::SplitWim, "swm", "Split Windows Imaging part (.swm)", ".swm", false, false, false, false},
    {ArchiveFormat::Xar, "xar", "XAR (.xar)", ".xar", false, true, false, true},
    {ArchiveFormat::Deb, "deb", "Debian package (.deb)", ".deb", false, true, false, true},
    {ArchiveFormat::Rpm, "rpm", "RPM package (.rpm)", ".rpm", false, true, false, true},
}};

constexpr std::array<ExtensionFormatMapping, 44> kExtensionFormats{{
    {".suzip", ArchiveFormat::SuperZip},
    {".zip", ArchiveFormat::Zip},
    {".zipx", ArchiveFormat::Zipx},
    {".7z", ArchiveFormat::SevenZip},
    {".rar", ArchiveFormat::Rar},
    {".tar.gz", ArchiveFormat::TarGzip},
    {".tgz", ArchiveFormat::TarGzip},
    {".tar.bz2", ArchiveFormat::TarBzip2},
    {".tbz", ArchiveFormat::TarBzip2},
    {".tbz2", ArchiveFormat::TarBzip2},
    {".tar.xz", ArchiveFormat::TarXz},
    {".txz", ArchiveFormat::TarXz},
    {".tar.lz", ArchiveFormat::TarLzip},
    {".tlz", ArchiveFormat::TarLzip},
    {".tar.zst", ArchiveFormat::TarZstd},
    {".tzst", ArchiveFormat::TarZstd},
    {".cpio.gz", ArchiveFormat::CpioGzip},
    {".cpgz", ArchiveFormat::CpioGzip},
    {".tar", ArchiveFormat::Tar},
    {".gz", ArchiveFormat::Gzip},
    {".z", ArchiveFormat::UnixCompress},
    {".b64", ArchiveFormat::Base64},
    {".bz2", ArchiveFormat::Bzip2},
    {".xz", ArchiveFormat::Xz},
    {".lzma", ArchiveFormat::Lzma},
    {".lz", ArchiveFormat::Lzip},
    {".zst", ArchiveFormat::Zstd},
    {".zstd", ArchiveFormat::Zstd},
    {".cab", ArchiveFormat::Cab},
    {".iso", ArchiveFormat::Iso},
    {".cpio", ArchiveFormat::Cpio},
    {".ar", ArchiveFormat::Ar},
    {".arj", ArchiveFormat::Arj},
    {".arc", ArchiveFormat::Arc},
    {".ark", ArchiveFormat::Arc},
    {".uue", ArchiveFormat::Uue},
    {".uu", ArchiveFormat::Uue},
    {".lha", ArchiveFormat::Lha},
    {".lzh", ArchiveFormat::Lha},
    {".wim", ArchiveFormat::Wim},
    {".swm", ArchiveFormat::SplitWim},
    {".xar", ArchiveFormat::Xar},
    {".deb", ArchiveFormat::Deb},
    {".rpm", ArchiveFormat::Rpm},
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

// Purpose: Detect ZIP-based application/document containers that must not be treated as archives.
// Inputs: `path` is the candidate archive path.
// Outputs: Returns true when a ZIP-magic file uses an extension outside SuperZip archive support.
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

// Purpose: Probe the native SUZIP footer and index magic without depending on the `.suzip` extension.
// Inputs: `path` is the candidate archive file.
// Outputs: Returns true only when the footer, version, index bounds, and index magic match the native format.
bool has_suzip_footer_signature(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return false;
        }
        input.seekg(0, std::ios::end);
        const auto end_position = input.tellg();
        if (end_position == std::ifstream::pos_type(-1) ||
            static_cast<std::uint64_t>(end_position) < kSuperZipFooterBytes) {
            return false;
        }
        const auto file_size = static_cast<std::uint64_t>(end_position);
        input.seekg(-static_cast<std::streamoff>(kSuperZipFooterBytes), std::ios::end);
        if (read_u32(input) != kSuperZipFooterMagic || read_u32(input) != kSuperZipVersion) {
            return false;
        }
        const auto index_offset = read_u64(input);
        const auto index_size = read_u64(input);
        if (index_size < 12U || index_size > kMaxArchiveIndexBytes || index_offset > file_size) {
            return false;
        }
        if (index_size > file_size - kSuperZipFooterBytes ||
            index_offset > file_size - kSuperZipFooterBytes - index_size) {
            return false;
        }
        input.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
        return read_u32(input) == kSuperZipMagic;
    } catch (const std::exception&) {
        return false;
    }
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

// Purpose: Detect a UUencode begin line in a bounded text probe.
// Inputs: `bytes` contains the file prefix; preamble lines are ignored only within the probe.
// Outputs: Returns true when a line starts with the strict `begin <mode> <name>` shape.
bool has_uue_begin_line(std::span<const unsigned char> bytes) {
    std::size_t line_start = 0;
    while (line_start < bytes.size()) {
        std::size_t line_end = line_start;
        while (line_end < bytes.size() && bytes[line_end] != '\n') {
            ++line_end;
        }
        auto line_size = line_end - line_start;
        if (line_size > 0U && bytes[line_start + line_size - 1U] == '\r') {
            --line_size;
        }
        constexpr std::string_view prefix = "begin ";
        if (line_size > prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), bytes.begin() + static_cast<std::ptrdiff_t>(line_start))) {
            auto cursor = line_start + prefix.size();
            const auto end = line_start + line_size;
            while (cursor < end && bytes[cursor] >= '0' && bytes[cursor] <= '7') {
                ++cursor;
            }
            if (cursor > line_start + prefix.size() && cursor + 1U < end && bytes[cursor] == ' ') {
                return true;
            }
        }
        line_start = line_end == bytes.size() ? bytes.size() : line_end + 1U;
    }
    return false;
}

// Purpose: Detect a wrapped Base64 begin line in a bounded text probe.
// Inputs: `bytes` contains the file prefix; preamble lines are ignored only within the probe.
// Outputs: Returns true when a line starts with `begin-base64` or `begin-base64-encoded`.
bool has_base64_begin_line(std::span<const unsigned char> bytes) {
    std::size_t line_start = 0;
    while (line_start < bytes.size()) {
        std::size_t line_end = line_start;
        while (line_end < bytes.size() && bytes[line_end] != '\n') {
            ++line_end;
        }
        auto line_size = line_end - line_start;
        if (line_size > 0U && bytes[line_start + line_size - 1U] == '\r') {
            --line_size;
        }
        constexpr std::array prefixes{
            std::string_view{"begin-base64 "},
            std::string_view{"begin-base64-encoded "},
        };
        for (const auto prefix : prefixes) {
            const bool prefix_matches =
                line_size > prefix.size() &&
                std::equal(prefix.begin(), prefix.end(), bytes.begin() + static_cast<std::ptrdiff_t>(line_start));
            if (prefix_matches) {
                return true;
            }
        }
        line_start = line_end == bytes.size() ? bytes.size() : line_end + 1U;
    }
    return false;
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
        const auto lower_name = ascii_lower(path.filename().string());
        return ends_with_lower(lower_name, ".zipx") ? ArchiveFormat::Zipx : ArchiveFormat::Zip;
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
    if (starts_with_signature(bytes, {0x1F, 0x9D})) {
        return ArchiveFormat::UnixCompress;
    }
    if (has_base64_begin_line(bytes)) {
        return ArchiveFormat::Base64;
    }
    if (starts_with_signature(bytes, {'B', 'Z', 'h'})) {
        return ArchiveFormat::Bzip2;
    }
    if (starts_with_signature(bytes, {0xFD, '7', 'z', 'X', 'Z', 0x00})) {
        return ArchiveFormat::Xz;
    }
    if (starts_with_signature(bytes, {'L', 'Z', 'I', 'P'})) {
        const auto lower_name = ascii_lower(path.filename().string());
        return (ends_with_lower(lower_name, ".tar.lz") || ends_with_lower(lower_name, ".tlz")) ? ArchiveFormat::TarLzip : ArchiveFormat::Lzip;
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
    if (bytes.size() >= 2U && bytes[0] == 0x1AU && bytes[1] <= 9U) {
        const auto lower_name = ascii_lower(path.filename().string());
        if (ends_with_lower(lower_name, ".arc") || ends_with_lower(lower_name, ".ark")) {
            return ArchiveFormat::Arc;
        }
    }
    if (matches_signature_at(bytes, 2U, {'-', 'l', 'h'})) {
        return ArchiveFormat::Lha;
    }
    if (starts_with_signature(bytes, {'M', 'S', 'W', 'I', 'M', 0x00, 0x00, 0x00})) {
        const auto lower_name = ascii_lower(path.filename().string());
        if (ends_with_lower(lower_name, ".swm")) {
            return ArchiveFormat::SplitWim;
        }
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
        return ends_with_lower(lower_name, ".deb") ? ArchiveFormat::Deb : ArchiveFormat::Ar;
    }
    if (matches_signature_at(bytes, 257U, {'u', 's', 't', 'a', 'r'})) {
        return ArchiveFormat::Tar;
    }
    if (matches_signature_at(bytes, 0x8001U, {'C', 'D', '0', '0', '1'}) ||
        matches_signature_at(bytes, 0x8801U, {'C', 'D', '0', '0', '1'})) {
        return ArchiveFormat::Iso;
    }
    if (has_uue_begin_line(bytes)) {
        return ArchiveFormat::Uue;
    }
    return ArchiveFormat::Unknown;
}

// Purpose: Detect archive format from common single and compound extensions.
// Inputs: `path` is the archive path being classified.
// Outputs: Returns a recognized extension format or unknown.
ArchiveFormat detect_by_extension(const std::filesystem::path& path) {
    const auto name = ascii_lower(path.filename().string());
    for (const auto& mapping : kExtensionFormats) {
        if (ends_with_lower(name, mapping.extension)) {
            return mapping.format;
        }
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

// Purpose: Parse a CLI/user format token without inspecting a file.
// Inputs: `token` is a case-insensitive value such as `suzip`, `zip`, `zipx`, or `auto`.
// Outputs: Returns the matching format or empty when the token is not a known archive format.
std::optional<ArchiveFormat> parse_archive_format_token(std::string_view token) {
    auto lowered = ascii_lower(std::string(token));
    if (lowered == "tgz") {
        lowered = "tar.gz";
    } else if (lowered == "tbz" || lowered == "tbz2") {
        lowered = "tar.bz2";
    } else if (lowered == "txz") {
        lowered = "tar.xz";
    } else if (lowered == "tlz") {
        lowered = "tar.lz";
    } else if (lowered == "tzst") {
        lowered = "tar.zst";
    } else if (lowered == "cpgz") {
        lowered = "cpio.gz";
    } else if (lowered == "zstd") {
        lowered = "zst";
    } else if (lowered == "gzip") {
        lowered = "gz";
    } else if (lowered == "bzip2") {
        lowered = "bz2";
    } else if (lowered == "lzip") {
        lowered = "lz";
    } else if (lowered == "compress" || lowered == "unix-compress") {
        lowered = "z";
    } else if (lowered == "base64") {
        lowered = "b64";
    } else if (lowered == "lzh") {
        lowered = "lha";
    } else if (lowered == "uu" || lowered == "uuencode") {
        lowered = "uue";
    } else if (lowered == "ark") {
        lowered = "arc";
    }
    const auto it = std::ranges::find_if(kFormatRegistry, [&](const ArchiveFormatInfo& info) {
        return lowered == info.key;
    });
    if (it == kFormatRegistry.end()) {
        return std::nullopt;
    }
    return it->format;
}

// Purpose: Detect a real archive format from path hints, leading magic, and native SUZIP footer/index magic.
// Inputs: `archive_path` is the candidate file to classify without mutating it.
// Outputs: Returns the detected format or `ArchiveFormat::Unknown` when no supported signature or extension matches.
ArchiveFormat detect_archive_format(const std::filesystem::path& archive_path) {
    if (has_excluded_zip_container_extension(archive_path)) {
        return ArchiveFormat::Unknown;
    }
    const auto by_extension = detect_by_extension(archive_path);
    if (by_extension == ArchiveFormat::TarGzip ||
        by_extension == ArchiveFormat::TarBzip2 ||
        by_extension == ArchiveFormat::TarXz ||
        by_extension == ArchiveFormat::TarLzip ||
        by_extension == ArchiveFormat::TarZstd ||
        by_extension == ArchiveFormat::CpioGzip) {
        return by_extension;
    }
    const auto by_magic = detect_by_magic(read_probe_bytes(archive_path), archive_path);
    if (by_magic != ArchiveFormat::Unknown) {
        return by_magic;
    }
    if (has_suzip_footer_signature(archive_path)) {
        return ArchiveFormat::SuperZip;
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
