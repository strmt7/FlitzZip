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

constexpr std::array<ArchiveFormatInfo, 38> kFormatRegistry{{
    {ArchiveFormat::Unknown, "unknown", "Unknown archive", "", false, false, false, false},
    {ArchiveFormat::Auto, "auto", "Automatic detection", "", false, true, false, false},
    {ArchiveFormat::SuperZip, "suzip", "SuperZip (.suzip)", ".suzip", true, true, true, true},
    {ArchiveFormat::Zip, "zip", "ZIP (.zip)", ".zip", true, true, false, true},
    {ArchiveFormat::Zipx, "zipx", "ZIPX (.zipx)", ".zipx", false, true, false, true},
    {ArchiveFormat::SevenZip, "7z", "7z (.7z)", ".7z", false, true, false, true},
    {ArchiveFormat::Rar, "rar", "RAR (.rar)", ".rar", false, false, false, false},
    {ArchiveFormat::Tar, "tar", "TAR (.tar)", ".tar", true, true, false, true},
    {ArchiveFormat::TarGzip, "tar.gz", "TAR.GZ (.tar.gz)", ".tar.gz,.tgz", true, true, false, true},
    {ArchiveFormat::TarBzip2, "tar.bz2", "TAR.BZ2 (.tar.bz2)", ".tar.bz2,.tbz,.tbz2", true, true, false, true},
    {ArchiveFormat::TarXz, "tar.xz", "TAR.XZ (.tar.xz)", ".tar.xz,.txz", false, true, false, true},
    {ArchiveFormat::TarLzip, "tar.lz", "TAR.LZ (.tar.lz)", ".tar.lz,.tlz", false, true, false, true},
    {ArchiveFormat::TarZstd, "tar.zst", "TAR.ZST (.tar.zst)", ".tar.zst,.tzst", true, true, false, true},
    {ArchiveFormat::Gzip, "gz", "Gzip (.gz)", ".gz", true, true, false, true},
    {ArchiveFormat::UnixCompress, "z", "Unix Compress (.Z)", ".Z", true, true, false, true},
    {ArchiveFormat::Base64, "b64", "Base64 (.b64)", ".b64", false, true, false, true},
    {ArchiveFormat::Bzip2, "bz2", "Bzip2 (.bz2)", ".bz2", true, true, false, true},
    {ArchiveFormat::Xz, "xz", "XZ (.xz)", ".xz", false, true, false, true},
    {ArchiveFormat::Lzma, "lzma", "LZMA (.lzma)", ".lzma", false, true, false, true},
    {ArchiveFormat::Lzip, "lz", "lzip (.lz)", ".lz", false, true, false, true},
    {ArchiveFormat::Zstd, "zst", "Zstandard (.zst)", ".zst,.zstd", true, true, false, true},
    {ArchiveFormat::Cab, "cab", "CAB (.cab)", ".cab", false, true, false, true},
    {ArchiveFormat::Iso, "iso", "ISO image (.iso)", ".iso", false, true, false, true},
    {ArchiveFormat::Cpio, "cpio", "CPIO (.cpio)", ".cpio", true, true, false, true},
    {ArchiveFormat::CpioGzip, "cpio.gz", "CPIO.GZ (.cpio.gz)", ".cpio.gz,.cpgz", true, true, false, true},
    {ArchiveFormat::Ar, "ar", "Unix AR (.ar)", ".ar", true, true, false, true},
    {ArchiveFormat::Arj, "arj", "ARJ (.arj)", ".arj", false, true, false, true},
    {ArchiveFormat::Arc, "arc", "SEA ARC/ARK (.arc)", ".arc,.ark", false, true, false, true},
    {ArchiveFormat::Hqx, "hqx", "BinHex 4.0 (.hqx)", ".hqx", false, true, false, true},
    {ArchiveFormat::MacBinary, "macbinary", "MacBinary (.macbin)", ".macbin", false, true, false, true},
    {ArchiveFormat::Xxe, "xxe", "XXEncode (.xxe)", ".xxe", false, true, false, true},
    {ArchiveFormat::Uue, "uue", "UUencode (.uue)", ".uue,.uu", false, true, false, true},
    {ArchiveFormat::Lha, "lha", "LHA/LZH (.lha)", ".lha,.lzh", false, true, false, true},
    {ArchiveFormat::Wim, "wim", "Windows Imaging (.wim)", ".wim", false, true, false, true},
    {ArchiveFormat::SplitWim, "swm", "Split WIM (.swm)", ".swm", false, false, false, false},
    {ArchiveFormat::Xar, "xar", "XAR (.xar)", ".xar", false, true, false, true},
    {ArchiveFormat::Deb, "deb", "Debian package (.deb)", ".deb", false, true, false, true},
    {ArchiveFormat::Rpm, "rpm", "RPM package (.rpm)", ".rpm", false, true, false, true},
}};

constexpr ArchiveFormatExtensionInfo kUnknownExtensionInfo{};

constexpr std::array<ArchiveFormatExtensionInfo, 48> kFormatExtensionRegistry{{
    {ArchiveFormat::SuperZip, ".suzip", "SuperZip (.suzip)", true, true},
    {ArchiveFormat::Zip, ".zip", "ZIP (.zip)", true, true},
    {ArchiveFormat::Zipx, ".zipx", "ZIPX (.zipx)", false, true},
    {ArchiveFormat::SevenZip, ".7z", "7z (.7z)", false, true},
    {ArchiveFormat::Rar, ".rar", "RAR (.rar)", false, false},
    {ArchiveFormat::TarGzip, ".tar.gz", "TAR.GZ (.tar.gz)", true, true},
    {ArchiveFormat::TarGzip, ".tgz", "TAR.GZ (.tgz)", true, true},
    {ArchiveFormat::TarBzip2, ".tar.bz2", "TAR.BZ2 (.tar.bz2)", true, true},
    {ArchiveFormat::TarBzip2, ".tbz2", "TAR.BZ2 (.tbz2)", true, true},
    {ArchiveFormat::TarBzip2, ".tbz", "TAR.BZ2 (.tbz)", true, true},
    {ArchiveFormat::TarXz, ".tar.xz", "TAR.XZ (.tar.xz)", false, true},
    {ArchiveFormat::TarXz, ".txz", "TAR.XZ (.txz)", false, true},
    {ArchiveFormat::TarLzip, ".tar.lz", "TAR.LZ (.tar.lz)", false, true},
    {ArchiveFormat::TarLzip, ".tlz", "TAR.LZ (.tlz)", false, true},
    {ArchiveFormat::TarZstd, ".tar.zst", "TAR.ZST (.tar.zst)", true, true},
    {ArchiveFormat::TarZstd, ".tzst", "TAR.ZST (.tzst)", true, true},
    {ArchiveFormat::CpioGzip, ".cpio.gz", "CPIO.GZ (.cpio.gz)", true, true},
    {ArchiveFormat::CpioGzip, ".cpgz", "CPIO.GZ (.cpgz)", true, true},
    {ArchiveFormat::Tar, ".tar", "TAR (.tar)", true, true},
    {ArchiveFormat::Gzip, ".gz", "Gzip (.gz)", true, true},
    {ArchiveFormat::UnixCompress, ".Z", "Unix Compress (.Z)", true, true},
    {ArchiveFormat::Base64, ".b64", "Base64 (.b64)", false, true},
    {ArchiveFormat::Bzip2, ".bz2", "Bzip2 (.bz2)", true, true},
    {ArchiveFormat::Xz, ".xz", "XZ (.xz)", false, true},
    {ArchiveFormat::Lzma, ".lzma", "LZMA (.lzma)", false, true},
    {ArchiveFormat::Lzip, ".lz", "lzip (.lz)", false, true},
    {ArchiveFormat::Zstd, ".zst", "Zstandard (.zst)", true, true},
    {ArchiveFormat::Zstd, ".zstd", "Zstandard (.zstd)", true, true},
    {ArchiveFormat::Cab, ".cab", "CAB (.cab)", false, true},
    {ArchiveFormat::Iso, ".iso", "ISO image (.iso)", false, true},
    {ArchiveFormat::Cpio, ".cpio", "CPIO (.cpio)", true, true},
    {ArchiveFormat::Ar, ".ar", "Unix AR (.ar)", true, true},
    {ArchiveFormat::Arj, ".arj", "ARJ (.arj)", false, true},
    {ArchiveFormat::Arc, ".arc", "SEA ARC/ARK (.arc)", false, true},
    {ArchiveFormat::Arc, ".ark", "SEA ARC/ARK (.ark)", false, true},
    {ArchiveFormat::Hqx, ".hqx", "BinHex 4.0 (.hqx)", false, true},
    {ArchiveFormat::MacBinary, ".macbin", "MacBinary (.macbin)", false, true},
    {ArchiveFormat::MacBinary, ".bin", "MacBinary (.bin)", false, true},
    {ArchiveFormat::Xxe, ".xxe", "XXEncode (.xxe)", false, true},
    {ArchiveFormat::Uue, ".uue", "UUencode (.uue)", false, true},
    {ArchiveFormat::Uue, ".uu", "UUencode (.uu)", false, true},
    {ArchiveFormat::Lha, ".lha", "LHA/LZH (.lha)", false, true},
    {ArchiveFormat::Lha, ".lzh", "LHA/LZH (.lzh)", false, true},
    {ArchiveFormat::Wim, ".wim", "Windows Imaging (.wim)", false, true},
    {ArchiveFormat::SplitWim, ".swm", "Split WIM (.swm)", false, false},
    {ArchiveFormat::Xar, ".xar", "XAR (.xar)", false, true},
    {ArchiveFormat::Deb, ".deb", "Debian package (.deb)", false, true},
    {ArchiveFormat::Rpm, ".rpm", "RPM package (.rpm)", false, true},
}};

constexpr std::array<ExtensionFormatMapping, 47> kExtensionFormats{{
    {".suzip", ArchiveFormat::SuperZip},   {".zip", ArchiveFormat::Zip},
    {".zipx", ArchiveFormat::Zipx},        {".7z", ArchiveFormat::SevenZip},
    {".rar", ArchiveFormat::Rar},          {".tar.gz", ArchiveFormat::TarGzip},
    {".tgz", ArchiveFormat::TarGzip},      {".tar.bz2", ArchiveFormat::TarBzip2},
    {".tbz", ArchiveFormat::TarBzip2},     {".tbz2", ArchiveFormat::TarBzip2},
    {".tar.xz", ArchiveFormat::TarXz},     {".txz", ArchiveFormat::TarXz},
    {".tar.lz", ArchiveFormat::TarLzip},   {".tlz", ArchiveFormat::TarLzip},
    {".tar.zst", ArchiveFormat::TarZstd},  {".tzst", ArchiveFormat::TarZstd},
    {".cpio.gz", ArchiveFormat::CpioGzip}, {".cpgz", ArchiveFormat::CpioGzip},
    {".tar", ArchiveFormat::Tar},          {".gz", ArchiveFormat::Gzip},
    {".z", ArchiveFormat::UnixCompress},   {".b64", ArchiveFormat::Base64},
    {".bz2", ArchiveFormat::Bzip2},        {".xz", ArchiveFormat::Xz},
    {".lzma", ArchiveFormat::Lzma},        {".lz", ArchiveFormat::Lzip},
    {".zst", ArchiveFormat::Zstd},         {".zstd", ArchiveFormat::Zstd},
    {".cab", ArchiveFormat::Cab},          {".iso", ArchiveFormat::Iso},
    {".cpio", ArchiveFormat::Cpio},        {".ar", ArchiveFormat::Ar},
    {".arj", ArchiveFormat::Arj},          {".arc", ArchiveFormat::Arc},
    {".ark", ArchiveFormat::Arc},          {".hqx", ArchiveFormat::Hqx},
    {".macbin", ArchiveFormat::MacBinary}, {".xxe", ArchiveFormat::Xxe},
    {".uue", ArchiveFormat::Uue},          {".uu", ArchiveFormat::Uue},
    {".lha", ArchiveFormat::Lha},          {".lzh", ArchiveFormat::Lha},
    {".wim", ArchiveFormat::Wim},          {".swm", ArchiveFormat::SplitWim},
    {".xar", ArchiveFormat::Xar},          {".deb", ArchiveFormat::Deb},
    {".rpm", ArchiveFormat::Rpm},
}};

// Purpose: Convert a string to lowercase ASCII for extension and token matching.
// Inputs: `value` is an archive token or filename.
// Outputs: Returns a lowercased copy; non-ASCII bytes are preserved except C-locale ASCII folds.
std::string ascii_lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

// Purpose: Test whether a string ends in a case-normalized suffix.
// Inputs: `value` and `suffix` are lowercase strings.
// Outputs: Returns true when `suffix` is a byte suffix of `value`.
bool ends_with_lower(const std::string& value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Purpose: Detect ZIP-based application/document containers that must not be treated as archives.
// Inputs: `path` is the candidate archive path.
// Outputs: Returns true when a ZIP-magic file uses an extension outside SuperZip archive support.
bool has_excluded_zip_container_extension(const std::filesystem::path& path) {
    const auto name = ascii_lower(path.filename().string());
    constexpr std::array excluded = {
        ".docx", ".pptx", ".xlsx", ".odt", ".ods", ".odp", ".jar", ".war", ".ear", ".apk", ".ipa", ".xpi", ".cbz",
    };
    return std::ranges::any_of(excluded, [&](std::string_view extension) { return ends_with_lower(name, extension); });
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
        const auto footer_magic = read_u32(input);
        const auto footer_version = read_u32(input);
        if (footer_magic != kSuperZipFooterMagic || footer_version < kSuperZipMinReadableVersion ||
            footer_version > kSuperZipVersion) {
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

// Purpose: Detect an XXEncode begin line without stealing generic UUencode text probes.
// Inputs: `bytes` contains the file prefix and `path` supplies the `.xxe` extension hint for short payloads.
// Outputs: Returns true for `.xxe` files with a strict begin line or for extensionless probes with an XXE-only length
// marker.
bool has_xxe_begin_line(std::span<const unsigned char> bytes, const std::filesystem::path& path) {
    const bool extension_hint = ends_with_lower(ascii_lower(path.filename().string()), ".xxe");
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
        const bool begin_matches =
            line_size > prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), bytes.begin() + static_cast<std::ptrdiff_t>(line_start));
        if (begin_matches) {
            auto cursor = line_start + prefix.size();
            const auto end = line_start + line_size;
            while (cursor < end && bytes[cursor] >= '0' && bytes[cursor] <= '7') {
                ++cursor;
            }
            const bool strict_header = cursor > line_start + prefix.size() && cursor + 1U < end && bytes[cursor] == ' ';
            if (strict_header) {
                if (extension_hint) {
                    return true;
                }
                const auto next_start = line_end == bytes.size() ? bytes.size() : line_end + 1U;
                if (next_start >= bytes.size()) {
                    return false;
                }
                const auto marker = bytes[next_start];
                return marker == static_cast<unsigned char>('h') || marker == static_cast<unsigned char>('+');
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

// Purpose: Detect a BinHex 4.0 transfer comment in a bounded text probe.
// Inputs: `bytes` contains the file prefix; the encoded body is not decoded during format probing.
// Outputs: Returns true when the standard BinHex comment and a following payload colon appear in the probe.
bool has_hqx_comment_marker(std::span<const unsigned char> bytes) {
    constexpr std::string_view comment = "(This file must be converted with BinHex 4.0)";
    auto it = std::search(bytes.begin(), bytes.end(), comment.begin(), comment.end(),
                          [](unsigned char lhs, char rhs) { return lhs == static_cast<unsigned char>(rhs); });
    if (it == bytes.end()) {
        return false;
    }
    it += static_cast<std::ptrdiff_t>(comment.size());
    return std::find(it, bytes.end(), static_cast<unsigned char>(':')) != bytes.end();
}

// Purpose: Read a big-endian 16-bit integer from a bounded probe buffer.
// Inputs: `bytes` contains at least `offset + 2` bytes and `offset` is the first byte.
// Outputs: Returns the decoded integer.
std::uint16_t read_probe_be_u16(std::span<const unsigned char> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8U) |
                                      static_cast<std::uint16_t>(bytes[offset + 1U]));
}

// Purpose: Read a big-endian 32-bit integer from a bounded probe buffer.
// Inputs: `bytes` contains at least `offset + 4` bytes and `offset` is the first byte.
// Outputs: Returns the decoded integer.
std::uint32_t read_probe_be_u32(std::span<const unsigned char> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3U]);
}

// Purpose: Update a CRC-16/XMODEM register with one byte for MacBinary header probing.
// Inputs: `crc` is the mutable register and `byte` is the next header byte.
// Outputs: Mutates `crc`.
void update_probe_crc16_xmodem(std::uint16_t& crc, std::uint8_t byte) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(byte) << 8U));
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
        if ((crc & 0x8000U) != 0U) {
            crc = static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U);
        } else {
            crc = static_cast<std::uint16_t>(crc << 1U);
        }
    }
}

// Purpose: Compute the MacBinary II/III header CRC over probe bytes 0 through 123.
// Inputs: `bytes` contains at least one full 128-byte MacBinary header.
// Outputs: Returns the expected stored CRC value.
std::uint16_t macbinary_probe_crc(std::span<const unsigned char> bytes) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < 124U; ++i) {
        update_probe_crc16_xmodem(crc, static_cast<std::uint8_t>(bytes[i]));
    }
    return crc;
}

// Purpose: Detect a strongly identifiable MacBinary II/III header in a generic binary probe.
// Inputs: `bytes` contains the file prefix and may be shorter than a full header.
// Outputs: Returns true only when required zero fields, filename bytes, version/signature markers, and header CRC
// agree.
bool has_macbinary_header_marker(std::span<const unsigned char> bytes) {
    if (bytes.size() < 128U || bytes[0] != 0U || bytes[74] != 0U || bytes[82] != 0U) {
        return false;
    }
    const auto name_length = static_cast<std::size_t>(bytes[1]);
    if (name_length == 0U || name_length > 63U) {
        return false;
    }
    for (std::size_t i = 0; i < name_length; ++i) {
        const auto byte = bytes[2U + i];
        if (byte < 0x20U || byte > 0x7EU || byte == static_cast<unsigned char>(':')) {
            return false;
        }
    }
    const bool versioned = (bytes[122] == 0x81U || bytes[122] == 0x82U) && (bytes[123] == 0x81U || bytes[123] == 0x82U);
    const bool signed_v3 =
        bytes[102] == static_cast<unsigned char>('m') && bytes[103] == static_cast<unsigned char>('B') &&
        bytes[104] == static_cast<unsigned char>('I') && bytes[105] == static_cast<unsigned char>('N');
    if (!versioned && !signed_v3) {
        return false;
    }
    if ((read_probe_be_u32(bytes, 83U) & 0x80000000U) != 0U || (read_probe_be_u32(bytes, 87U) & 0x80000000U) != 0U) {
        return false;
    }
    return read_probe_be_u16(bytes, 124U) == macbinary_probe_crc(bytes);
}

// Purpose: Match a fixed byte signature at an arbitrary offset.
// Inputs: `bytes` is the file probe, `offset` is the byte offset, and `signature` is the magic sequence.
// Outputs: Returns true when all signature bytes match at `offset`.
bool matches_signature_at(std::span<const unsigned char> bytes, std::size_t offset,
                          std::initializer_list<unsigned char> signature) {
    if (offset > bytes.size() || bytes.size() - offset < signature.size()) {
        return false;
    }
    return std::equal(signature.begin(), signature.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Purpose: Detect single-stream compression and text-transfer formats with stable prefix markers.
// Inputs: `bytes` contains a bounded prefix probe and `path` supplies compound extension context for lzip TAR wrappers.
// Outputs: Returns a concrete single-stream format or unknown when no stream marker matches.
ArchiveFormat detect_stream_magic(std::span<const unsigned char> bytes, const std::filesystem::path& path) {
    if (starts_with_signature(bytes, {0x1F, 0x8B})) {
        return ArchiveFormat::Gzip;
    }
    if (starts_with_signature(bytes, {0x1F, 0x9D})) {
        return ArchiveFormat::UnixCompress;
    }
    if (has_base64_begin_line(bytes)) {
        return ArchiveFormat::Base64;
    }
    if (has_hqx_comment_marker(bytes)) {
        return ArchiveFormat::Hqx;
    }
    if (has_macbinary_header_marker(bytes)) {
        return ArchiveFormat::MacBinary;
    }
    if (starts_with_signature(bytes, {'B', 'Z', 'h'})) {
        return ArchiveFormat::Bzip2;
    }
    if (starts_with_signature(bytes, {0xFD, '7', 'z', 'X', 'Z', 0x00})) {
        return ArchiveFormat::Xz;
    }
    if (starts_with_signature(bytes, {'L', 'Z', 'I', 'P'})) {
        const auto lower_name = ascii_lower(path.filename().string());
        return (ends_with_lower(lower_name, ".tar.lz") || ends_with_lower(lower_name, ".tlz")) ? ArchiveFormat::TarLzip
                                                                                               : ArchiveFormat::Lzip;
    }
    if (starts_with_signature(bytes, {0x28, 0xB5, 0x2F, 0xFD})) {
        return ArchiveFormat::Zstd;
    }
    return ArchiveFormat::Unknown;
}

// Purpose: Detect archive formats with stable magic bytes before extension fallback.
// Inputs: `bytes` contains a bounded prefix probe and `path` supplies extension context for ambiguous container magics.
// Outputs: Returns a detected format or unknown.
ArchiveFormat detect_by_magic(std::span<const unsigned char> bytes, const std::filesystem::path& path) {
    if (starts_with_signature(bytes, {'P', 'K', 0x03, 0x04}) || starts_with_signature(bytes, {'P', 'K', 0x05, 0x06}) ||
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
    if (const auto stream_format = detect_stream_magic(bytes, path); stream_format != ArchiveFormat::Unknown) {
        return stream_format;
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
    if (has_xxe_begin_line(bytes, path)) {
        return ArchiveFormat::Xxe;
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

// Purpose: Return the static registry of real archive/container formats known to SuperZip.
// Inputs: None.
// Outputs: Returns immutable metadata shared by CLI, GUI, and tests.
std::span<const ArchiveFormatInfo> archive_format_registry() {
    return kFormatRegistry;
}

// Purpose: Return one display row per supported extension.
// Inputs: None.
// Outputs: Returns immutable extension metadata shared by GUI selectors and tests.
std::span<const ArchiveFormatExtensionInfo> archive_format_extension_registry() {
    return kFormatExtensionRegistry;
}

const ArchiveFormatInfo& archive_format_info(ArchiveFormat format) {
    const auto it = std::ranges::find_if(kFormatRegistry,
                                         [format](const ArchiveFormatInfo& info) { return info.format == format; });
    return it == kFormatRegistry.end() ? kFormatRegistry.front() : *it;
}

// Purpose: Look up extension-specific display metadata by exact extension.
// Inputs: `extension` is a case-insensitive extension including its leading dot.
// Outputs: Returns a registered row, or an unknown row when the extension is unsupported.
const ArchiveFormatExtensionInfo& archive_format_extension_info_for_extension(std::string_view extension) {
    const auto lowered = ascii_lower(std::string(extension));
    const auto it = std::ranges::find_if(kFormatExtensionRegistry, [&](const ArchiveFormatExtensionInfo& info) {
        return ascii_lower(info.extension) == lowered;
    });
    return it == kFormatExtensionRegistry.end() ? kUnknownExtensionInfo : *it;
}

// Purpose: Return the canonical extension row for one archive format.
// Inputs: `format` is a concrete archive format.
// Outputs: Returns the first extension row for that format, or an unknown row when no extension row exists.
const ArchiveFormatExtensionInfo& canonical_extension_info_for_format(ArchiveFormat format) {
    const auto it = std::ranges::find_if(
        kFormatExtensionRegistry, [format](const ArchiveFormatExtensionInfo& info) { return info.format == format; });
    return it == kFormatExtensionRegistry.end() ? kUnknownExtensionInfo : *it;
}

// Purpose: Look up the extension-specific display metadata for a detected archive path.
// Inputs: `format` is the detected format and `archive_path` supplies the actual filename extension.
// Outputs: Returns the matching extension row, or the canonical row when the path was detected by magic only.
const ArchiveFormatExtensionInfo& archive_format_extension_info_for_path(ArchiveFormat format,
                                                                         const std::filesystem::path& archive_path) {
    if (format == ArchiveFormat::Unknown || format == ArchiveFormat::Auto) {
        return kUnknownExtensionInfo;
    }
    const auto name = ascii_lower(archive_path.filename().string());
    for (const auto& info : kFormatExtensionRegistry) {
        if (info.format == format && ends_with_lower(name, ascii_lower(info.extension))) {
            return info;
        }
    }
    return canonical_extension_info_for_format(format);
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
    } else if (lowered == "binhex" || lowered == "binhex4" || lowered == "binhex40") {
        lowered = "hqx";
    } else if (lowered == "macbin") {
        lowered = "macbinary";
    } else if (lowered == "xxencode") {
        lowered = "xxe";
    } else if (lowered == "uu" || lowered == "uuencode") {
        lowered = "uue";
    } else if (lowered == "ark") {
        lowered = "arc";
    }
    const auto it =
        std::ranges::find_if(kFormatRegistry, [&](const ArchiveFormatInfo& info) { return lowered == info.key; });
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
    if (by_extension == ArchiveFormat::TarGzip || by_extension == ArchiveFormat::TarBzip2 ||
        by_extension == ArchiveFormat::TarXz || by_extension == ArchiveFormat::TarLzip ||
        by_extension == ArchiveFormat::TarZstd || by_extension == ArchiveFormat::CpioGzip) {
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
