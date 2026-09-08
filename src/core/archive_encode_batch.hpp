#pragma once

#include "core/archive.hpp"
#include "core/archive_index.hpp"
#include "core/file_manifest.hpp"

#include <cstddef>
#include <iosfwd>
#include <span>

namespace superzip {

inline constexpr std::size_t kArchiveEncodeBatchFiles = 64;
inline constexpr std::uint64_t kArchiveEncodeBatchBytes = 8U * 1024U * 1024U;

// Purpose: Select a bounded consecutive prefix of independently encodable small regular files.
// Inputs: entries are the remaining manifest; options supplies validated chunk/block bounds and backend policy.
// Outputs: Returns zero unless at least two files fit both byte/count bounds; forced CPU retains its existing path.
std::size_t archive_encode_batch_count(std::span<const ManifestEntry> entries, const CompressOptions& options);

// Purpose: Read locked small sources and append their independently encoded payloads to a staging archive.
// Inputs: entries must be a complete eligible batch; options controls the codec; output/index receive bytes/metadata;
// stats, block_count, progress, callback, and telemetry retain operation-level accounting.
// Outputs: Appends one independent entry per source, retaining source locks through writes; throws on any failure.
void compress_manifest_batch(std::span<const ManifestEntry> entries, const CompressOptions& options,
                             std::ostream& output, ArchiveIndex& index, OperationStats& stats,
                             std::uint64_t& block_count, ProgressState& progress, const ProgressCallback& callback,
                             const std::shared_ptr<GpuTelemetry>& telemetry);

}  // namespace superzip
