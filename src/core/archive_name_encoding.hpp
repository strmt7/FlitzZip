#pragma once

#include "core/path_safety.hpp"

#include <string>
#include <string_view>
#include <array>

namespace superzip {

struct ArchiveNameEncodingChoice {
    ArchivePathEncoding encoding;
    std::string_view key;
    std::wstring_view label;
};

inline constexpr std::array kArchiveNameEncodingChoices{
    ArchiveNameEncodingChoice{ArchivePathEncoding::Utf8, "utf8", L"UTF-8"},
    ArchiveNameEncodingChoice{ArchivePathEncoding::HostCodePage, "system", L"Windows ANSI"},
};

// Purpose: Resolve a stable CLI encoding token using the shared GUI option registry.
// Inputs: An exact supported key, without automatic encoding detection.
// Outputs: Returns a supported encoding or throws ArchiveError for an unknown token.
ArchivePathEncoding parse_archive_name_encoding(std::string_view token);

// Purpose: Validate a caller-selected encoding for archive formats without an encoding marker.
// Inputs: `encoding` is an explicit choice; host-code-page mode requires Windows.
// Outputs: Throws ArchiveError for unsupported choices before extraction creates output.
void validate_archive_name_encoding(ArchivePathEncoding encoding);

// Purpose: Decode unmarked archive member names to the internal UTF-8 representation.
// Inputs: Bounded raw metadata and an explicit encoding, without automatic fallback or replacement characters.
// Outputs: Returns strict UTF-8 within path limits; throws ArchiveError for malformed or excessive metadata.
std::string decode_archive_name(std::string_view raw, ArchivePathEncoding encoding);

}  // namespace superzip
