#include "cli/memory_benchmark.hpp"

#include "core/checksum.hpp"
#include "core/archive_index.hpp"
#include "core/decoded_chunk.hpp"
#include "core/host_memory_budget.hpp"
#include "core/worker_budget.hpp"
#include "core/result.hpp"
#include "gpu/gpu_codec.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace superzip::cli {
namespace {

constexpr std::uint64_t kCliMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kMemoryBenchmarkReserveBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::array<int, 5> kBenchmarkCompressionLevels{1, 3, superzip::kDefaultCompressionLevel, 7, 9};
constexpr std::array<std::uint32_t, 7> kBenchmarkBlockSizes{
    256U * 1024U, 512U * 1024U, 1024U * 1024U, 2048U * 1024U, 4096U * 1024U, 8192U * 1024U, 16384U * 1024U,
};

struct MemoryArchiveChunk {
    superzip::EncodedChunk encoded;
    std::uint32_t crc32 = 0;
    std::uint64_t uncompressed_size = 0;
    double source_generation_worker_seconds = 0.0;
    double codec_encode_worker_seconds = 0.0;
};

struct PendingMemoryEncode {
    std::size_t index = 0;
    std::future<MemoryArchiveChunk> result;
};

struct PendingMemoryCrc {
    std::size_t index = 0;
    std::future<superzip::DecodedChunkCrc> result;
};

struct MemoryDecodeResult {
    std::uint32_t crc32 = 0;
    bool gpu_used = false;
};

struct PendingMemoryDecode {
    std::size_t index = 0;
    std::future<MemoryDecodeResult> result;
};

struct BenchmarkSuiteCase {
    int compression_level = superzip::kDefaultCompressionLevel;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    MemoryBenchmarkResult cpu;
    MemoryBenchmarkResult gpu;
    double cpu_score = 0.0;
    double gpu_score = 0.0;
    double speedup = 0.0;
    double compression_ratio = 1.0;
};

// Purpose: Convert byte/second statistics to MiB/s for display.
// Inputs: `bytes` is the processed byte count and `seconds` is elapsed wall time.
// Outputs: Returns MiB per second, or zero when elapsed time is zero.
double mib_per_second(std::uint64_t bytes, double seconds) {
    return seconds > 0.0 ? (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds : 0.0;
}

// Purpose: Compute an archive size ratio for benchmark records.
// Inputs: `input_bytes` and `output_bytes` are uncompressed and compressed byte counters.
// Outputs: Returns output/input, or zero when no input bytes exist.
double compression_ratio(std::uint64_t input_bytes, std::uint64_t output_bytes) {
    return input_bytes == 0 ? 0.0 : static_cast<double>(output_bytes) / static_cast<double>(input_bytes);
}

// Purpose: Compute a single throughput metric from benchmark phase throughput.
// Inputs: `result` contains measured compress, verify, and extract timings.
// Outputs: Returns a weighted MiB/s value favoring compression while retaining full roundtrip cost.
double benchmark_composite_mib_s(const MemoryBenchmarkResult& result) {
    const auto input_bytes = result.stats.input_bytes;
    return (mib_per_second(input_bytes, result.compress_seconds) * 0.50) +
           (mib_per_second(input_bytes, result.verify_seconds) * 0.25) +
           (mib_per_second(input_bytes, result.extract_seconds) * 0.25);
}

// Purpose: Convert the composite benchmark throughput into a stable user-facing score.
// Inputs: `result` is one completed CPU or GPU memory benchmark lane.
// Outputs: Returns a rounded score; higher is better for the same profile, level, and workload size.
double benchmark_score(const MemoryBenchmarkResult& result) {
    return std::round(benchmark_composite_mib_s(result) * 10.0);
}

// Purpose: Add two unsigned 64-bit counters while detecting overflow.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the sum or throws `ArchiveError` before wraparound.
std::uint64_t checked_add_cli_u64(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw superzip::ArchiveError(message);
    }
    return lhs + rhs;
}

// Purpose: Multiply two unsigned 64-bit counters while detecting overflow.
// Inputs: `lhs` and `rhs` are byte counters and `message` labels the failing operation.
// Outputs: Returns the product or throws `ArchiveError` before wraparound.
std::uint64_t checked_multiply_cli_u64(std::uint64_t lhs, std::uint64_t rhs, const char* message) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw superzip::ArchiveError(message);
    }
    return lhs * rhs;
}

// Purpose: Resolve bounded in-flight work for the memory-only benchmark pipeline.
// Inputs: `workers` is the resolved CPU worker count.
// Outputs: Returns a depth admitted by production buffer policy plus benchmark reserve, or throws ArchiveError.
std::uint32_t resolve_memory_benchmark_inflight(std::uint32_t workers) {
    const auto limit = superzip::resolve_host_pipeline_inflight_limit(
        superzip::query_host_memory_snapshot(), superzip::kMaxArchiveChunkBytes, kMemoryBenchmarkReserveBytes);
    return superzip::resolve_worker_inflight_limit(workers, limit);
}

// Purpose: Match production per-chunk codec worker allocation for the memory-only benchmark.
// Inputs: `workers` is the global worker budget, `inflight` is resolved queue depth, and `chunk_count` is the generated
// workload chunk count.
// Outputs: Returns at least one codec worker per chunk, using more workers only when fewer chunks can be active.
std::uint32_t resolve_memory_codec_workers(std::uint32_t workers, std::uint32_t inflight, std::size_t chunk_count) {
    return superzip::resolve_codec_worker_count(workers, inflight, chunk_count);
}

// Purpose: Generate a deterministic random-looking byte from a virtual workload offset.
// Inputs: `index` is the zero-based virtual byte offset.
// Outputs: Returns one reproducible byte without maintaining RNG state.
std::uint8_t randomish_benchmark_byte(std::uint64_t index) {
    std::uint64_t value = index + 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    return static_cast<std::uint8_t>(value >> 56U);
}

// Purpose: Generate a deterministic low-entropy byte that is not a fill or periodic pattern.
// Inputs: `index` is the zero-based virtual byte offset inside the low-entropy region.
// Outputs: Returns one reproducible byte from a biased distribution similar to already-compressed scientific chunks.
std::uint8_t low_entropy_benchmark_byte(std::uint64_t index) {
    const auto bucket = static_cast<std::uint32_t>((static_cast<std::uint64_t>(randomish_benchmark_byte(index)) << 2U) |
                                                   (randomish_benchmark_byte(index + 0xA5A5A5A5ULL) & 0x03U));
    if (bucket < 180U) {
        return 1U;
    }
    if (bucket < 330U) {
        return 0U;
    }
    if (bucket < 450U) {
        return 2U;
    }
    if (bucket < 520U) {
        return 3U;
    }
    if (bucket < 720U) {
        return static_cast<std::uint8_t>(4U + (bucket % 16U));
    }
    if (bucket < 900U) {
        return static_cast<std::uint8_t>(20U + (bucket % 64U));
    }
    return static_cast<std::uint8_t>(84U + (bucket % 172U));
}

// Purpose: Fill independent 64 KiB groups whose four near-identical records require local dictionary matches.
// Inputs: `buffer` is the output and `global_offset` is its virtual file offset; neither needs group alignment.
// Outputs: Writes deterministic bytes without filesystem access or cross-group repetition.
void fill_segmented_record_chunk(std::vector<std::byte>& buffer, std::uint64_t global_offset) {
    constexpr std::size_t record_bytes = 16U * 1024U;
    constexpr std::size_t segment_bytes = 4U * record_bytes;
    constexpr std::size_t patch_offset = 1024U;
    std::array<std::byte, record_bytes> record{};
    for (std::size_t offset = 0; offset < buffer.size();) {
        const auto absolute_offset = global_offset + offset;
        const auto segment_index = absolute_offset / segment_bytes;
        const auto segment_offset = static_cast<std::size_t>(absolute_offset % segment_bytes);
        for (std::size_t index = 0; index < record_bytes; ++index) {
            record[index] = static_cast<std::byte>(randomish_benchmark_byte(segment_index * record_bytes + index));
        }
        const auto segment_count = std::min(segment_bytes - segment_offset, buffer.size() - offset);
        for (std::size_t copied = 0; copied < segment_count;) {
            const auto position = segment_offset + copied;
            const auto record_index = position / record_bytes;
            const auto record_offset = position % record_bytes;
            const auto count = std::min(record_bytes - record_offset, segment_count - copied);
            std::copy_n(record.begin() + static_cast<std::ptrdiff_t>(record_offset), count,
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset + copied));
            if (record_index != 0U && record_offset <= patch_offset && patch_offset - record_offset < count) {
                buffer[offset + copied + patch_offset - record_offset] ^=
                    static_cast<std::byte>(1U + (segment_index + record_index) % 255U);
            }
            copied += count;
        }
        offset += segment_count;
    }
}

// Purpose: Retain one seeded 1 MiB long-record motif without allocating it for other benchmark profiles.
// Inputs: None.
// Outputs: Returns a process-owned immutable motif for the long-sparse RAM workload.
const std::vector<std::byte>& long_sparse_record_motif() {
    static const auto record = [] {
        std::vector<std::byte> bytes(1024U * 1024U);
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(randomish_benchmark_byte(index));
        }
        return bytes;
    }();
    return record;
}

// Purpose: Fill a benchmark chunk with deterministic compressed-pattern or incompressible data.
// Inputs: `buffer` is the destination, `global_offset` is its virtual file offset, `total_bytes` is the workload size,
// and `profile` selects data shape.
// Outputs: Writes benchmark bytes into `buffer` without filesystem access.
void fill_memory_benchmark_chunk(std::vector<std::byte>& buffer, std::uint64_t global_offset, std::uint64_t total_bytes,
                                 const std::string& profile) {
    if (profile == "SegmentedRecords") {
        fill_segmented_record_chunk(buffer, global_offset);
        return;
    }
    if (profile == "RepeatedRecord" || profile == "SparseRecord" || profile == "LongSparseRecord") {
        static const auto record = [] {
            std::array<std::byte, 16U * 1024U> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = static_cast<std::byte>(randomish_benchmark_byte(index));
            }
            return bytes;
        }();
        const std::span<const std::byte> motif =
            profile == "LongSparseRecord" ? std::span(long_sparse_record_motif()) : std::span(record);
        for (std::size_t offset = 0; offset < buffer.size();) {
            const auto absolute_offset = global_offset + offset;
            const auto record_offset = static_cast<std::size_t>(absolute_offset % motif.size());
            const auto count = std::min(motif.size() - record_offset, buffer.size() - offset);
            std::copy_n(motif.begin() + static_cast<std::ptrdiff_t>(record_offset), count,
                        buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            constexpr std::size_t patch_offset = 1024U;
            if ((profile == "SparseRecord" || profile == "LongSparseRecord") && record_offset <= patch_offset &&
                patch_offset - record_offset < count) {
                const auto record_index = absolute_offset / motif.size();
                if (profile == "SparseRecord" || record_index != 0U) {
                    buffer[offset + patch_offset - record_offset] ^= static_cast<std::byte>(1U + record_index % 255U);
                }
            }
            offset += count;
        }
        return;
    }
    std::uint64_t zero_limit = 0;
    std::uint64_t text_limit = 0;
    std::uint64_t low_entropy_limit = 0;
    if (profile == "Compressible") {
        zero_limit = total_bytes / 10U;
        text_limit = zero_limit + ((total_bytes / 10U) * 8U);
        low_entropy_limit = text_limit;
    } else if (profile == "Mixed") {
        zero_limit = total_bytes / 4U;
        text_limit = zero_limit + (total_bytes / 4U);
        low_entropy_limit = text_limit + (total_bytes / 4U);
    } else if (profile != "Incompressible") {
        throw superzip::ArchiveError("unknown memory benchmark profile: " + profile);
    }

    constexpr char text[] =
        "SuperZip memory benchmark line: AMD HIP native archive codec, metadata, and verification.\n";
    constexpr auto text_len = sizeof(text) - 1U;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const auto pos = global_offset + i;
        if (pos < zero_limit) {
            buffer[i] = std::byte{0};
        } else if (pos < text_limit) {
            buffer[i] = static_cast<std::byte>(text[pos % text_len]);
        } else if (pos < low_entropy_limit) {
            buffer[i] = static_cast<std::byte>(low_entropy_benchmark_byte(pos - text_limit));
        } else {
            buffer[i] = static_cast<std::byte>(randomish_benchmark_byte(pos));
        }
    }
}

// Purpose: Resolve the worker count used by in-memory codec benchmarks.
// Inputs: `requested` is the user-provided worker count, where zero means automatic.
// Outputs: Returns a bounded worker count accepted by codec resource limits.
std::uint32_t resolve_memory_benchmark_workers(std::uint32_t requested) {
    if (requested > superzip::kMaxArchiveWorkers) {
        throw superzip::ArchiveError("worker count exceeds SuperZip resource limit");
    }
    if (requested != 0) {
        return requested;
    }
    return std::min<std::uint32_t>(
        superzip::kMaxArchiveWorkers,
        std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(std::max(1U, std::thread::hardware_concurrency()))));
}

// Purpose: Drain one memory benchmark encode task into the archive table.
// Inputs: `pending_encode` owns completed or running encode tasks, `archive` stores chunks by index, and `result`
// accumulates stats.
// Outputs: Moves one completed encode result into `archive` and updates output byte/GPU and worker-time counters.
void flush_one_memory_encode(std::deque<PendingMemoryEncode>& pending_encode, std::vector<MemoryArchiveChunk>& archive,
                             MemoryBenchmarkResult& result) {
    auto pending = std::move(pending_encode.front());
    pending_encode.pop_front();
    auto chunk = pending.result.get();
    result.stats.gpu_used = result.stats.gpu_used || chunk.encoded.gpu_used;
    result.stats.output_bytes = checked_add_cli_u64(result.stats.output_bytes, chunk.encoded.payload.size(),
                                                    "memory benchmark encoded byte count overflows");
    result.source_generation_worker_seconds += chunk.source_generation_worker_seconds;
    result.codec_encode_worker_seconds += chunk.codec_encode_worker_seconds;
    archive[pending.index] = std::move(chunk);
}

// Purpose: Model one native file's serialized block table while bounded benchmark windows are still resident.
// Inputs: `archive` owns completed chunks and `entry` tracks the virtual file's preceding payload and CRC.
// Outputs: Appends offset-adjusted descriptors and combines CRCs, or throws before index/resource overflow.
void append_memory_archive_index(std::span<const MemoryArchiveChunk> archive, superzip::ArchiveEntry& entry) {
    for (const auto& chunk : archive) {
        if (chunk.encoded.blocks.size() > superzip::kMaxArchiveBlocks - entry.blocks.size()) {
            throw superzip::ArchiveError("memory benchmark archive exceeds the native block limit");
        }
        for (auto block : chunk.encoded.blocks) {
            block.encoded_offset = checked_add_cli_u64(block.encoded_offset, entry.payload_size,
                                                       "memory benchmark block offset overflows");
            entry.blocks.push_back(block);
        }
        entry.payload_size = checked_add_cli_u64(entry.payload_size, chunk.encoded.payload.size(),
                                                 "memory benchmark archive payload overflows");
        entry.crc32 = superzip::crc32_combine(entry.crc32, chunk.crc32, chunk.uncompressed_size);
    }
}

// Purpose: Encode one synthetic memory-workload window without writing benchmark data to storage.
// Inputs: `window_offset`/`window_bytes` select the bounded window, `total_bytes` preserves deterministic data shape,
// `inflight`, `options`, and `codec_options` define backend policy, and `result` receives counters.
// Outputs: Returns an in-memory archive window with per-chunk CRC and separated worker-stage timings.
std::vector<MemoryArchiveChunk> encode_memory_benchmark_window(std::uint64_t window_offset, std::uint64_t window_bytes,
                                                               std::uint64_t total_bytes, std::uint32_t inflight,
                                                               const MemoryBenchmarkOptions& options,
                                                               const superzip::GpuCodecOptions& codec_options,
                                                               MemoryBenchmarkResult& result) {
    const auto chunk_count = static_cast<std::size_t>((window_bytes + superzip::kMaxArchiveChunkBytes - 1U) /
                                                      superzip::kMaxArchiveChunkBytes);
    std::vector<MemoryArchiveChunk> archive(chunk_count);
    std::deque<PendingMemoryEncode> pending_encode;
    for (std::uint64_t offset = 0, index = 0; offset < window_bytes; ++index) {
        const auto want = std::min<std::uint64_t>(superzip::kMaxArchiveChunkBytes, window_bytes - offset);
        const auto chunk_offset = window_offset + offset;
        const auto profile = options.profile;
        pending_encode.push_back(PendingMemoryEncode{
            .index = static_cast<std::size_t>(index),
            .result = std::async(std::launch::async,
                                 [chunk_offset, total_bytes, profile, want, codec_options]() {
                                     const auto generation_started = std::chrono::steady_clock::now();
                                     std::vector<std::byte> input(static_cast<std::size_t>(want));
                                     fill_memory_benchmark_chunk(input, chunk_offset, total_bytes, profile);
                                     const auto codec_started = std::chrono::steady_clock::now();
                                     auto encoded = superzip::encode_owned_chunk(std::move(input), codec_options);
                                     const auto encoded_at = std::chrono::steady_clock::now();
                                     if (!encoded.source_crc32_available) {
                                         throw superzip::ArchiveError(
                                             "owned memory benchmark encode did not return a source CRC");
                                     }
                                     const auto chunk_crc = encoded.source_crc32;
                                     return MemoryArchiveChunk{
                                         .encoded = std::move(encoded),
                                         .crc32 = chunk_crc,
                                         .uncompressed_size = want,
                                         .source_generation_worker_seconds =
                                             std::chrono::duration<double>(codec_started - generation_started).count(),
                                         .codec_encode_worker_seconds =
                                             std::chrono::duration<double>(encoded_at - codec_started).count(),
                                     };
                                 }),
        });
        offset += want;
        if (pending_encode.size() >= inflight) {
            flush_one_memory_encode(pending_encode, archive, result);
        }
    }
    while (!pending_encode.empty()) {
        flush_one_memory_encode(pending_encode, archive, result);
    }
    return archive;
}

// Purpose: Check deterministic HIP output for equal repeated-record chunks before decode can obscure an encode defect.
// Inputs: A completed RAM-only archive window whose full-size chunks have identical synthetic source bytes.
// Outputs: Returns for equal payloads; identifies the first differing encoded byte or payload length.
void verify_repeated_record_payloads(const std::vector<MemoryArchiveChunk>& archive) {
    if (archive.empty()) {
        return;
    }
    const auto& reference = archive.front();
    for (std::size_t index = 1; index < archive.size(); ++index) {
        const auto& candidate = archive[index];
        if (candidate.uncompressed_size != reference.uncompressed_size) {
            continue;
        }
        if (candidate.encoded.payload.size() != reference.encoded.payload.size()) {
            throw superzip::ArchiveError("repeated-record encoding differs at chunk " + std::to_string(index) +
                                         ": reference_bytes=" + std::to_string(reference.encoded.payload.size()) +
                                         " candidate_bytes=" + std::to_string(candidate.encoded.payload.size()));
        }
        const auto mismatch = std::mismatch(reference.encoded.payload.begin(), reference.encoded.payload.end(),
                                            candidate.encoded.payload.begin());
        if (mismatch.first != reference.encoded.payload.end()) {
            const auto offset = static_cast<std::size_t>(mismatch.first - reference.encoded.payload.begin());
            throw superzip::ArchiveError("repeated-record encoding differs at chunk " + std::to_string(index) +
                                         " encoded_byte=" + std::to_string(offset) +
                                         " reference=" + std::to_string(static_cast<std::uint8_t>(*mismatch.first)) +
                                         " candidate=" + std::to_string(static_cast<std::uint8_t>(*mismatch.second)));
        }
    }
}

// Purpose: Drain one decoded-CRC benchmark task and compare it with the original chunk CRC.
// Inputs: `pending_crc` owns CRC tasks, `archive` is the encoded chunk table, `profile` selects failure diagnostics,
// and `result` accumulates backend telemetry.
// Outputs: Removes one task; throws `ArchiveError` if the decoded payload hash differs from the source hash.
void flush_one_memory_crc(std::deque<PendingMemoryCrc>& pending_crc, const std::vector<MemoryArchiveChunk>& archive,
                          const std::string& profile, MemoryBenchmarkResult& result) {
    auto pending = std::move(pending_crc.front());
    pending_crc.pop_front();
    const auto decoded_crc = pending.result.get();
    const auto& chunk = archive[pending.index];
    result.stats.gpu_used = result.stats.gpu_used || decoded_crc.gpu_used;
    if (decoded_crc.crc32 != chunk.crc32) {
        superzip::GpuCodecOptions cpu_options;
        cpu_options.require_gpu = false;
        cpu_options.force_cpu = true;
        cpu_options.block_size = result.block_size;
        const auto cpu_crc = superzip::crc_decoded_chunk(chunk.encoded.payload, chunk.encoded.blocks,
                                                         chunk.uncompressed_size, cpu_options);
        std::string location;
        if (profile == "RepeatedRecord" && cpu_crc.crc32 != chunk.crc32) {
            const auto owner =
                superzip::decode_owned_chunk(chunk.encoded.payload, chunk.encoded.blocks,
                                             static_cast<std::size_t>(chunk.uncompressed_size), cpu_options);
            const auto decoded = owner.bytes();
            std::size_t bad_count = 0;
            std::size_t last_bad = 0;
            for (std::size_t offset = 0; offset < decoded.size(); ++offset) {
                const auto expected = randomish_benchmark_byte(offset % (16U * 1024U));
                if (decoded[offset] != static_cast<std::byte>(expected)) {
                    if (bad_count == 0U) {
                        location = " first_bad_byte=" + std::to_string(offset) +
                                   " block=" + std::to_string(offset / result.block_size) +
                                   " expected=" + std::to_string(expected) +
                                   " actual=" + std::to_string(static_cast<std::uint8_t>(decoded[offset]));
                    }
                    ++bad_count;
                    last_bad = offset;
                }
            }
            location += " bad_bytes=" + std::to_string(bad_count) + " last_bad_byte=" + std::to_string(last_bad);
        }
        throw superzip::ArchiveError("memory benchmark verification CRC mismatch at chunk " +
                                     std::to_string(pending.index) + ": source=" + std::to_string(chunk.crc32) +
                                     " hip=" + std::to_string(decoded_crc.crc32) +
                                     " cpu=" + std::to_string(cpu_crc.crc32) + location);
    }
}

// Purpose: Verify encoded chunks by computing decoded CRCs without retaining full decoded buffers.
// Inputs: `archive`, `inflight`, `profile`, and `codec_options` define bounded concurrent verification work; `result`
// receives GPU usage.
// Outputs: Returns normally when every chunk CRC matches; throws on decode or integrity failure.
void verify_memory_benchmark_archive(const std::vector<MemoryArchiveChunk>& archive, std::uint32_t inflight,
                                     const std::string& profile, const superzip::GpuCodecOptions& codec_options,
                                     MemoryBenchmarkResult& result) {
    std::deque<PendingMemoryCrc> pending_crc;
    for (std::size_t index = 0; index < archive.size(); ++index) {
        pending_crc.push_back(PendingMemoryCrc{
            .index = index,
            .result = std::async(std::launch::async,
                                 [&archive, index, codec_options] {
                                     const auto& chunk = archive[index];
                                     return superzip::crc_decoded_chunk(
                                         std::span<const std::byte>(chunk.encoded.payload.data(),
                                                                    chunk.encoded.payload.size()),
                                         std::span<const superzip::BlockDescriptor>(chunk.encoded.blocks.data(),
                                                                                    chunk.encoded.blocks.size()),
                                         chunk.uncompressed_size, codec_options);
                                 }),
        });
        if (pending_crc.size() >= inflight) {
            flush_one_memory_crc(pending_crc, archive, profile, result);
        }
    }
    while (!pending_crc.empty()) {
        flush_one_memory_crc(pending_crc, archive, profile, result);
    }
}

// Purpose: Drain one full-decode benchmark task and validate its CRC.
// Inputs: `pending_decode` owns decode tasks, `archive` is the encoded chunk table, and `result` accumulates backend
// telemetry.
// Outputs: Removes one task; throws `ArchiveError` if extraction output differs from the source hash.
void flush_one_memory_decode(std::deque<PendingMemoryDecode>& pending_decode,
                             const std::vector<MemoryArchiveChunk>& archive, MemoryBenchmarkResult& result) {
    auto pending = std::move(pending_decode.front());
    pending_decode.pop_front();
    const auto decoded = pending.result.get();
    result.stats.gpu_used = result.stats.gpu_used || decoded.gpu_used;
    if (decoded.crc32 != archive[pending.index].crc32) {
        throw superzip::ArchiveError("memory benchmark extraction CRC mismatch");
    }
}

// Purpose: Decode every in-memory benchmark chunk to exercise the extraction path without disk writes.
// Inputs: `archive`, `inflight`, and `codec_options` define bounded concurrent decode work; `result` receives GPU
// usage.
// Outputs: Returns normally when every decoded chunk matches the original CRC.
void extract_memory_benchmark_archive(const std::vector<MemoryArchiveChunk>& archive, std::uint32_t inflight,
                                      const superzip::GpuCodecOptions& codec_options, MemoryBenchmarkResult& result) {
    std::deque<PendingMemoryDecode> pending_decode;
    for (std::size_t index = 0; index < archive.size(); ++index) {
        pending_decode.push_back(PendingMemoryDecode{
            .index = index,
            .result = std::async(std::launch::async,
                                 [&archive, index, codec_options] {
                                     const auto& chunk = archive[index];
                                     const auto decoded = superzip::decode_owned_chunk(
                                         chunk.encoded.payload, chunk.encoded.blocks,
                                         static_cast<std::size_t>(chunk.uncompressed_size), codec_options);
                                     return MemoryDecodeResult{
                                         .crc32 = decoded.crc32,
                                         .gpu_used = decoded.gpu_used,
                                     };
                                 }),
        });
        if (pending_decode.size() >= inflight) {
            flush_one_memory_decode(pending_decode, archive, result);
        }
    }
    while (!pending_decode.empty()) {
        flush_one_memory_decode(pending_decode, archive, result);
    }
}

// Purpose: Execute one benchmark-suite candidate with separate forced-CPU and required-GPU lanes.
// Inputs: `options` supplies shared workload settings, `level` is the compression level, and `block_size` is the SUZIP
// block size.
// Outputs: Returns CPU/GPU timings, scores, speedup, and compression ratio; throws if required HIP is unavailable.
BenchmarkSuiteCase run_benchmark_suite_case(const BenchmarkSuiteOptions& options, int level, std::uint32_t block_size) {
    MemoryBenchmarkOptions cpu_options;
    cpu_options.size_mib = options.size_mib;
    cpu_options.profile = options.profile;
    cpu_options.force_cpu = true;
    cpu_options.workers = options.workers;
    cpu_options.block_size = block_size;
    cpu_options.compression_level = level;

    MemoryBenchmarkOptions gpu_options = cpu_options;
    gpu_options.force_cpu = false;
    gpu_options.require_gpu = true;

    auto cpu = run_memory_benchmark(cpu_options);
    auto gpu = run_memory_benchmark(gpu_options);
    const auto cpu_total = cpu.compress_seconds + cpu.verify_seconds + cpu.extract_seconds;
    const auto gpu_total = gpu.compress_seconds + gpu.verify_seconds + gpu.extract_seconds;
    const auto cpu_score = benchmark_score(cpu);
    const auto gpu_score = benchmark_score(gpu);
    const auto ratio = compression_ratio(gpu.stats.input_bytes, gpu.stats.output_bytes);
    return BenchmarkSuiteCase{
        .compression_level = level,
        .block_size = block_size,
        .cpu = std::move(cpu),
        .gpu = std::move(gpu),
        .cpu_score = cpu_score,
        .gpu_score = gpu_score,
        .speedup = gpu_total > 0.0 ? cpu_total / gpu_total : 0.0,
        .compression_ratio = ratio,
    };
}

// Purpose: Print one benchmark-suite candidate in a stable key/value format.
// Inputs: `candidate` is the completed CPU/GPU benchmark pair.
// Outputs: Writes a single parseable `suite_case` line to stdout.
void print_benchmark_suite_case(const BenchmarkSuiteCase& candidate) {
    std::cout << "suite_case"
              << " compression_level=" << candidate.compression_level
              << " block_size_kib=" << (candidate.block_size / 1024U) << " cpu_score=" << candidate.cpu_score
              << " gpu_score=" << candidate.gpu_score << " speedup_vs_cpu=" << candidate.speedup
              << " cpu_input_bytes=" << candidate.cpu.stats.input_bytes
              << " cpu_output_bytes=" << candidate.cpu.stats.output_bytes
              << " gpu_input_bytes=" << candidate.gpu.stats.input_bytes
              << " gpu_output_bytes=" << candidate.gpu.stats.output_bytes << " cpu_compression_ratio="
              << compression_ratio(candidate.cpu.stats.input_bytes, candidate.cpu.stats.output_bytes)
              << " gpu_compression_ratio="
              << compression_ratio(candidate.gpu.stats.input_bytes, candidate.gpu.stats.output_bytes)
              << " compression_ratio=" << candidate.compression_ratio << " cpu_compress_mib_s="
              << mib_per_second(candidate.cpu.stats.input_bytes, candidate.cpu.compress_seconds)
              << " gpu_compress_mib_s="
              << mib_per_second(candidate.gpu.stats.input_bytes, candidate.gpu.compress_seconds)
              << " gpu_encode_chunks=" << candidate.gpu.stats.gpu_runtime.encode_chunks
              << " gpu_decode_chunks=" << candidate.gpu.stats.gpu_runtime.decode_chunks
              << " gpu_kernel_launches=" << candidate.gpu.stats.gpu_runtime.kernel_launches
              << " gpu_kernel_ms=" << candidate.gpu.stats.gpu_runtime.kernel_ms
              << " gpu_pattern_blocks=" << candidate.gpu.stats.gpu_runtime.pattern_blocks
              << " gpu_prefix_blocks=" << candidate.gpu.stats.gpu_runtime.prefix_blocks
              << " gpu_dictionary_blocks=" << candidate.gpu.stats.gpu_runtime.dictionary_blocks
              << " gpu_sparse_pattern_blocks=" << candidate.gpu.stats.gpu_runtime.sparse_pattern_blocks
              << " memory_only=true"
              << " disk_write_bytes=0"
              << "\n";
}

// Purpose: Select the best autotune candidate without silently degrading compression below the balanced baseline.
// Inputs: `candidates` is a non-empty set of measured benchmark-suite cases and `tune_levels` controls ratio guarding.
// Outputs: Returns a reference to the recommended candidate.
const BenchmarkSuiteCase& choose_benchmark_suite_recommendation(const std::vector<BenchmarkSuiteCase>& candidates,
                                                                bool tune_levels) {
    const BenchmarkSuiteCase* baseline = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.compression_level == superzip::kDefaultCompressionLevel &&
            candidate.block_size == superzip::kDefaultArchiveBlockBytes) {
            baseline = &candidate;
            break;
        }
    }
    if (baseline == nullptr) {
        baseline = &candidates.front();
    }
    const auto max_ratio = tune_levels ? baseline->compression_ratio * 1.02 : std::numeric_limits<double>::infinity();
    const BenchmarkSuiteCase* best = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.compression_ratio > max_ratio) {
            continue;
        }
        if (best == nullptr || candidate.gpu_score > best->gpu_score) {
            best = &candidate;
        }
    }
    return *(best == nullptr ? baseline : best);
}

// Purpose: Validate RAM benchmark arguments before host admission and any codec allocation.
// Inputs: `options` selects size in MiB, effort, block size, and mutually exclusive backend policies.
// Outputs: Returns checked input bytes or throws on invalid arguments; pipeline admission follows before allocation.
std::uint64_t validate_memory_benchmark_options(const MemoryBenchmarkOptions& options) {
    if (options.size_mib < 10240U) {
        throw superzip::ArchiveError("memory benchmark workload must be at least 10240 MiB (10 GiB)");
    }
    if (options.compression_level < superzip::kMinCompressionLevel ||
        options.compression_level > superzip::kMaxCompressionLevel) {
        throw superzip::ArchiveError("compression level must be between 1 and 9");
    }
    if (options.block_size < superzip::kMinArchiveBlockBytes || options.block_size > superzip::kMaxArchiveBlockBytes) {
        throw superzip::ArchiveError("memory benchmark block size is outside SuperZip resource limits");
    }
    if ((superzip::kMaxArchiveChunkBytes % options.block_size) != 0) {
        throw superzip::ArchiveError("memory benchmark block size must divide the 128 MiB chunk size");
    }
    if (options.require_gpu && options.force_cpu) {
        throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
    }
    const auto total_bytes = checked_multiply_cli_u64(options.size_mib, kCliMiB, "memory benchmark size overflows");
    return total_bytes;
}

}  // namespace

// Purpose: Print one machine-readable memory benchmark result line.
// Inputs: `result` contains operation statistics, benchmark settings, and RAM-only proof fields.
// Outputs: Writes result fields and overlapping GPU worker-stage times to stdout without mutating benchmark state.
void print_memory_benchmark_stats(const MemoryBenchmarkResult& result) {
    const auto& stats = result.stats;
    const auto& stages = stats.gpu_runtime.encode_stage_worker_seconds;
    const auto& classification = stats.gpu_runtime.classification_stage_worker_seconds;
    const auto& decode_stages = stats.gpu_runtime.owned_decode_stage_worker_seconds;
    std::cout
        << "entries=" << stats.entries << " input_bytes=" << stats.input_bytes << " output_bytes=" << stats.output_bytes
        << " archive_bytes=" << result.archive_bytes << " workers=" << stats.workers
        << " inflight_chunks=" << stats.inflight_chunks << " codec_workers=" << result.codec_workers
        << " decode_inflight_chunks=" << result.decode_inflight_chunks << " block_size_bytes=" << result.block_size
        << " decode_codec_workers=" << result.decode_codec_workers << " compression_level=" << result.compression_level
        << " gpu_used=" << (stats.gpu_used ? "true" : "false")
        << " gpu_encode_chunks=" << stats.gpu_runtime.encode_chunks
        << " gpu_decode_chunks=" << stats.gpu_runtime.decode_chunks
        << " gpu_kernel_launches=" << stats.gpu_runtime.kernel_launches
        << " gpu_kernel_ms=" << stats.gpu_runtime.kernel_ms << " gpu_h2d_bytes=" << stats.gpu_runtime.h2d_bytes
        << " gpu_d2h_bytes=" << stats.gpu_runtime.d2h_bytes
        << " gpu_device_allocation_bytes=" << stats.gpu_runtime.device_allocation_bytes
        << " gpu_host_pinned_allocation_bytes=" << stats.gpu_runtime.host_pinned_allocation_bytes
        << " gpu_host_pinned_output_bytes=" << stats.gpu_runtime.host_pinned_output_bytes
        << " gpu_pattern_blocks=" << stats.gpu_runtime.pattern_blocks
        << " gpu_prefix_blocks=" << stats.gpu_runtime.prefix_blocks
        << " gpu_dictionary_blocks=" << stats.gpu_runtime.dictionary_blocks
        << " gpu_sparse_pattern_blocks=" << stats.gpu_runtime.sparse_pattern_blocks
        << " gpu_readiness_worker_seconds=" << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::Readiness)]
        << " gpu_analysis_worker_seconds=" << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::HostAnalysis)]
        << " gpu_classification_worker_seconds="
        << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::DeviceClassification)]
        << " gpu_classification_allocation_worker_seconds="
        << classification[static_cast<std::size_t>(superzip::GpuClassificationStage::InputAllocation)]
        << " gpu_classification_upload_worker_seconds="
        << classification[static_cast<std::size_t>(superzip::GpuClassificationStage::InputUpload)]
        << " gpu_classification_crc_worker_seconds="
        << classification[static_cast<std::size_t>(superzip::GpuClassificationStage::SourceChecksum)]
        << " gpu_classification_validation_worker_seconds="
        << classification[static_cast<std::size_t>(superzip::GpuClassificationStage::CandidateValidation)]
        << " gpu_prefix_worker_seconds=" << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::Prefix)]
        << " gpu_sparse_worker_seconds=" << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::Sparse)]
        << " gpu_dictionary_worker_seconds=" << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::Dictionary)]
        << " gpu_publication_worker_seconds=" << stages[static_cast<std::size_t>(superzip::GpuEncodeStage::Publication)]
        << " decode_allocation_worker_seconds="
        << decode_stages[static_cast<std::size_t>(superzip::OwnedDecodeStage::OutputAllocation)]
        << " decode_materialization_worker_seconds="
        << decode_stages[static_cast<std::size_t>(superzip::OwnedDecodeStage::Materialization)]
        << " decode_crc_worker_seconds="
        << decode_stages[static_cast<std::size_t>(superzip::OwnedDecodeStage::HostChecksum)]
        << " seconds=" << stats.seconds << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
        << " compress_seconds=" << result.compress_seconds << " verify_seconds=" << result.verify_seconds
        << " extract_seconds=" << result.extract_seconds
        << " source_generation_worker_seconds=" << result.source_generation_worker_seconds
        << " codec_encode_worker_seconds=" << result.codec_encode_worker_seconds
        << " compress_mib_s=" << mib_per_second(stats.input_bytes, result.compress_seconds)
        << " verify_mib_s=" << mib_per_second(stats.input_bytes, result.verify_seconds)
        << " extract_mib_s=" << mib_per_second(stats.input_bytes, result.extract_seconds)
        << " compression_ratio=" << compression_ratio(stats.input_bytes, stats.output_bytes)
        << " archive_compression_ratio=" << compression_ratio(stats.input_bytes, result.archive_bytes)
        << " memory_only=true"
        << " disk_write_bytes=0"
        << "\n";
}

// Purpose: Execute a bounded, RAM-only archive workload with independent encode, verify, and extract phases.
// Inputs: `options` selects the benchmark profile, size, backend policy, workers, block size, and effort.
// Outputs: Returns exact size, timing, integrity, and GPU telemetry statistics or throws on any failed phase.
MemoryBenchmarkResult run_memory_benchmark(const MemoryBenchmarkOptions& options) {
    const auto total_bytes = validate_memory_benchmark_options(options);

    const auto workers = resolve_memory_benchmark_workers(options.workers);
    const auto inflight = resolve_memory_benchmark_inflight(workers);
    const auto chunk_count = static_cast<std::size_t>((total_bytes + superzip::kMaxArchiveChunkBytes - 1U) /
                                                      superzip::kMaxArchiveChunkBytes);
    const auto codec_workers = resolve_memory_codec_workers(workers, inflight, chunk_count);
    auto telemetry = std::make_shared<superzip::GpuTelemetry>();
    superzip::GpuCodecOptions codec_options{
        .require_gpu = options.require_gpu,
        .force_cpu = options.force_cpu,
        .block_size = options.block_size,
        .worker_count = codec_workers,
        .compression_level = options.compression_level,
        .telemetry = telemetry,
    };

    MemoryBenchmarkResult result;
    result.stats.input_bytes = total_bytes;
    result.stats.workers = workers;
    result.stats.inflight_chunks = inflight;
    result.codec_workers = codec_workers;
    result.decode_inflight_chunks =
        superzip::resolve_owned_decode_inflight(inflight, superzip::kMaxArchiveChunkBytes, codec_options);
    result.decode_codec_workers =
        superzip::resolve_codec_worker_count(workers, result.decode_inflight_chunks, chunk_count);
    result.block_size = options.block_size;
    result.compression_level = options.compression_level;
    superzip::ArchiveIndex modeled_index;
    modeled_index.version = superzip::kSuperZipMaxReadableVersion;
    modeled_index.entries.push_back(superzip::ArchiveEntry{
        .path = "memory-benchmark.bin",
        .uncompressed_size = total_bytes,
    });

    const auto total_started = std::chrono::steady_clock::now();
    const auto window_chunks = std::max<std::uint64_t>(1U, inflight);
    const auto window_bytes = checked_multiply_cli_u64(window_chunks, superzip::kMaxArchiveChunkBytes,
                                                       "memory benchmark window size overflows");
    for (std::uint64_t offset = 0; offset < total_bytes;) {
        const auto current_window = std::min<std::uint64_t>(window_bytes, total_bytes - offset);
        auto phase_started = std::chrono::steady_clock::now();
        auto archive = encode_memory_benchmark_window(offset, current_window, total_bytes, inflight, options,
                                                      codec_options, result);
        append_memory_archive_index(archive, modeled_index.entries.front());
        result.compress_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();

        if (options.profile == "RepeatedRecord") {
            verify_repeated_record_payloads(archive);
        }

        phase_started = std::chrono::steady_clock::now();
        verify_memory_benchmark_archive(archive, inflight, options.profile, codec_options, result);
        result.verify_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();

        phase_started = std::chrono::steady_clock::now();
        if (codec_options.require_gpu && !codec_options.host_output_pool) {
            codec_options.host_output_pool = superzip::make_owned_decode_pool(codec_options);
        }
        {
            auto decode_options = codec_options;
            decode_options.worker_count = result.decode_codec_workers;
            extract_memory_benchmark_archive(archive, result.decode_inflight_chunks, decode_options, result);
        }
        if (offset + current_window == total_bytes) {
            codec_options.host_output_pool.reset();
        }
        result.extract_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();
        offset += current_window;
    }
    const auto finalize_started = std::chrono::steady_clock::now();
    if (modeled_index.entries.front().payload_size != result.stats.output_bytes) {
        throw superzip::ArchiveError("memory benchmark archive payload count differs from encoded chunks");
    }
    std::ostringstream serialized_index(std::ios::out | std::ios::binary);
    superzip::write_archive_index(serialized_index, modeled_index);
    result.archive_bytes = checked_add_cli_u64(result.stats.output_bytes, serialized_index.view().size(),
                                               "memory benchmark archive size overflows");
    result.archive_bytes = checked_add_cli_u64(result.archive_bytes, superzip::kSuperZipFooterBytes,
                                               "memory benchmark archive footer overflows");
    result.compress_seconds +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - finalize_started).count();
    result.stats.entries = modeled_index.entries.size();
    result.stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - total_started).count();
    result.stats.gpu_runtime = superzip::snapshot_gpu_telemetry(*telemetry);
    return result;
}

// Purpose: Execute the RAM-only CPU/GPU benchmark suite and print parseable recommendation lines.
// Inputs: `options` selects profile, compression level, block-size sweep, and tuning mode.
// Outputs: Writes `suite_case` and `suite_recommendation` lines to stdout; throws on benchmark failures.
void run_benchmark_suite(const BenchmarkSuiteOptions& options) {
    std::vector<std::uint32_t> block_sizes;
    if (options.tune) {
        block_sizes.assign(kBenchmarkBlockSizes.begin(), kBenchmarkBlockSizes.end());
    } else {
        block_sizes.push_back(options.block_size);
    }

    std::vector<int> levels;
    if (options.tune_levels) {
        levels.assign(kBenchmarkCompressionLevels.begin(), kBenchmarkCompressionLevels.end());
    } else {
        levels.push_back(options.compression_level);
    }

    std::vector<BenchmarkSuiteCase> candidates;
    for (const auto level : levels) {
        for (const auto block_size : block_sizes) {
            auto candidate = run_benchmark_suite_case(options, level, block_size);
            print_benchmark_suite_case(candidate);
            candidates.push_back(std::move(candidate));
        }
    }
    const auto& recommendation = choose_benchmark_suite_recommendation(candidates, options.tune_levels);
    std::cout << "suite_recommendation"
              << " compression_level=" << recommendation.compression_level
              << " block_size_kib=" << (recommendation.block_size / 1024U) << " gpu_score=" << recommendation.gpu_score
              << " speedup_vs_cpu=" << recommendation.speedup << " input_bytes=" << recommendation.gpu.stats.input_bytes
              << " output_bytes=" << recommendation.gpu.stats.output_bytes
              << " compression_ratio=" << recommendation.compression_ratio
              << " gpu_pattern_blocks=" << recommendation.gpu.stats.gpu_runtime.pattern_blocks
              << " gpu_prefix_blocks=" << recommendation.gpu.stats.gpu_runtime.prefix_blocks
              << " gpu_dictionary_blocks=" << recommendation.gpu.stats.gpu_runtime.dictionary_blocks
              << " gpu_sparse_pattern_blocks=" << recommendation.gpu.stats.gpu_runtime.sparse_pattern_blocks
              << " memory_only=true"
              << " disk_write_bytes=0"
              << "\n";
}

}  // namespace superzip::cli
