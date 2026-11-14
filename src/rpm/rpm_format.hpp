#pragma once

#include "core/progress.hpp"
#include <cstdint>
#include <filesystem>
#include <string>

namespace flitzzip {

enum class RpmPayloadCompression {
    None,
    Gzip,
    Bzip2,
    Xz,
    Zstd,
};

struct RpmPayloadInfo {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    RpmPayloadCompression compression = RpmPayloadCompression::None;
    std::string payload_format;
    std::string payload_compressor;
};

// Purpose: Parse the RPM lead, signature header, package header, and payload descriptor.
// Inputs: `archive_path` is an RPM package candidate; `checkpoint` is synchronously borrowed across bounded header
// reads and record checks. Its original exception propagates without retention.
// Outputs: Returns payload offset/size/compression metadata; throws `ArchiveError` for malformed or unsupported RPM
// containers.
RpmPayloadInfo scan_rpm_payload(const std::filesystem::path& archive_path, const ProgressCheckpoint& checkpoint = {});

}  // namespace flitzzip
