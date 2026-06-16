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
    SevenZip,
    Rar,
    Tar,
    TarGzip,
    TarBzip2,
    TarXz,
    TarZstd,
    Gzip,
    UnixCompress,
    Bzip2,
    Xz,
    Zstd,
    Cab,
    Iso,
    Cpio,
    Ar,
    Arj,
    Lha,
    Wim,
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

// Purpose: Return the static registry of real archive/container formats known to SuperZip.
// Inputs: None.
// Outputs: Returns immutable metadata for CLI, GUI, and documentation-oriented format listing.
std::span<const ArchiveFormatInfo> archive_format_registry();

// Purpose: Look up display and support metadata for one archive format.
// Inputs: `format` is a recognized archive format enum.
// Outputs: Returns registry metadata; unknown inputs map to the unknown entry.
const ArchiveFormatInfo& archive_format_info(ArchiveFormat format);

// Purpose: Parse a CLI/user format token without inspecting a file.
// Inputs: `token` is a case-insensitive value such as `suzip`, `zip`, `tar`, or `auto`.
// Outputs: Returns the matching format or empty when the token is not a known archive format.
std::optional<ArchiveFormat> parse_archive_format_token(std::string_view token);

// Purpose: Detect a real archive format from filename extensions and bounded magic-byte probing.
// Inputs: `archive_path` is the candidate archive file; it may be missing for extension-only output decisions.
// Outputs: Returns the detected format or `ArchiveFormat::Unknown` without throwing for unreadable probes.
ArchiveFormat detect_archive_format(const std::filesystem::path& archive_path);

// Purpose: Render supported format keys for help text.
// Inputs: `include_auto` controls whether the synthetic `auto` token is listed.
// Outputs: Returns a comma-separated token list.
std::string archive_format_key_list(bool include_auto);

}  // namespace superzip
