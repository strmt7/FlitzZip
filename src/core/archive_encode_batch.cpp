#include "core/archive_encode_batch.hpp"

#include "core/path_text.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

namespace superzip {
namespace {

// Purpose: Capture exact source bytes while keeping each validated source and parent chain pinned.
// Inputs: entries are a bounded eligible batch; locks retains ownership; progress/callback report successful reads.
// Outputs: Returns dense immutable input and populates locks; throws before encoding on any read or callback failure.
std::vector<std::byte> read_batch_sources(std::span<const ManifestEntry> entries,
                                          std::vector<ManifestSourceLock>& locks, ProgressState& progress,
                                          const ProgressCallback& callback) {
    std::size_t total = 0;
    for (const auto& entry : entries) {
        total += static_cast<std::size_t>(entry.size);
    }
    std::vector<std::byte> input(total);
    locks.reserve(entries.size());
    std::size_t offset = 0;
    for (const auto& entry : entries) {
        if (progress.cancelled()) {
            throw ArchiveError("operation cancelled");
        }
        progress.set_current(entry.archive_path);
        publish_progress(progress, callback);
        locks.push_back(lock_manifest_source(entry));
        std::ifstream source(entry.source_path, std::ios::binary);
        if (!source) {
            throw ArchiveError("cannot open source file: " + path_diagnostic_utf8(entry.source_path));
        }
        source.read(reinterpret_cast<char*>(input.data() + offset), static_cast<std::streamsize>(entry.size));
        if (!source || source.gcount() != static_cast<std::streamsize>(entry.size)) {
            throw ArchiveError("source file read was incomplete: " + path_diagnostic_utf8(entry.source_path));
        }
        offset += static_cast<std::size_t>(entry.size);
        progress.add_bytes(entry.size);
        publish_progress(progress, callback);
    }
    return input;
}

// Purpose: Preserve native per-file metadata when publishing one block from a shared codec payload.
// Inputs: source, block, payload, and checksum describe one encoded file; output receives only that file's bytes.
// Outputs: Returns an entry with zero-relative block offset; throws on inconsistent codec output or stream failure.
ArchiveEntry write_batch_entry(const ManifestEntry& source, BlockDescriptor block, std::span<const std::byte> payload,
                               std::uint32_t checksum, std::ostream& output) {
    if (block.uncompressed_len != source.size || block.encoded_offset > payload.size() ||
        block.encoded_len > payload.size() - block.encoded_offset) {
        throw ArchiveError("batched archive encoder returned inconsistent block bounds");
    }
    const auto position = output.tellp();
    if (position < 0) {
        throw ArchiveError("failed to query archive stream position");
    }
    const auto bytes = payload.subspan(static_cast<std::size_t>(block.encoded_offset), block.encoded_len);
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw ArchiveError("failed to write archive payload");
        }
    }
    block.encoded_offset = 0;
    return ArchiveEntry{.path = source.archive_path,
                        .directory = false,
                        .uncompressed_size = source.size,
                        .payload_offset = static_cast<std::uint64_t>(position),
                        .payload_size = bytes.size(),
                        .crc32 = checksum,
                        .blocks = {block}};
}

}  // namespace

// Purpose: Select consecutive small files without crossing directory, empty-file, or resource boundaries.
// Inputs: entries and options provide remaining validated source metadata and requested codec bounds.
// Outputs: Returns a count in [2,64], or zero for an ineligible prefix; allocates nothing and touches no files.
std::size_t archive_encode_batch_count(std::span<const ManifestEntry> entries, const CompressOptions& options) {
    if (options.force_cpu) {
        return 0;
    }
    const auto limit = std::min(options.chunk_size, kArchiveEncodeBatchBytes);
    std::uint64_t bytes = 0;
    std::size_t count = 0;
    for (const auto& entry : entries.first(std::min(entries.size(), kArchiveEncodeBatchFiles))) {
        if (entry.directory || entry.size == 0 || entry.size > options.block_size || entry.size > limit - bytes) {
            break;
        }
        bytes += entry.size;
        ++count;
    }
    return count >= 2 ? count : 0;
}

// Purpose: Encode and write one bounded multi-file batch without changing native format or independent CRCs.
// Inputs: entries/options define an eligible batch; remaining references belong to the active archive transaction.
// Outputs: Updates stream/index, GPU statistics, block count, and progress; leaves final publication to the caller.
void compress_manifest_batch(std::span<const ManifestEntry> entries, const CompressOptions& options,
                             std::ostream& output, ArchiveIndex& index, OperationStats& stats,
                             std::uint64_t& block_count, ProgressState& progress, const ProgressCallback& callback,
                             const std::shared_ptr<GpuTelemetry>& telemetry) {
    if (entries.empty() || archive_encode_batch_count(entries, options) != entries.size()) {
        throw ArchiveError("archive encode batch violates its file or byte limits");
    }
    if (block_count > kMaxArchiveBlocks || entries.size() > kMaxArchiveBlocks - block_count) {
        throw ArchiveError("archive requires too many total blocks");
    }
    std::vector<ManifestSourceLock> source_locks;
    auto input = read_batch_sources(entries, source_locks, progress, callback);
    std::vector<std::uint32_t> lengths;
    lengths.reserve(entries.size());
    for (const auto& entry : entries) {
        lengths.push_back(static_cast<std::uint32_t>(entry.size));
    }
    if (progress.cancelled()) {
        throw ArchiveError("operation cancelled");
    }
    const auto batch = encode_owned_block_batch(std::move(input), lengths,
                                                {.require_gpu = options.gpu_required,
                                                 .force_cpu = options.force_cpu,
                                                 .block_size = options.block_size,
                                                 .worker_count = 1,
                                                 .compression_level = options.compression_level,
                                                 .telemetry = telemetry});
    if (batch.encoded.blocks.size() != entries.size() || batch.block_crc32.size() != entries.size()) {
        throw ArchiveError("batched archive encoder returned an inconsistent file count");
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (progress.cancelled()) {
            throw ArchiveError("operation cancelled");
        }
        index.entries.push_back(write_batch_entry(entries[i], batch.encoded.blocks[i], batch.encoded.payload,
                                                  batch.block_crc32[i], output));
        ++block_count;
        progress.set_current(entries[i].archive_path);
        progress.finish_entry();
        publish_progress(progress, callback);
    }
    stats.gpu_used = stats.gpu_used || batch.encoded.gpu_used;
}

}  // namespace superzip
