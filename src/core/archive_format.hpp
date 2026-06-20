#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace superzip {

enum class ArchiveFormat {
    Unknown,
    Auto,
    SuperZip,
    Zip,
    Zipx,
    SevenZip,
    Rar,
    Tar,
    TarGzip,
    TarBzip2,
    TarXz,
    TarLzip,
    TarZstd,
    Gzip,
    UnixCompress,
    Base64,
    Bzip2,
    Xz,
    Lzma,
    Lzip,
    Zstd,
    Cab,
    Iso,
    Cpio,
    CpioGzip,
    Ar,
    Arj,
    Arc,
    Hqx,
    MacBinary,
    Xxe,
    Uue,
    Lha,
    Wim,
    SplitWim,
    Xar,
    Deb,
    Rpm,
};

struct ArchiveFormatInfo {
    ArchiveFormat format = ArchiveFormat::Unknown;
    const char* key = "unknown";
    const char* display_name = "Unknown archive";
    const char* extensions = "";
    bool can_create = false;
    bool can_extract = false;
    bool gpu_native = false;
    bool bundled_native = false;
};

struct ArchiveFormatExtensionInfo {
    ArchiveFormat format = ArchiveFormat::Unknown;
    const char* extension = "";
    const char* display_name = "Unknown archive";
    bool can_create = false;
    bool can_extract = false;
};

// Purpose: Return the static registry of real archive/container formats known to SuperZip.
// Inputs: None.
// Outputs: Returns immutable metadata for CLI, GUI, and documentation-oriented format listing.
std::span<const ArchiveFormatInfo> archive_format_registry();

// Purpose: Return one display row per supported extension.
// Inputs: None.
// Outputs: Returns immutable extension-specific metadata for GUI and tests.
std::span<const ArchiveFormatExtensionInfo> archive_format_extension_registry();

// Purpose: Look up display and support metadata for one archive format.
// Inputs: `format` is a recognized archive format enum.
// Outputs: Returns registry metadata; unknown inputs map to the unknown entry.
const ArchiveFormatInfo& archive_format_info(ArchiveFormat format);

// Purpose: Look up extension-specific display metadata by exact extension.
// Inputs: `extension` is a case-insensitive supported extension such as `.zip` or `.tar.gz`.
// Outputs: Returns the matching row, or an unknown row when the extension is not registered.
const ArchiveFormatExtensionInfo& archive_format_extension_info_for_extension(std::string_view extension);

// Purpose: Look up the extension-specific display metadata for a detected archive path.
// Inputs: `format` is the detected format and `archive_path` supplies the actual filename extension.
// Outputs: Returns the matching display row, or the canonical row for `format` when detection used magic only.
const ArchiveFormatExtensionInfo& archive_format_extension_info_for_path(ArchiveFormat format,
                                                                         const std::filesystem::path& archive_path);

// Purpose: Parse a CLI/user format token without inspecting a file.
// Inputs: `token` is a case-insensitive value such as `suzip`, `zip`, `tar`, or `auto`.
// Outputs: Returns the matching format or empty when the token is not a known archive format.
std::optional<ArchiveFormat> parse_archive_format_token(std::string_view token);

// Purpose: Detect a real archive format from filename extensions and bounded magic-byte probing.
// Inputs: `archive_path` is the candidate archive file; it may be missing for extension-only output decisions.
// Outputs: Returns the detected format or `ArchiveFormat::Unknown` without throwing for unreadable probes.
ArchiveFormat detect_archive_format(const std::filesystem::path& archive_path);

// Purpose: Detect a supported archive format from filename extensions only.
// Inputs: `archive_path` is the candidate file path; no filesystem or content reads are performed.
// Outputs: Returns the extension-mapped format, or `ArchiveFormat::Unknown` for unsupported/excluded extensions.
ArchiveFormat detect_archive_format_by_extension(const std::filesystem::path& archive_path);

// Purpose: Render supported format keys for help text.
// Inputs: `include_auto` controls whether the synthetic `auto` token is listed.
// Outputs: Returns a comma-separated token list.
std::string archive_format_key_list(bool include_auto);

}  // namespace superzip
