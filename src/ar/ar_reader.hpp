#pragma once

#include "core/archive_name_encoding.hpp"

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

namespace flitzzip::detail {

// Purpose: Own a validated AR member's decoded name and bounded payload location.
// Inputs: The reader supplies archive-relative bytes and an absolute input-stream offset.
// Outputs: Describes a regular file; no stream or caller buffer is retained.
struct ArEntryMetadata {
    std::string path;
    std::uint64_t size = 0;
    std::uint64_t payload_offset = 0;
};

// Purpose: Retain the complete validated AR publication plan independently of the input stream.
// Inputs: Decoded member metadata accumulated within archive resource limits.
// Outputs: Owns the file entries, aggregate payload bytes and retained pathname budget.
struct ArScanResult {
    std::vector<ArEntryMetadata> entries;
    std::uint64_t total_file_bytes = 0;
    std::uint64_t path_metadata_bytes = 0;
};

// Purpose: Validate an entire bounded AR stream before the filesystem adapter publishes any entries.
// Inputs: Input starts at offset zero; archive_size is its independently established extent; encoding selects names.
// Outputs: Owns validated metadata or throws on invalid framing, unsafe paths, resource limits or incomplete input.
// The borrowed seek-position-aware stream must remain alive throughout this synchronous call.
ArScanResult scan_ar_stream(std::istream& input, std::uint64_t archive_size, ArchivePathEncoding encoding);

}  // namespace flitzzip::detail
