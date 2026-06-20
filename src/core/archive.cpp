#include "core/archive.hpp"

#include "core/archive_index.hpp"
#include "core/checksum.hpp"
#include "core/file_manifest.hpp"
#include "core/file_publish.hpp"
#include "core/path_safety.hpp"
#include "core/result.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace superzip {
namespace {

constexpr std::uint64_t kArchiveFooterSize = 24;
constexpr std::size_t kFileStreamBufferBytes = 4U * 1024U * 1024U;

struct PipelineBudget {
    std::uint32_t workers = 1;
    std::uint32_t inflight_chunks = 1;
};

struct HostMemorySnapshot {
    std::uint64_t total_bytes = 0;
    std::uint64_t available_bytes = 0;
};

struct EncodedArchiveChunk {
    EncodedChunk encoded;
    std::uint32_t crc32 = 0;
    std::uint64_t uncompressed_size = 0;
};

struct PendingEncode {
    std::future<EncodedArchiveChunk> result;
};

struct DecodedChunk {
    std::vector<std::byte> bytes;
    std::uint32_t crc32 = 0;
    bool gpu_used = false;
};

struct PendingDecode {
    std::future<DecodedChunk> result;
};

struct DecodedCrcChunk {
    std::uint32_t crc32 = 0;
    std::uint64_t uncompressed_size = 0;
    bool gpu_used = false;
};

struct PendingDecodedCrc {
    std::future<DecodedCrcChunk> result;
};

struct DecodeStreamResult {
    bool gpu_used = false;
    std::uint32_t crc32 = 0;
};

struct ArchiveValidationSummary {
    std::uint64_t total_uncompressed_bytes = 0;
    std::uint64_t largest_decoded_block_bytes = 1;
};

// Purpose: Identify block kinds that carry bytes in the archive payload.
// Inputs: `kind` is a native SUZIP block encoding kind.
// Outputs: Returns true for block kinds whose `encoded_offset`/`encoded_len` reserve payload bytes.
bool block_has_payload(BlockKind kind) {
    return kind == BlockKind::Raw || kind == BlockKind::Deflate || kind == BlockKind::Pattern ||
           kind == BlockKind::GpuPrefix || kind == BlockKind::GpuAdaptivePrefix;
}

// Purpose: Create a bounded file-stream buffer for high-throughput archive I/O.
// Inputs: None.
// Outputs: Returns a caller-owned buffer that must outlive the configured stream.
std::vector<char> make_file_stream_buffer() {
    return std::vector<char>(kFileStreamBufferBytes);
}

// Purpose: Attach a caller-owned buffer before opening a file stream.
// Inputs: `stream` is a not-yet-open file stream and `buffer` is retained by the caller.
// Outputs: Configures the stream buffer in-place.
template <typename Stream> void configure_file_stream_buffer(Stream& stream, std::vector<char>& buffer) {
    stream.rdbuf()->pubsetbuf(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}

// Purpose: Add two unsigned counters while detecting overflow.
// Inputs: `lhs` and `rhs` are byte or block counters; `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_u64(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Read physical RAM counters from the host OS.
// Inputs: None.
// Outputs: Returns total and available bytes, or a conservative fallback when the platform counter is unavailable.
HostMemorySnapshot query_host_memory_snapshot() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return HostMemorySnapshot{
            .total_bytes = static_cast<std::uint64_t>(status.ullTotalPhys),
            .available_bytes = static_cast<std::uint64_t>(status.ullAvailPhys),
        };
    }
#endif
    return HostMemorySnapshot{
        .total_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL,
        .available_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
    };
}

// Purpose: Multiply two unsigned counters while detecting overflow.
// Inputs: `lhs` and `rhs` are byte or block counters; `message` labels the failing operation.
// Outputs: Returns the product or throws `ArchiveError` before wraparound.
std::uint64_t checked_multiply_u64(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw ArchiveError(message);
    }
    return lhs * rhs;
}

// Purpose: Count bounded streaming windows for a byte total without overflowing.
// Inputs: `bytes` is a file or entry length and `window_bytes` is the nonzero processing window size.
// Outputs: Returns at least one window; throws `ArchiveError` when the window size is invalid.
std::uint64_t count_stream_windows(std::uint64_t bytes, std::uint64_t window_bytes) {
    if (window_bytes == 0) {
        throw ArchiveError("streaming window size must be greater than zero");
    }
    if (bytes == 0) {
        return 1;
    }
    return ((bytes - 1U) / window_bytes) + 1U;
}

// Purpose: Compute the host RAM budget available to the archive pipeline.
// Inputs: `per_chunk_budget` estimates the memory held by one in-flight chunk.
// Outputs: Returns a byte budget that never projects total physical RAM usage above the target usage limit.
std::uint64_t resolve_host_pipeline_memory_budget(std::uint64_t per_chunk_budget) {
    const auto memory = query_host_memory_snapshot();
    if (memory.total_bytes == 0 || memory.available_bytes > memory.total_bytes) {
        return per_chunk_budget;
    }
    const auto current_used = memory.total_bytes - memory.available_bytes;
    const auto target_used = (memory.total_bytes / 100U) * kHostMemoryTargetUsagePercent;
    if (current_used >= target_used) {
        return per_chunk_budget;
    }
    const auto safe_growth = target_used - current_used;
    return std::max<std::uint64_t>(per_chunk_budget, std::min<std::uint64_t>(safe_growth, kMaxPipelineMemoryBytes));
}

// Purpose: Resolve worker and in-flight chunk counts from caller options and host memory.
// Inputs: `chunk_size`, `requested_workers`, and `requested_inflight` are validated option values.
// Outputs: Returns bounded concurrency settings or throws when requested concurrency would exceed memory limits.
PipelineBudget resolve_pipeline_budget(std::uint64_t chunk_size, std::uint32_t requested_workers,
                                       std::uint32_t requested_inflight) {
    const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const auto workers =
        requested_workers == 0 ? std::min<std::uint32_t>(hardware_threads, kMaxArchiveWorkers) : requested_workers;
    const auto per_chunk_budget = checked_multiply_u64(chunk_size, 3U, "pipeline memory budget overflows");
    const auto memory_budget = resolve_host_pipeline_memory_budget(per_chunk_budget);
    const auto memory_limited_inflight =
        static_cast<std::uint32_t>(std::max<std::uint64_t>(1U, memory_budget / per_chunk_budget));
    const auto automatic_target =
        requested_workers == 0
            ? std::min<std::uint32_t>(
                  kMaxInflightArchiveChunks,
                  std::max<std::uint32_t>(workers, hardware_threads > (std::numeric_limits<std::uint32_t>::max() / 2U)
                                                       ? kMaxInflightArchiveChunks
                                                       : hardware_threads * 2U))
            : workers;
    const auto automatic_inflight =
        std::max<std::uint32_t>(1U, std::min({automatic_target, memory_limited_inflight, kMaxInflightArchiveChunks}));
    const auto inflight = requested_inflight == 0 ? automatic_inflight : requested_inflight;
    if (inflight > memory_limited_inflight) {
        throw ArchiveError("requested in-flight chunks exceed SuperZip memory budget");
    }
    return PipelineBudget{
        .workers = workers,
        .inflight_chunks = inflight,
    };
}

// Purpose: Allocate per-chunk codec workers from the production pipeline budget.
// Inputs: `budget` is the resolved worker/in-flight policy and `work_windows` is the number of chunks/windows in the
// current file entry. Outputs: Returns at least one worker per chunk without exceeding the requested worker budget for
// normal large-entry steady state.
std::uint32_t resolve_codec_worker_count(const PipelineBudget& budget, std::uint64_t work_windows) {
    const auto active_windows = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1U, std::min<std::uint64_t>(budget.inflight_chunks, work_windows == 0 ? 1U : work_windows)));
    return std::max<std::uint32_t>(
        1U, std::min<std::uint32_t>(budget.workers, (budget.workers + active_windows - 1U) / active_windows));
}

// Purpose: Validate public archive options before any filesystem scan or allocation.
// Inputs: `chunk_size`, `block_size`, `worker_count`, and `max_inflight_chunks` are caller-selected resource controls.
// Outputs: Returns normally for bounded, GPU-compatible settings; throws `ArchiveError` otherwise.
void validate_archive_options(std::uint64_t chunk_size, std::uint32_t block_size, std::uint32_t worker_count,
                              std::uint32_t max_inflight_chunks) {
    if (chunk_size == 0) {
        throw ArchiveError("chunk size must be greater than zero");
    }
    if (chunk_size > kMaxArchiveChunkBytes) {
        throw ArchiveError("chunk size exceeds SuperZip resource limit");
    }
    if (block_size < kMinArchiveBlockBytes || block_size > kMaxArchiveBlockBytes) {
        throw ArchiveError("block size is outside SuperZip resource limits");
    }
    if (block_size > chunk_size) {
        throw ArchiveError("block size cannot exceed chunk size");
    }
    if ((chunk_size % block_size) != 0) {
        throw ArchiveError("chunk size must be an exact multiple of block size");
    }
    if (worker_count > kMaxArchiveWorkers) {
        throw ArchiveError("worker count exceeds SuperZip resource limit");
    }
    if (max_inflight_chunks > kMaxInflightArchiveChunks) {
        throw ArchiveError("in-flight chunk count exceeds SuperZip resource limit");
    }
}

// Purpose: Validate the miniz deflate compression level used for native SUZIP blocks.
// Inputs: `compression_level` is the caller-selected miniz level.
// Outputs: Returns normally for levels accepted by miniz; throws `ArchiveError` otherwise.
void validate_compression_level(int compression_level) {
    if (compression_level < kMinCompressionLevel || compression_level > kMaxCompressionLevel) {
        throw ArchiveError("compression level must be between 1 and 9");
    }
}

// Purpose: Read a bounded chunk from an input file.
// Inputs: `input` is an open binary stream and `max_bytes` is the maximum byte count to allocate/read.
// Outputs: Returns the bytes read, possibly fewer at EOF; throws `ArchiveError` on read failure.
std::vector<std::byte> read_file_chunk(std::ifstream& input, std::uint64_t max_bytes) {
    if (max_bytes == 0 || max_bytes > kMaxArchiveChunkBytes) {
        throw ArchiveError("input chunk request exceeds SuperZip resource limits");
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(max_bytes));
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto got = input.gcount();
    if (got < 0) {
        throw ArchiveError("failed to read input chunk");
    }
    buffer.resize(static_cast<std::size_t>(got));
    return buffer;
}

// Purpose: Query an output stream's current write position.
// Inputs: `stream` is an open archive output stream.
// Outputs: Returns the byte offset; throws `ArchiveError` when the offset cannot be queried.
std::uint64_t stream_position(std::ostream& stream) {
    const auto pos = stream.tellp();
    if (pos < 0) {
        throw ArchiveError("failed to query archive stream position");
    }
    return static_cast<std::uint64_t>(pos);
}

// Purpose: Query an input stream's current read position.
// Inputs: `stream` is an open archive input stream.
// Outputs: Returns the byte offset; throws `ArchiveError` when the offset cannot be queried.
std::uint64_t stream_position(std::istream& stream) {
    const auto pos = stream.tellg();
    if (pos < 0) {
        throw ArchiveError("failed to query archive stream position");
    }
    return static_cast<std::uint64_t>(pos);
}

// Purpose: Load the SuperZip index referenced by the archive footer.
// Inputs: `input` is an open binary archive stream.
// Outputs: Returns the parsed index with offset/size populated; throws `ArchiveError` for missing or invalid footer
// metadata.
ArchiveIndex read_index_from_file(std::ifstream& input) {
    input.seekg(0, std::ios::end);
    const auto size = stream_position(input);
    if (size < kArchiveFooterSize) {
        throw ArchiveError("archive is too small");
    }
    input.seekg(static_cast<std::streamoff>(size - kArchiveFooterSize), std::ios::beg);
    const auto footer_magic = read_u32(input);
    if (footer_magic != kSuperZipFooterMagic) {
        throw ArchiveError("archive footer is missing");
    }
    const auto version = read_u32(input);
    if (version < kSuperZipMinReadableVersion || version > kSuperZipVersion) {
        throw ArchiveError("unsupported archive footer version");
    }
    const auto index_offset = read_u64(input);
    const auto index_size = read_u64(input);
    if (index_offset > size || index_size > size - index_offset) {
        throw ArchiveError("archive index points outside file");
    }
    if (index_size > kMaxArchiveIndexBytes) {
        throw ArchiveError("archive index exceeds SuperZip resource limit");
    }
    input.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
    std::string index_bytes(static_cast<std::size_t>(index_size), '\0');
    input.read(index_bytes.data(), static_cast<std::streamsize>(index_bytes.size()));
    if (static_cast<std::uint64_t>(input.gcount()) != index_size) {
        throw ArchiveError("archive index is truncated");
    }
    std::istringstream index_stream(index_bytes, std::ios::binary);
    auto index = read_archive_index(index_stream);
    index.index_offset = index_offset;
    index.index_size = index_size;
    return index;
}

// Purpose: Read a bounded payload range for one archive entry.
// Inputs: `input` is the archive stream, `entry` supplies payload base metadata, `archive_size` bounds the file,
// `relative_offset` is entry-relative payload offset, and `size` is bytes to read. Outputs: Returns payload bytes;
// throws `ArchiveError` when metadata points outside the archive or bytes are truncated.
std::vector<std::byte> read_payload_window(std::ifstream& input, const ArchiveEntry& entry, std::uint64_t archive_size,
                                           std::uint64_t relative_offset, std::uint64_t size) {
    if (entry.payload_offset > archive_size || entry.payload_size > archive_size ||
        entry.payload_offset > archive_size - entry.payload_size || relative_offset > entry.payload_size ||
        size > entry.payload_size - relative_offset) {
        throw ArchiveError("entry payload points outside archive");
    }
    if (size > kMaxArchiveChunkBytes) {
        throw ArchiveError("entry payload window exceeds SuperZip resource limit");
    }
    input.seekg(static_cast<std::streamoff>(entry.payload_offset + relative_offset), std::ios::beg);
    std::vector<std::byte> payload(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (static_cast<std::uint64_t>(input.gcount()) != size) {
        throw ArchiveError("entry payload is truncated");
    }
    return payload;
}

// Purpose: Process one entry's block stream in bounded memory chunks.
// Inputs: `input` is the archive stream, `entry` is validated metadata, `archive_size` bounds reads,
// `decode_window_bytes` is the memory-accounted decode window, `gpu_options` controls codec behavior, and `consume`
// receives decoded bytes. Outputs: Returns true if any chunk used AMD HIP; throws on malformed block windows, decode
// errors, or callback failures.
DecodeStreamResult decode_entry_streaming(std::ifstream& input, const ArchiveEntry& entry, std::uint64_t archive_size,
                                          std::uint64_t decode_window_bytes, const GpuCodecOptions& gpu_options,
                                          const PipelineBudget& budget,
                                          const std::function<void(std::span<const std::byte>)>& consume) {
    const auto chunk_limit = decode_window_bytes;
    DecodeStreamResult stream_result;
    std::size_t block_index = 0;
    std::deque<PendingDecode> pending;
    auto flush_one = [&]() {
        auto decoded = pending.front().result.get();
        pending.pop_front();
        stream_result.gpu_used = decoded.gpu_used || stream_result.gpu_used;
        stream_result.crc32 = crc32_combine(stream_result.crc32, decoded.crc32, decoded.bytes.size());
        consume(std::span<const std::byte>(decoded.bytes.data(), decoded.bytes.size()));
    };
    while (block_index < entry.blocks.size()) {
        std::uint64_t uncompressed_window = 0;
        std::uint64_t payload_start = entry.payload_size;
        std::uint64_t payload_end = 0;
        const auto first = block_index;
        while (block_index < entry.blocks.size()) {
            const auto& block = entry.blocks[block_index];
            if (uncompressed_window != 0 && block.uncompressed_len > chunk_limit - uncompressed_window) {
                break;
            }
            if (block_has_payload(block.kind)) {
                payload_start = std::min<std::uint64_t>(payload_start, block.encoded_offset);
                payload_end =
                    std::max<std::uint64_t>(payload_end, checked_add_u64(block.encoded_offset, block.encoded_len,
                                                                         "block payload bounds overflow"));
            }
            uncompressed_window = checked_add_u64(uncompressed_window, block.uncompressed_len,
                                                  "decoded chunk size exceeds SuperZip resource limits");
            ++block_index;
            if (uncompressed_window >= chunk_limit) {
                break;
            }
        }

        const bool has_payload = payload_end > payload_start;
        const auto payload_size = has_payload ? payload_end - payload_start : 0;
        auto payload = read_payload_window(input, entry, archive_size, has_payload ? payload_start : 0, payload_size);
        std::vector<BlockDescriptor> adjusted;
        adjusted.reserve(block_index - first);
        for (std::size_t i = first; i < block_index; ++i) {
            auto block = entry.blocks[i];
            if (block_has_payload(block.kind)) {
                block.encoded_offset -= payload_start;
            } else {
                block.encoded_offset = 0;
            }
            adjusted.push_back(block);
        }
        pending.push_back(PendingDecode{
            .result = std::async(std::launch::async,
                                 [payload = std::move(payload), adjusted = std::move(adjusted), uncompressed_window,
                                  gpu_options]() mutable {
                                     std::vector<std::byte> decoded(static_cast<std::size_t>(uncompressed_window));
                                     const bool decoded_on_gpu = decode_chunk(payload, adjusted, decoded, gpu_options);
                                     const auto decoded_crc =
                                         crc32(std::span<const std::byte>(decoded.data(), decoded.size()));
                                     return DecodedChunk{
                                         .bytes = std::move(decoded),
                                         .crc32 = decoded_crc,
                                         .gpu_used = decoded_on_gpu,
                                     };
                                 }),
        });
        if (pending.size() >= budget.inflight_chunks) {
            flush_one();
        }
    }
    while (!pending.empty()) {
        flush_one();
    }
    return stream_result;
}

// Purpose: Verify one entry's block stream by computing decoded CRCs in bounded chunks.
// Inputs: `input` is the archive stream, `entry` is validated metadata, `archive_size` bounds reads,
// `decode_window_bytes` is the memory-accounted decode window, `gpu_options` controls codec behavior, and
// `consume_bytes` receives decoded byte counts. Outputs: Returns GPU-use and CRC state for the entry; throws on
// malformed block windows, CRC decode errors, or callback failures.
DecodeStreamResult verify_entry_streaming(std::ifstream& input, const ArchiveEntry& entry, std::uint64_t archive_size,
                                          std::uint64_t decode_window_bytes, const GpuCodecOptions& gpu_options,
                                          const PipelineBudget& budget,
                                          const std::function<void(std::uint64_t)>& consume_bytes) {
    const auto chunk_limit = decode_window_bytes;
    DecodeStreamResult stream_result;
    std::size_t block_index = 0;
    std::deque<PendingDecodedCrc> pending;
    auto flush_one = [&]() {
        auto decoded = pending.front().result.get();
        pending.pop_front();
        stream_result.gpu_used = decoded.gpu_used || stream_result.gpu_used;
        stream_result.crc32 = crc32_combine(stream_result.crc32, decoded.crc32, decoded.uncompressed_size);
        consume_bytes(decoded.uncompressed_size);
    };
    while (block_index < entry.blocks.size()) {
        std::uint64_t uncompressed_window = 0;
        std::uint64_t payload_start = entry.payload_size;
        std::uint64_t payload_end = 0;
        const auto first = block_index;
        while (block_index < entry.blocks.size()) {
            const auto& block = entry.blocks[block_index];
            if (uncompressed_window != 0 && block.uncompressed_len > chunk_limit - uncompressed_window) {
                break;
            }
            if (block_has_payload(block.kind)) {
                payload_start = std::min<std::uint64_t>(payload_start, block.encoded_offset);
                payload_end =
                    std::max<std::uint64_t>(payload_end, checked_add_u64(block.encoded_offset, block.encoded_len,
                                                                         "block payload bounds overflow"));
            }
            uncompressed_window = checked_add_u64(uncompressed_window, block.uncompressed_len,
                                                  "decoded chunk size exceeds SuperZip resource limits");
            ++block_index;
            if (uncompressed_window >= chunk_limit) {
                break;
            }
        }

        const bool has_payload = payload_end > payload_start;
        const auto payload_size = has_payload ? payload_end - payload_start : 0;
        auto payload = read_payload_window(input, entry, archive_size, has_payload ? payload_start : 0, payload_size);
        std::vector<BlockDescriptor> adjusted;
        adjusted.reserve(block_index - first);
        for (std::size_t i = first; i < block_index; ++i) {
            auto block = entry.blocks[i];
            if (block_has_payload(block.kind)) {
                block.encoded_offset -= payload_start;
            } else {
                block.encoded_offset = 0;
            }
            adjusted.push_back(block);
        }
        pending.push_back(PendingDecodedCrc{
            .result = std::async(std::launch::async,
                                 [payload = std::move(payload), adjusted = std::move(adjusted), uncompressed_window,
                                  gpu_options]() mutable {
                                     const auto crc =
                                         crc_decoded_chunk(payload, adjusted, uncompressed_window, gpu_options);
                                     return DecodedCrcChunk{
                                         .crc32 = crc.crc32,
                                         .uncompressed_size = uncompressed_window,
                                         .gpu_used = crc.gpu_used,
                                     };
                                 }),
        });
        if (pending.size() >= budget.inflight_chunks) {
            flush_one();
        }
    }
    while (!pending.empty()) {
        flush_one();
    }
    return stream_result;
}

// Purpose: Add GPU telemetry counters from two phases of the same operation.
// Inputs: `lhs` and `rhs` are operation statistics from compression and optional verification.
// Outputs: Returns combined counters without changing either input.
GpuRuntimeStats combine_gpu_runtime_stats(const GpuRuntimeStats& lhs, const GpuRuntimeStats& rhs) {
    return GpuRuntimeStats{
        .encode_chunks = checked_add_u64(lhs.encode_chunks, rhs.encode_chunks, "GPU encode chunk counter overflows"),
        .decode_chunks = checked_add_u64(lhs.decode_chunks, rhs.decode_chunks, "GPU decode chunk counter overflows"),
        .kernel_launches =
            checked_add_u64(lhs.kernel_launches, rhs.kernel_launches, "GPU kernel launch counter overflows"),
        .h2d_bytes = checked_add_u64(lhs.h2d_bytes, rhs.h2d_bytes, "GPU H2D byte counter overflows"),
        .d2h_bytes = checked_add_u64(lhs.d2h_bytes, rhs.d2h_bytes, "GPU D2H byte counter overflows"),
        .device_allocation_bytes = checked_add_u64(lhs.device_allocation_bytes, rhs.device_allocation_bytes,
                                                   "GPU allocation byte counter overflows"),
        .pattern_blocks =
            checked_add_u64(lhs.pattern_blocks, rhs.pattern_blocks, "GPU pattern block counter overflows"),
        .prefix_blocks = checked_add_u64(lhs.prefix_blocks, rhs.prefix_blocks, "GPU prefix block counter overflows"),
        .kernel_ms = lhs.kernel_ms + rhs.kernel_ms,
    };
}

// Purpose: Sum uncompressed block lengths for an entry.
// Inputs: `blocks` is the archive entry block table.
// Outputs: Returns total uncompressed bytes; throws before integer overflow.
std::uint64_t sum_block_sizes(const std::vector<BlockDescriptor>& blocks) {
    std::uint64_t total = 0;
    for (const auto& block : blocks) {
        total = checked_add_u64(total, block.uncompressed_len, "entry block sizes overflow");
    }
    return total;
}

// Purpose: Return the serialized byte count of a GPU prefix block's per-segment offset table.
// Inputs: `decoded_len` is the block's uncompressed byte length.
// Outputs: Returns the required table bytes; throws before overflow.
std::uint64_t gpu_prefix_table_bytes(std::uint32_t decoded_len) {
    const auto segment_count =
        (static_cast<std::uint64_t>(decoded_len) + kGpuPrefixSegmentBytes - 1U) / kGpuPrefixSegmentBytes;
    return checked_add_u64(segment_count, 1U, "GPU prefix segment count overflows") * sizeof(std::uint32_t);
}

// Purpose: Return the serialized byte count of an adaptive GPU-prefix codebook and per-segment offset table.
// Inputs: `decoded_len` is the block's uncompressed byte length.
// Outputs: Returns the required metadata bytes; throws before overflow.
std::uint64_t gpu_adaptive_prefix_header_bytes(std::uint32_t decoded_len) {
    return checked_add_u64(kGpuAdaptivePrefixCodebookBytes, gpu_prefix_table_bytes(decoded_len),
                           "GPU adaptive prefix header size overflows");
}

// Purpose: Validate the shared archive block kind and decoded-length invariants.
// Inputs: `entry` supplies the user-facing archive path and `block` is one parsed descriptor.
// Outputs: Returns normally for a supported non-empty bounded block; throws `ArchiveError` otherwise.
void validate_block_header_metadata(const ArchiveEntry& entry, const BlockDescriptor& block) {
    if (block.kind != BlockKind::Raw && block.kind != BlockKind::Fill && block.kind != BlockKind::Deflate &&
        block.kind != BlockKind::Pattern && block.kind != BlockKind::GpuPrefix &&
        block.kind != BlockKind::GpuAdaptivePrefix) {
        throw ArchiveError("archive block has unknown encoding kind");
    }
    if (block.uncompressed_len == 0) {
        throw ArchiveError("archive block has zero decoded length: " + entry.path);
    }
    if (block.uncompressed_len > kMaxArchiveBlockBytes) {
        throw ArchiveError("archive block exceeds SuperZip block size limit: " + entry.path);
    }
}

// Purpose: Reject sparse or overlapping payload block offsets before payload decoding.
// Inputs: `entry`, `block`, and `expected_offset` describe the parsed payload stream cursor.
// Outputs: Returns normally when the block begins at the cursor; throws `ArchiveError` otherwise.
void require_dense_payload_offset(const ArchiveEntry& entry, const BlockDescriptor& block,
                                  std::uint64_t expected_offset, std::string_view label) {
    if (block.encoded_offset != expected_offset) {
        throw ArchiveError(std::string(label) + " block payload is sparse or overlapping: " + entry.path);
    }
}

// Purpose: Validate one block's payload-specific metadata and advance the dense payload cursor.
// Inputs: `entry`, `block`, and `payload_cursor` describe one parsed block and the current payload byte position.
// Outputs: Returns the next payload cursor; throws `ArchiveError` when metadata is inconsistent or oversized.
std::uint64_t validate_block_payload_metadata(const ArchiveEntry& entry, const BlockDescriptor& block,
                                              std::uint64_t payload_cursor) {
    switch (block.kind) {
    case BlockKind::Fill:
        if (block.encoded_len != 0) {
            throw ArchiveError("fill block contains payload bytes");
        }
        return payload_cursor;
    case BlockKind::Raw:
        if (block.encoded_len != block.uncompressed_len) {
            throw ArchiveError("raw block metadata is invalid");
        }
        require_dense_payload_offset(entry, block, payload_cursor, "raw");
        return checked_add_u64(payload_cursor, block.encoded_len, "raw block payload size overflows");
    case BlockKind::Deflate:
        if (block.encoded_len == 0 || block.encoded_len >= block.uncompressed_len) {
            throw ArchiveError("deflate block metadata is invalid");
        }
        require_dense_payload_offset(entry, block, payload_cursor, "deflate");
        return checked_add_u64(payload_cursor, block.encoded_len, "deflate block payload size overflows");
    case BlockKind::Pattern:
        if (block.encoded_len < 2 || block.encoded_len > kMaxGpuPatternBytes ||
            block.encoded_len >= block.uncompressed_len) {
            throw ArchiveError("GPU pattern block metadata is invalid");
        }
        require_dense_payload_offset(entry, block, payload_cursor, "GPU pattern");
        return checked_add_u64(payload_cursor, block.encoded_len, "GPU pattern block payload size overflows");
    case BlockKind::GpuPrefix: {
        const auto table_bytes = gpu_prefix_table_bytes(block.uncompressed_len);
        if (block.encoded_len <= table_bytes || block.encoded_len >= block.uncompressed_len) {
            throw ArchiveError("GPU prefix block metadata is invalid");
        }
        require_dense_payload_offset(entry, block, payload_cursor, "GPU prefix");
        return checked_add_u64(payload_cursor, block.encoded_len, "GPU prefix block payload size overflows");
    }
    case BlockKind::GpuAdaptivePrefix: {
        const auto header_bytes = gpu_adaptive_prefix_header_bytes(block.uncompressed_len);
        if (block.encoded_len <= header_bytes || block.encoded_len >= block.uncompressed_len) {
            throw ArchiveError("GPU adaptive prefix block metadata is invalid");
        }
        require_dense_payload_offset(entry, block, payload_cursor, "GPU adaptive prefix");
        return checked_add_u64(payload_cursor, block.encoded_len, "GPU adaptive prefix block payload size overflows");
    }
    }
    throw ArchiveError("archive block has unknown encoding kind");
}

// Purpose: Validate archive entry metadata before decoding or extraction.
// Inputs: `entry` is parsed archive metadata.
// Outputs: Returns normally when metadata is safe and consistent; throws `ArchiveError` or `SecurityError` otherwise.
void validate_entry_metadata(const ArchiveEntry& entry) {
    (void)normalize_archive_path_key(entry.path);
    if (entry.directory) {
        if (entry.uncompressed_size != 0 || entry.payload_size != 0 || !entry.blocks.empty()) {
            throw ArchiveError("directory entry has payload metadata: " + entry.path);
        }
        return;
    }
    std::uint64_t raw_payload_cursor = 0;
    for (std::size_t i = 0; i < entry.blocks.size(); ++i) {
        const auto& block = entry.blocks[i];
        validate_block_header_metadata(entry, block);
        raw_payload_cursor = validate_block_payload_metadata(entry, block, raw_payload_cursor);
    }
    if (sum_block_sizes(entry.blocks) != entry.uncompressed_size) {
        throw ArchiveError("entry block sizes do not match uncompressed size: " + entry.path);
    }
    if (raw_payload_cursor != entry.payload_size) {
        throw ArchiveError("entry payload size does not match raw block metadata: " + entry.path);
    }
}

// Purpose: Validate an archive index and summarize decode memory requirements.
// Inputs: `index` is parsed untrusted archive metadata from a SUZIP archive.
// Outputs: Returns total decoded bytes and largest decoded block size; throws before any extraction output is created.
ArchiveValidationSummary validate_archive_index_metadata(const ArchiveIndex& index) {
    ArchiveValidationSummary summary;
    std::vector<ArchivePathValidationEntry> path_entries;
    path_entries.reserve(index.entries.size());
    for (const auto& entry : index.entries) {
        validate_entry_metadata(entry);
        path_entries.push_back(ArchivePathValidationEntry{
            .path = entry.path,
            .directory = entry.directory,
        });
        summary.total_uncompressed_bytes = checked_add_u64(summary.total_uncompressed_bytes, entry.uncompressed_size,
                                                           "archive uncompressed size overflows");
        for (const auto& block : entry.blocks) {
            summary.largest_decoded_block_bytes =
                std::max<std::uint64_t>(summary.largest_decoded_block_bytes, block.uncompressed_len);
        }
    }
    validate_archive_path_set(path_entries);
    return summary;
}

// Purpose: Resolve the decode window used for memory budgeting and block grouping.
// Inputs: `options` contains the caller-requested chunk size and `summary` contains validated archive block
// requirements. Outputs: Returns a window large enough for any single block while honoring larger caller chunk sizes.
std::uint64_t resolve_decode_window_bytes(const ExtractOptions& options, const ArchiveValidationSummary& summary) {
    return std::max<std::uint64_t>(options.chunk_size, summary.largest_decoded_block_bytes);
}

// Purpose: Append the native SUZIP footer after the serialized index.
// Inputs: `output` is positioned after the index and `index` contains the index offset and size.
// Outputs: Writes the footer magic, version, index offset, and index size to `output`.
void write_suzip_footer(std::ofstream& output, const ArchiveIndex& index) {
    write_u32(output, kSuperZipFooterMagic);
    write_u32(output, kSuperZipVersion);
    write_u64(output, index.index_offset);
    write_u64(output, index.index_size);
}

// Purpose: Drain one completed encode task into the archive output stream and entry metadata.
// Inputs: `pending` owns encode futures, `entry` receives block descriptors, `output` receives payload bytes, `stats`
// records GPU use, `payload_written` and `archive_block_count` track bounded archive offsets, and `entry_name` labels
// diagnostics. Outputs: Mutates archive payload, entry blocks, CRC, counters, and stats; throws on failed writes or
// block-count limits.
void write_one_encoded_chunk(std::deque<PendingEncode>& pending, ArchiveEntry& entry, std::ofstream& output,
                             OperationStats& stats, std::uint64_t& payload_written, std::uint64_t& archive_block_count,
                             std::uint32_t& crc, const std::string& entry_name) {
    auto chunk_result = pending.front().result.get();
    pending.pop_front();
    auto& encoded = chunk_result.encoded;
    crc = crc32_combine(crc, chunk_result.crc32, chunk_result.uncompressed_size);
    stats.gpu_used = stats.gpu_used || encoded.gpu_used;
    if (encoded.blocks.size() > kMaxBlocksPerEntry - entry.blocks.size()) {
        throw ArchiveError("file requires too many archive blocks: " + entry_name);
    }
    if (encoded.blocks.size() > kMaxArchiveBlocks - archive_block_count) {
        throw ArchiveError("archive requires too many total blocks");
    }
    archive_block_count += encoded.blocks.size();
    for (auto block : encoded.blocks) {
        block.encoded_offset += payload_written;
        entry.blocks.push_back(block);
    }
    if (!encoded.payload.empty()) {
        output.write(reinterpret_cast<const char*>(encoded.payload.data()),
                     static_cast<std::streamsize>(encoded.payload.size()));
        if (!output) {
            throw ArchiveError("failed to write archive payload");
        }
        payload_written = checked_add_u64(payload_written, encoded.payload.size(), "archive payload size overflows");
    }
}

// Purpose: Queue one bounded source chunk for asynchronous native SUZIP encoding.
// Inputs: `pending` receives the future, `chunk` owns uncompressed bytes, and `gpu_options` defines the CPU/GPU codec
// policy. Outputs: Appends one future that returns encoded blocks, payload bytes, CRC, and decoded byte count.
void enqueue_encode_chunk(std::deque<PendingEncode>& pending, std::vector<std::byte> chunk,
                          const GpuCodecOptions& gpu_options) {
    pending.push_back(PendingEncode{
        .result = std::async(std::launch::async,
                             [chunk = std::move(chunk), gpu_options]() {
                                 const auto chunk_size = chunk.size();
                                 auto encoded = encode_owned_chunk(std::move(chunk), gpu_options);
                                 if (!encoded.source_crc32_available) {
                                     throw ArchiveError("owned archive encode did not return a source CRC");
                                 }
                                 const auto chunk_crc = encoded.source_crc32;
                                 return EncodedArchiveChunk{
                                     .encoded = std::move(encoded),
                                     .crc32 = chunk_crc,
                                     .uncompressed_size = chunk_size,
                                 };
                             }),
    });
}

// Purpose: Compress one regular manifest file into a SUZIP entry while keeping memory bounded.
// Inputs: `manifest_entry` identifies the source file, `options` and `budget` bound chunking and concurrency,
// `gpu_telemetry` receives HIP metrics, `output` receives payload bytes, `entry` receives metadata, `stats` records
// telemetry, `archive_block_count` tracks global limits, and `progress_callback` receives progress. Outputs: Mutates
// `entry`, `stats`, `archive_block_count`, and archive payload; throws on read, encode, write, or resource-limit
// failure.
void compress_manifest_file_entry(const ManifestEntry& manifest_entry, const CompressOptions& options,
                                  const PipelineBudget& budget, const std::shared_ptr<GpuTelemetry>& gpu_telemetry,
                                  std::ofstream& output, ArchiveEntry& entry, OperationStats& stats,
                                  std::uint64_t& archive_block_count, ProgressState& progress,
                                  const ProgressCallback& progress_callback) {
    auto input_buffer = make_file_stream_buffer();
    std::ifstream input;
    configure_file_stream_buffer(input, input_buffer);
    input.open(manifest_entry.source_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open source file: " + manifest_entry.source_path.string());
    }

    const auto entry_chunks = count_stream_windows(manifest_entry.size, options.chunk_size);
    const GpuCodecOptions gpu_options{
        .require_gpu = options.gpu_required,
        .force_cpu = options.force_cpu,
        .block_size = options.block_size,
        .worker_count = resolve_codec_worker_count(budget, entry_chunks),
        .compression_level = options.compression_level,
        .telemetry = gpu_telemetry,
    };

    std::uint64_t remaining = manifest_entry.size;
    std::uint64_t payload_written = 0;
    std::uint32_t crc = 0;
    std::deque<PendingEncode> pending;
    while (remaining > 0) {
        const auto want = std::min<std::uint64_t>(remaining, options.chunk_size);
        auto chunk = read_file_chunk(input, want);
        if (chunk.empty() && want != 0) {
            throw ArchiveError("source file ended unexpectedly: " + manifest_entry.source_path.string());
        }
        const auto chunk_bytes = chunk.size();
        enqueue_encode_chunk(pending, std::move(chunk), gpu_options);
        remaining -= chunk_bytes;
        progress.add_bytes(chunk_bytes);
        publish_progress(progress, progress_callback);
        if (pending.size() >= budget.inflight_chunks) {
            write_one_encoded_chunk(pending, entry, output, stats, payload_written, archive_block_count, crc,
                                    manifest_entry.archive_path);
        }
    }
    while (!pending.empty()) {
        write_one_encoded_chunk(pending, entry, output, stats, payload_written, archive_block_count, crc,
                                manifest_entry.archive_path);
    }
    entry.payload_size = payload_written;
    entry.crc32 = crc;
}

}  // namespace

// Purpose: Create a native SUZIP archive from validated filesystem sources.
// Inputs: `sources` are input roots, `output_archive` is replaced, `options` controls CPU/GPU codec policy and resource
// bounds, and `progress_callback` receives progress snapshots. Outputs: Writes the archive and returns operation
// telemetry; throws on invalid inputs, resource limits, cancellation, codec failure, or write failure.
OperationStats compress_suzip(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& output_archive, const CompressOptions& options,
                              const ProgressCallback& progress_callback) {
    validate_archive_options(options.chunk_size, options.block_size, options.worker_count, options.max_inflight_chunks);
    validate_compression_level(options.compression_level);
    const auto budget = resolve_pipeline_budget(options.chunk_size, options.worker_count, options.max_inflight_chunks);
    const auto gpu_telemetry = std::make_shared<GpuTelemetry>();
    const auto started = std::chrono::steady_clock::now();
    const auto manifest = build_manifest(sources);
    ProgressState progress;
    progress.start(OperationKind::Compress, manifest.total_file_bytes, manifest.entries.size());

    auto archive_output_buffer = make_file_stream_buffer();
    std::ofstream output;
    configure_file_stream_buffer(output, archive_output_buffer);
    output.open(output_archive, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw ArchiveError("cannot create archive: " + output_archive.string());
    }

    ArchiveIndex index;
    OperationStats stats;
    stats.input_bytes = manifest.total_file_bytes;
    stats.entries = manifest.entries.size();
    stats.workers = budget.workers;
    stats.inflight_chunks = budget.inflight_chunks;
    std::uint64_t archive_block_count = 0;
    for (const auto& manifest_entry : manifest.entries) {
        if (progress.cancelled()) {
            throw ArchiveError("operation cancelled");
        }
        progress.set_current(manifest_entry.archive_path);
        publish_progress(progress, progress_callback);

        ArchiveEntry entry;
        entry.path = manifest_entry.archive_path;
        entry.directory = manifest_entry.directory;
        entry.uncompressed_size = manifest_entry.size;
        entry.payload_offset = stream_position(output);

        // Directories are represented in the index only; they never carry
        // payload blocks, CRCs, or archive data bytes.
        if (manifest_entry.directory) {
            index.entries.push_back(std::move(entry));
            progress.finish_entry();
            continue;
        }

        compress_manifest_file_entry(manifest_entry, options, budget, gpu_telemetry, output, entry, stats,
                                     archive_block_count, progress, progress_callback);
        index.entries.push_back(std::move(entry));
        progress.finish_entry();
    }

    index.index_offset = stream_position(output);
    write_archive_index(output, index);
    index.index_size = stream_position(output) - index.index_offset;
    write_suzip_footer(output, index);
    output.flush();
    if (!output) {
        throw ArchiveError("failed to finalize archive");
    }
    stats.output_bytes = std::filesystem::file_size(output_archive);
    stats.gpu_runtime = snapshot_gpu_telemetry(*gpu_telemetry);

    // Optional verify-after-write reuses the normal extraction validator so the
    // just-created archive is checked through the same bounds and CRC path.
    if (options.verify_after_write) {
        const auto verified = verify_suzip(output_archive, ExtractOptions{
                                                               .gpu_required = options.gpu_required,
                                                               .force_cpu = options.force_cpu,
                                                               .overwrite = false,
                                                               .chunk_size = options.chunk_size,
                                                               .block_size = options.block_size,
                                                               .worker_count = options.worker_count,
                                                               .max_inflight_chunks = options.max_inflight_chunks,
                                                           });
        stats.gpu_used = stats.gpu_used || verified.gpu_used;
        stats.gpu_runtime = combine_gpu_runtime_stats(stats.gpu_runtime, verified.gpu_runtime);
    }
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

// Purpose: Extract a native SUZIP archive while publishing each file only after verification.
// Inputs: `archive_path`, `destination`, `options`, and optional `progress_callback` describe the extraction run.
// Outputs: Restores archive entries into `destination` and returns operation telemetry or throws on validation failure.
OperationStats extract_suzip(const std::filesystem::path& archive_path, const std::filesystem::path& destination,
                             const ExtractOptions& options, const ProgressCallback& progress_callback) {
    validate_archive_options(options.chunk_size, options.block_size, options.worker_count, options.max_inflight_chunks);
    const auto gpu_telemetry = std::make_shared<GpuTelemetry>();
    const auto started = std::chrono::steady_clock::now();
    auto archive_input_buffer = make_file_stream_buffer();
    std::ifstream input;
    configure_file_stream_buffer(input, archive_input_buffer);
    input.open(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open archive: " + archive_path.string());
    }
    const auto archive_size = std::filesystem::file_size(archive_path);
    auto index = read_index_from_file(input);

    // Validate the complete index up front so extraction cannot create files for malformed metadata.
    const auto validation = validate_archive_index_metadata(index);
    const auto decode_window_bytes = resolve_decode_window_bytes(options, validation);
    const auto budget = resolve_pipeline_budget(decode_window_bytes, options.worker_count, options.max_inflight_chunks);
    std::filesystem::create_directories(destination);
    ProgressState progress;
    progress.start(OperationKind::Extract, validation.total_uncompressed_bytes, index.entries.size());
    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = validation.total_uncompressed_bytes;
    stats.entries = index.entries.size();
    stats.workers = budget.workers;
    stats.inflight_chunks = budget.inflight_chunks;

    for (const auto& entry : index.entries) {
        // Directory entries are materialized directly; file entries follow the temp-publish path below.
        if (progress.cancelled()) {
            throw ArchiveError("operation cancelled");
        }
        progress.set_current(entry.path);
        publish_progress(progress, progress_callback);
        const auto target = safe_join_archive_path(destination, entry.path);
        if (entry.directory) {
            std::filesystem::create_directories(target);
            progress.finish_entry();
            continue;
        }
        if (!options.overwrite && std::filesystem::exists(target)) {
            throw SecurityError("refusing to overwrite existing file: " + target.string());
        }
        const auto entry_windows = count_stream_windows(entry.uncompressed_size, decode_window_bytes);
        const GpuCodecOptions gpu_options{
            .require_gpu = options.gpu_required,
            .force_cpu = options.force_cpu,
            .block_size = options.block_size,
            .worker_count = resolve_codec_worker_count(budget, entry_windows),
            .telemetry = gpu_telemetry,
        };
        std::filesystem::create_directories(target.parent_path());
        // Decode into a private same-directory temporary target so failures never expose partial output.
        const auto temporary_target = reserve_file_publish_target(target);
        bool temporary_active = false;
        DecodeStreamResult decoded;
        try {
            auto output_buffer = make_file_stream_buffer();
            std::ofstream output;
            configure_file_stream_buffer(output, output_buffer);
            output.open(temporary_target.file, std::ios::binary | std::ios::trunc);
            temporary_active = true;
            if (!output) {
                throw ArchiveError("cannot create temporary extraction file: " + temporary_target.file.string());
            }
            decoded = decode_entry_streaming(input, entry, archive_size, decode_window_bytes, gpu_options, budget,
                                             [&](std::span<const std::byte> bytes) {
                                                 output.write(reinterpret_cast<const char*>(bytes.data()),
                                                              static_cast<std::streamsize>(bytes.size()));
                                                 progress.add_bytes(bytes.size());
                                             });
            output.flush();
            if (!output) {
                throw ArchiveError("failed to write temporary extraction file: " + temporary_target.file.string());
            }
            output.close();
            if (!output) {
                throw ArchiveError("failed to close temporary extraction file: " + temporary_target.file.string());
            }
            if (decoded.crc32 != entry.crc32) {
                throw ArchiveError("CRC mismatch while extracting: " + entry.path);
            }
            // The final path is touched only after stream close and CRC verification have both succeeded.
            commit_verified_file(temporary_target.file, target, options.overwrite);
            cleanup_file_publish_target(temporary_target);
            temporary_active = false;
        } catch (...) {
            if (temporary_active) {
                // Keep cleanup narrow: remove only the known temp payload and its immediate directory.
                cleanup_file_publish_target(temporary_target);
            }
            throw;
        }
        stats.gpu_used = stats.gpu_used || decoded.gpu_used;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    stats.gpu_runtime = snapshot_gpu_telemetry(*gpu_telemetry);
    return stats;
}

// Purpose: Verify a native SUZIP archive without publishing extraction output.
// Inputs: `archive_path` identifies the archive, `options` controls decode policy, and `progress_callback` observes
// work. Outputs: Returns verification telemetry; throws on malformed metadata, decode failure, CRC mismatch, or
// cancellation.
OperationStats verify_suzip(const std::filesystem::path& archive_path, const ExtractOptions& options,
                            const ProgressCallback& progress_callback) {
    validate_archive_options(options.chunk_size, options.block_size, options.worker_count, options.max_inflight_chunks);
    const auto gpu_telemetry = std::make_shared<GpuTelemetry>();
    const auto started = std::chrono::steady_clock::now();
    auto archive_input_buffer = make_file_stream_buffer();
    std::ifstream input;
    configure_file_stream_buffer(input, archive_input_buffer);
    input.open(archive_path, std::ios::binary);
    if (!input) {
        throw ArchiveError("cannot open archive: " + archive_path.string());
    }
    const auto archive_size = std::filesystem::file_size(archive_path);
    auto index = read_index_from_file(input);
    const auto validation = validate_archive_index_metadata(index);
    const auto decode_window_bytes = resolve_decode_window_bytes(options, validation);
    const auto budget = resolve_pipeline_budget(decode_window_bytes, options.worker_count, options.max_inflight_chunks);
    ProgressState progress;
    progress.start(OperationKind::Verify, validation.total_uncompressed_bytes, index.entries.size());
    OperationStats stats;
    stats.input_bytes = archive_size;
    stats.output_bytes = validation.total_uncompressed_bytes;
    stats.entries = index.entries.size();
    stats.workers = budget.workers;
    stats.inflight_chunks = budget.inflight_chunks;
    for (const auto& entry : index.entries) {
        if (entry.directory) {
            progress.finish_entry();
            continue;
        }
        progress.set_current(entry.path);
        const auto entry_windows = count_stream_windows(entry.uncompressed_size, decode_window_bytes);
        const GpuCodecOptions gpu_options{
            .require_gpu = options.gpu_required,
            .force_cpu = options.force_cpu,
            .block_size = options.block_size,
            .worker_count = resolve_codec_worker_count(budget, entry_windows),
            .telemetry = gpu_telemetry,
        };
        const auto decoded = verify_entry_streaming(input, entry, archive_size, decode_window_bytes, gpu_options,
                                                    budget, [&](std::uint64_t bytes) { progress.add_bytes(bytes); });
        if (decoded.crc32 != entry.crc32) {
            throw ArchiveError("CRC mismatch while verifying: " + entry.path);
        }
        stats.gpu_used = stats.gpu_used || decoded.gpu_used;
        progress.finish_entry();
        publish_progress(progress, progress_callback);
    }
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    stats.gpu_runtime = snapshot_gpu_telemetry(*gpu_telemetry);
    return stats;
}

}  // namespace superzip
