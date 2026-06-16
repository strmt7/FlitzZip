#include "ar/ar_adapter.hpp"
#include "bzip2/bzip2_adapter.hpp"
#include "cab/cab_adapter.hpp"
#include "core/archive.hpp"
#include "core/archive_format.hpp"
#include "core/checksum.hpp"
#include "cpio/cpio_adapter.hpp"
#include "core/defender_scan.hpp"
#include "core/integrity.hpp"
#include "core/result.hpp"
#include "gzip/gzip_adapter.hpp"
#include "gpu/gpu_codec.hpp"
#include "iso/iso_adapter.hpp"
#include "lha/lha_adapter.hpp"
#include "rpm/rpm_adapter.hpp"
#include "sevenzip/sevenzip_adapter.hpp"
#include "tar/tar_adapter.hpp"
#include "unix_compress/unix_compress_adapter.hpp"
#include "wim/wim_adapter.hpp"
#include "xar/xar_adapter.hpp"
#include "xz/xz_adapter.hpp"
#include "zstd/zstd_adapter.hpp"
#include "zip/zip_adapter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
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

namespace {

constexpr std::uint64_t kCliMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kMemoryBenchmarkReserveBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::array<int, 5> kBenchmarkCompressionLevels{1, 3, superzip::kDefaultCompressionLevel, 7, 9};
constexpr std::array<std::uint32_t, 4> kBenchmarkBlockSizes{
    256U * 1024U,
    1024U * 1024U,
    4096U * 1024U,
    16384U * 1024U,
};

struct MemoryBenchmarkOptions {
    std::uint64_t size_mib = 10240;
    std::string profile = "Mixed";
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
};

struct MemoryArchiveChunk {
    superzip::EncodedChunk encoded;
    std::uint32_t crc32 = 0;
    std::uint64_t uncompressed_size = 0;
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

struct MemoryBenchmarkResult {
    superzip::OperationStats stats;
    std::uint32_t codec_workers = 1;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
    double compress_seconds = 0.0;
    double verify_seconds = 0.0;
    double extract_seconds = 0.0;
};

struct BenchmarkSuiteOptions {
    std::uint64_t size_mib = 10240;
    std::string profile = "Mixed";
    std::uint32_t workers = 0;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
    bool tune = false;
    bool tune_levels = false;
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

// Purpose: Print command-line help for supported SuperZip operations.
// Inputs: None.
// Outputs: Writes usage text to stdout.
void usage() {
    std::cout
        << "SuperZip CLI\n"
        << "Usage:\n"
        << "  superzip_cli gpu-info\n"
        << "  superzip_cli gpu-diagnostic [--seconds <1-30>] [--buffer-mib <16-512>] [--inner-iterations <1-4096>]\n"
        << "  superzip_cli dependency-check\n"
        << "  superzip_cli formats\n"
        << "  superzip_cli identify <archive>\n"
        << "  superzip_cli memory-benchmark --size-mib <n> --profile Mixed|Compressible|Incompressible [--require-gpu|--force-cpu] [--workers <n>] [--block-size-kib <256|1024|4096|16384>] [--compression-level <1-9>]\n"
        << "  superzip_cli benchmark-suite [--size-mib <n>] [--profile Mixed|Compressible|Incompressible] [--workers <n>] [--block-size-kib <256|1024|4096|16384>] [--compression-level <1-9>] [--tune] [--tune-levels]\n"
        << "  superzip_cli compress --format suzip --output <archive> [--require-gpu|--force-cpu] [--workers <n>] [--inflight <n>] [--block-size-kib <256|1024|4096|16384>] [--compression-level <1-9>] [--verify-after-write] [--sha256] [--defender-scan] <path>...\n"
        << "  superzip_cli compress --format zip|tar|tar.gz|tgz|tar.bz2|tbz|tbz2|tar.zst|tzst|gz|gzip|bz2|bzip2|zst|zstd|z|compress|cpio|ar --output <archive> [--sha256] [--defender-scan] <path>...\n"
        << "  superzip_cli extract --format suzip --output <directory> [--require-gpu|--force-cpu] [--workers <n>] [--inflight <n>] [--overwrite] [--sha256] [--defender-scan] <archive.suzip>\n"
        << "  superzip_cli extract --format auto|zip|tar|tar.gz|tgz|tar.bz2|tbz|tbz2|tar.xz|txz|tar.zst|tzst|gz|gzip|bz2|bzip2|xz|zst|zstd|z|compress|cab|iso|cpio|ar|deb|rpm|7z|lha|lzh|wim|swm|xar --output <directory> [--overwrite] [--sha256] [--defender-scan] <archive>\n"
        << "  superzip_cli verify [--require-gpu|--force-cpu] [--workers <n>] [--inflight <n>] [--sha256] [--defender-scan] <archive.suzip>\n";
}

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
    return (mib_per_second(input_bytes, result.compress_seconds) * 0.50)
        + (mib_per_second(input_bytes, result.verify_seconds) * 0.25)
        + (mib_per_second(input_bytes, result.extract_seconds) * 0.25);
}

// Purpose: Convert the composite benchmark throughput into a stable user-facing score.
// Inputs: `result` is one completed CPU or GPU memory benchmark lane.
// Outputs: Returns a rounded score; higher is better for the same profile, level, and workload size.
double benchmark_score(const MemoryBenchmarkResult& result) {
    return std::round(benchmark_composite_mib_s(result) * 10.0);
}

// Purpose: Print archive operation statistics in a stable key/value format.
// Inputs: `stats` is the completed operation result.
// Outputs: Writes one line to stdout.
void print_stats(const superzip::OperationStats& stats) {
    std::cout << "entries=" << stats.entries
              << " input_bytes=" << stats.input_bytes
              << " output_bytes=" << stats.output_bytes
              << " workers=" << stats.workers
              << " inflight_chunks=" << stats.inflight_chunks
              << " gpu_used=" << (stats.gpu_used ? "true" : "false")
              << " gpu_encode_chunks=" << stats.gpu_runtime.encode_chunks
              << " gpu_decode_chunks=" << stats.gpu_runtime.decode_chunks
              << " gpu_kernel_launches=" << stats.gpu_runtime.kernel_launches
              << " gpu_kernel_ms=" << stats.gpu_runtime.kernel_ms
              << " gpu_h2d_bytes=" << stats.gpu_runtime.h2d_bytes
              << " gpu_d2h_bytes=" << stats.gpu_runtime.d2h_bytes
              << " gpu_device_allocation_bytes=" << stats.gpu_runtime.device_allocation_bytes
              << " gpu_pattern_blocks=" << stats.gpu_runtime.pattern_blocks
              << " seconds=" << stats.seconds
              << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
              << " compression_ratio=" << compression_ratio(stats.input_bytes, stats.output_bytes)
              << "\n";
}

// Purpose: Print the archive formats that SuperZip recognizes and whether each is implemented.
// Inputs: None.
// Outputs: Writes one parseable line per real archive format.
void print_format_registry() {
    for (const auto& info : superzip::archive_format_registry()) {
        if (info.format == superzip::ArchiveFormat::Unknown || info.format == superzip::ArchiveFormat::Auto) {
            continue;
        }
        std::cout << "format=" << info.key
                  << " display=\"" << info.display_name << "\""
                  << " extensions=\"" << info.extensions << "\""
                  << " can_create=" << (info.can_create ? "true" : "false")
                  << " can_extract=" << (info.can_extract ? "true" : "false")
                  << " gpu_native=" << (info.gpu_native ? "true" : "false")
                  << " bundled_native=" << (info.bundled_native ? "true" : "false")
                  << "\n";
    }
}

// Purpose: Resolve a CLI format token and optional auto-detection into one concrete archive format.
// Inputs: `format_token` is the user token, `archive_path` is used only for `auto`, and `allow_auto` gates auto-detection.
// Outputs: Returns a concrete format; throws `ArchiveError` for unknown tokens or unsupported auto-detection.
superzip::ArchiveFormat resolve_cli_archive_format(
    const std::string& format_token,
    const std::filesystem::path& archive_path,
    bool allow_auto) {
    const auto parsed = superzip::parse_archive_format_token(format_token);
    if (!parsed.has_value()) {
        throw superzip::ArchiveError("unknown archive format: " + format_token);
    }
    if (*parsed != superzip::ArchiveFormat::Auto) {
        return *parsed;
    }
    if (!allow_auto) {
        throw superzip::ArchiveError("archive format auto-detection is only supported for extraction and identify");
    }
    const auto detected = superzip::detect_archive_format(archive_path);
    if (detected == superzip::ArchiveFormat::Unknown) {
        throw superzip::ArchiveError("unable to detect archive format: " + archive_path.string());
    }
    return detected;
}

// Purpose: Reject recognized archive formats that SuperZip can identify but cannot yet process.
// Inputs: `format` is a concrete archive format and `operation` is `create` or `extract`.
// Outputs: Returns normally for implemented formats; throws a clear non-implementation error otherwise.
void reject_unsupported_cli_format(superzip::ArchiveFormat format, std::string_view operation) {
    const auto& info = superzip::archive_format_info(format);
    const bool supported = operation == "create" ? info.can_create : info.can_extract;
    if (!supported) {
        throw superzip::ArchiveError(
            std::string("archive format recognized but not yet implemented for ") +
            std::string(operation) + ": " + info.key);
    }
}

// Purpose: Print memory-only benchmark statistics in the same stable key/value style as archive operations.
// Inputs: `result` is the completed memory benchmark result.
// Outputs: Writes one parseable line to stdout.
void print_memory_benchmark_stats(const MemoryBenchmarkResult& result) {
    const auto& stats = result.stats;
    std::cout << "entries=" << stats.entries
              << " input_bytes=" << stats.input_bytes
              << " output_bytes=" << stats.output_bytes
              << " workers=" << stats.workers
              << " inflight_chunks=" << stats.inflight_chunks
              << " codec_workers=" << result.codec_workers
              << " block_size_bytes=" << result.block_size
              << " compression_level=" << result.compression_level
              << " gpu_used=" << (stats.gpu_used ? "true" : "false")
              << " gpu_encode_chunks=" << stats.gpu_runtime.encode_chunks
              << " gpu_decode_chunks=" << stats.gpu_runtime.decode_chunks
              << " gpu_kernel_launches=" << stats.gpu_runtime.kernel_launches
              << " gpu_kernel_ms=" << stats.gpu_runtime.kernel_ms
              << " gpu_h2d_bytes=" << stats.gpu_runtime.h2d_bytes
              << " gpu_d2h_bytes=" << stats.gpu_runtime.d2h_bytes
              << " gpu_device_allocation_bytes=" << stats.gpu_runtime.device_allocation_bytes
              << " gpu_pattern_blocks=" << stats.gpu_runtime.pattern_blocks
              << " seconds=" << stats.seconds
              << " throughput_mib_s=" << mib_per_second(stats.input_bytes, stats.seconds)
              << " compress_seconds=" << result.compress_seconds
              << " verify_seconds=" << result.verify_seconds
              << " extract_seconds=" << result.extract_seconds
              << " compress_mib_s=" << mib_per_second(stats.input_bytes, result.compress_seconds)
              << " verify_mib_s=" << mib_per_second(stats.input_bytes, result.verify_seconds)
              << " extract_mib_s=" << mib_per_second(stats.input_bytes, result.extract_seconds)
              << " compression_ratio=" << compression_ratio(stats.input_bytes, stats.output_bytes)
              << " memory_only=true"
              << " disk_write_bytes=0"
              << "\n";
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

// Purpose: Estimate safe host-memory growth before the process risks exceeding the SuperZip RAM target.
// Inputs: None.
// Outputs: Returns available growth bytes until 80% host RAM use, or a conservative fallback on unsupported hosts.
std::uint64_t safe_host_memory_growth_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0 && status.ullAvailPhys <= status.ullTotalPhys) {
        const auto total = static_cast<std::uint64_t>(status.ullTotalPhys);
        const auto available = static_cast<std::uint64_t>(status.ullAvailPhys);
        const auto used = total - available;
        const auto target = (total / 100U) * superzip::kHostMemoryTargetUsagePercent;
        return used >= target ? 0 : target - used;
    }
#endif
    return 4ULL * 1024ULL * 1024ULL * 1024ULL;
}

// Purpose: Refuse a memory-only benchmark that would plausibly force paging or exceed the RAM-use policy.
// Inputs: `total_bytes` is the virtual uncompressed workload size.
// Outputs: Returns normally when host RAM can hold the generated in-memory archive and working chunks.
void assert_memory_benchmark_budget(std::uint64_t total_bytes) {
    auto required = checked_add_cli_u64(total_bytes, kMemoryBenchmarkReserveBytes, "memory benchmark budget overflow");
    required = checked_add_cli_u64(
        required,
        checked_multiply_cli_u64(superzip::kMaxArchiveChunkBytes, 2U, "memory benchmark working set overflow"),
        "memory benchmark budget overflow");
    const auto safe_growth = safe_host_memory_growth_bytes();
    if (required > safe_growth) {
        throw superzip::ArchiveError(
            "memory benchmark would exceed the 80% host RAM target; reduce --size-mib or close other memory-heavy processes");
    }
}

// Purpose: Resolve bounded in-flight work for the memory-only benchmark pipeline.
// Inputs: `workers` is the resolved CPU worker count and `total_bytes` is the virtual workload size.
// Outputs: Returns a queue depth that fits the 80% host RAM policy and SuperZip's in-flight limit.
std::uint32_t resolve_memory_benchmark_inflight(std::uint32_t workers, std::uint64_t total_bytes) {
    const auto safe_growth = safe_host_memory_growth_bytes();
    auto baseline = checked_add_cli_u64(total_bytes, kMemoryBenchmarkReserveBytes, "memory benchmark budget overflow");
    baseline = checked_add_cli_u64(
        baseline,
        checked_multiply_cli_u64(superzip::kMaxArchiveChunkBytes, 2U, "memory benchmark working set overflow"),
        "memory benchmark budget overflow");
    if (baseline >= safe_growth) {
        return 1;
    }
    const auto remaining = safe_growth - baseline;
    const auto per_inflight = checked_multiply_cli_u64(
        superzip::kMaxArchiveChunkBytes,
        2U,
        "memory benchmark in-flight budget overflow");
    const auto memory_limited = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1U, remaining / per_inflight));
    return std::max<std::uint32_t>(
        1U,
        std::min({workers, memory_limited, superzip::kMaxInflightArchiveChunks}));
}

// Purpose: Match production per-chunk codec worker allocation for the memory-only benchmark.
// Inputs: `workers` is the global worker budget, `inflight` is resolved queue depth, and `chunk_count` is the generated workload chunk count.
// Outputs: Returns at least one codec worker per chunk, using more workers only when fewer chunks can be active.
std::uint32_t resolve_memory_codec_workers(
    std::uint32_t workers,
    std::uint32_t inflight,
    std::size_t chunk_count) {
    const auto active_windows = std::max<std::uint32_t>(
        1U,
        std::min<std::uint32_t>(
            inflight,
            static_cast<std::uint32_t>(std::min<std::size_t>(chunk_count, std::numeric_limits<std::uint32_t>::max()))));
    return std::max<std::uint32_t>(
        1U,
        std::min<std::uint32_t>(workers, (workers + active_windows - 1U) / active_windows));
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

// Purpose: Fill a benchmark chunk with deterministic compressible or incompressible data.
// Inputs: `buffer` is the destination, `global_offset` is its virtual file offset, `total_bytes` is the workload size, and `profile` selects data shape.
// Outputs: Writes benchmark bytes into `buffer` without filesystem access.
void fill_memory_benchmark_chunk(
    std::vector<std::byte>& buffer,
    std::uint64_t global_offset,
    std::uint64_t total_bytes,
    const std::string& profile) {
    std::uint64_t zero_limit = 0;
    std::uint64_t text_limit = 0;
    if (profile == "Compressible") {
        zero_limit = total_bytes / 10U;
        text_limit = zero_limit + ((total_bytes / 10U) * 8U);
    } else if (profile == "Mixed") {
        zero_limit = total_bytes / 4U;
        text_limit = zero_limit + (total_bytes / 4U);
    } else if (profile != "Incompressible") {
        throw superzip::ArchiveError("unknown memory benchmark profile: " + profile);
    }

    constexpr char text[] = "SuperZip memory benchmark line: AMD HIP native archive codec, metadata, and verification.\n";
    constexpr auto text_len = sizeof(text) - 1U;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const auto pos = global_offset + i;
        if (pos < zero_limit) {
            buffer[i] = std::byte{0};
        } else if (pos < text_limit) {
            buffer[i] = static_cast<std::byte>(text[pos % text_len]);
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
// Inputs: `pending_encode` owns completed or running encode tasks, `archive` stores chunks by index, and `result` accumulates stats.
// Outputs: Moves one completed encode result into `archive` and updates output byte/GPU counters.
void flush_one_memory_encode(
    std::deque<PendingMemoryEncode>& pending_encode,
    std::vector<MemoryArchiveChunk>& archive,
    MemoryBenchmarkResult& result) {
    auto pending = std::move(pending_encode.front());
    pending_encode.pop_front();
    auto chunk = pending.result.get();
    result.stats.gpu_used = result.stats.gpu_used || chunk.encoded.gpu_used;
    result.stats.output_bytes = checked_add_cli_u64(
        result.stats.output_bytes,
        chunk.encoded.payload.size(),
        "memory benchmark encoded byte count overflows");
    archive[pending.index] = std::move(chunk);
}

// Purpose: Encode the synthetic memory workload without writing benchmark data to storage.
// Inputs: `total_bytes`, `inflight`, `options`, and `codec_options` define generated chunks and backend policy; `result` receives counters.
// Outputs: Returns an in-memory archive chunk vector with per-chunk CRC data for later verification.
std::vector<MemoryArchiveChunk> encode_memory_benchmark_archive(
    std::uint64_t total_bytes,
    std::uint32_t inflight,
    const MemoryBenchmarkOptions& options,
    const superzip::GpuCodecOptions& codec_options,
    MemoryBenchmarkResult& result) {
    const auto chunk_count = static_cast<std::size_t>(
        (total_bytes + superzip::kMaxArchiveChunkBytes - 1U) / superzip::kMaxArchiveChunkBytes);
    std::vector<MemoryArchiveChunk> archive(chunk_count);
    std::deque<PendingMemoryEncode> pending_encode;
    for (std::uint64_t offset = 0, index = 0; offset < total_bytes; ++index) {
        const auto want = std::min<std::uint64_t>(superzip::kMaxArchiveChunkBytes, total_bytes - offset);
        const auto chunk_offset = offset;
        const auto profile = options.profile;
        pending_encode.push_back(PendingMemoryEncode{
            .index = static_cast<std::size_t>(index),
            .result = std::async(
                std::launch::async,
                [chunk_offset, total_bytes, profile, want, codec_options]() {
                    std::vector<std::byte> input(static_cast<std::size_t>(want));
                    fill_memory_benchmark_chunk(input, chunk_offset, total_bytes, profile);
                    auto encoded = superzip::encode_chunk(
                        std::span<const std::byte>(input.data(), input.size()),
                        codec_options);
                    const auto chunk_crc = encoded.source_crc32_available
                        ? encoded.source_crc32
                        : superzip::crc32(std::span<const std::byte>(input.data(), input.size()));
                    return MemoryArchiveChunk{
                        .encoded = std::move(encoded),
                        .crc32 = chunk_crc,
                        .uncompressed_size = want,
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

// Purpose: Drain one decoded-CRC benchmark task and compare it with the original chunk CRC.
// Inputs: `pending_crc` owns CRC tasks, `archive` is the encoded chunk table, and `result` accumulates backend telemetry.
// Outputs: Removes one task; throws `ArchiveError` if the decoded payload hash differs from the source hash.
void flush_one_memory_crc(
    std::deque<PendingMemoryCrc>& pending_crc,
    const std::vector<MemoryArchiveChunk>& archive,
    MemoryBenchmarkResult& result) {
    auto pending = std::move(pending_crc.front());
    pending_crc.pop_front();
    const auto decoded_crc = pending.result.get();
    const auto& chunk = archive[pending.index];
    result.stats.gpu_used = result.stats.gpu_used || decoded_crc.gpu_used;
    if (decoded_crc.crc32 != chunk.crc32) {
        throw superzip::ArchiveError("memory benchmark verification CRC mismatch");
    }
}

// Purpose: Verify encoded chunks by computing decoded CRCs without retaining full decoded buffers.
// Inputs: `archive`, `inflight`, and `codec_options` define bounded concurrent verification work; `result` receives GPU usage.
// Outputs: Returns normally when every chunk CRC matches; throws on decode or integrity failure.
void verify_memory_benchmark_archive(
    const std::vector<MemoryArchiveChunk>& archive,
    std::uint32_t inflight,
    const superzip::GpuCodecOptions& codec_options,
    MemoryBenchmarkResult& result) {
    std::deque<PendingMemoryCrc> pending_crc;
    for (std::size_t index = 0; index < archive.size(); ++index) {
        pending_crc.push_back(PendingMemoryCrc{
            .index = index,
            .result = std::async(
                std::launch::async,
                [&archive, index, codec_options] {
                    const auto& chunk = archive[index];
                    return superzip::crc_decoded_chunk(
                        std::span<const std::byte>(chunk.encoded.payload.data(), chunk.encoded.payload.size()),
                        std::span<const superzip::BlockDescriptor>(chunk.encoded.blocks.data(), chunk.encoded.blocks.size()),
                        chunk.uncompressed_size,
                        codec_options);
                }),
        });
        if (pending_crc.size() >= inflight) {
            flush_one_memory_crc(pending_crc, archive, result);
        }
    }
    while (!pending_crc.empty()) {
        flush_one_memory_crc(pending_crc, archive, result);
    }
}

// Purpose: Drain one full-decode benchmark task and validate its CRC.
// Inputs: `pending_decode` owns decode tasks, `archive` is the encoded chunk table, and `result` accumulates backend telemetry.
// Outputs: Removes one task; throws `ArchiveError` if extraction output differs from the source hash.
void flush_one_memory_decode(
    std::deque<PendingMemoryDecode>& pending_decode,
    const std::vector<MemoryArchiveChunk>& archive,
    MemoryBenchmarkResult& result) {
    auto pending = std::move(pending_decode.front());
    pending_decode.pop_front();
    const auto decoded = pending.result.get();
    result.stats.gpu_used = result.stats.gpu_used || decoded.gpu_used;
    if (decoded.crc32 != archive[pending.index].crc32) {
        throw superzip::ArchiveError("memory benchmark extraction CRC mismatch");
    }
}

// Purpose: Decode every in-memory benchmark chunk to exercise the extraction path without disk writes.
// Inputs: `archive`, `inflight`, and `codec_options` define bounded concurrent decode work; `result` receives GPU usage.
// Outputs: Returns normally when every decoded chunk matches the original CRC.
void extract_memory_benchmark_archive(
    const std::vector<MemoryArchiveChunk>& archive,
    std::uint32_t inflight,
    const superzip::GpuCodecOptions& codec_options,
    MemoryBenchmarkResult& result) {
    std::deque<PendingMemoryDecode> pending_decode;
    for (std::size_t index = 0; index < archive.size(); ++index) {
        pending_decode.push_back(PendingMemoryDecode{
            .index = index,
            .result = std::async(
                std::launch::async,
                [&archive, index, codec_options] {
                    const auto& chunk = archive[index];
                    std::vector<std::byte> decoded(static_cast<std::size_t>(chunk.uncompressed_size));
                    const bool decoded_on_gpu = superzip::decode_chunk(
                        std::span<const std::byte>(chunk.encoded.payload.data(), chunk.encoded.payload.size()),
                        std::span<const superzip::BlockDescriptor>(chunk.encoded.blocks.data(), chunk.encoded.blocks.size()),
                        std::span<std::byte>(decoded.data(), decoded.size()),
                        codec_options);
                    return MemoryDecodeResult{
                        .crc32 = superzip::crc32(std::span<const std::byte>(decoded.data(), decoded.size())),
                        .gpu_used = decoded_on_gpu,
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

// Purpose: Run a no-filesystem CPU or AMD-HIP benchmark over generated in-memory chunks.
// Inputs: `options` controls virtual workload size, data profile, backend lane, worker count, and compression level.
// Outputs: Returns timing, byte counts, GPU telemetry, and correctness-checked phase statistics.
MemoryBenchmarkResult run_memory_benchmark(const MemoryBenchmarkOptions& options) {
    if (options.size_mib < 10240U) {
        throw superzip::ArchiveError("memory benchmark workload must be at least 10240 MiB (10 GiB)");
    }
    if (options.compression_level < superzip::kMinCompressionLevel || options.compression_level > superzip::kMaxCompressionLevel) {
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
    assert_memory_benchmark_budget(total_bytes);

    const auto workers = resolve_memory_benchmark_workers(options.workers);
    const auto inflight = resolve_memory_benchmark_inflight(workers, total_bytes);
    const auto chunk_count = static_cast<std::size_t>(
        (total_bytes + superzip::kMaxArchiveChunkBytes - 1U) / superzip::kMaxArchiveChunkBytes);
    const auto codec_workers = resolve_memory_codec_workers(workers, inflight, chunk_count);
    auto telemetry = std::make_shared<superzip::GpuTelemetry>();
    const superzip::GpuCodecOptions codec_options{
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
    result.block_size = options.block_size;
    result.compression_level = options.compression_level;

    const auto total_started = std::chrono::steady_clock::now();
    auto phase_started = std::chrono::steady_clock::now();
    auto archive = encode_memory_benchmark_archive(total_bytes, inflight, options, codec_options, result);
    result.compress_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();

    phase_started = std::chrono::steady_clock::now();
    verify_memory_benchmark_archive(archive, inflight, codec_options, result);
    result.verify_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();

    phase_started = std::chrono::steady_clock::now();
    extract_memory_benchmark_archive(archive, inflight, codec_options, result);
    result.extract_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - phase_started).count();
    result.stats.entries = archive.size();
    result.stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - total_started).count();
    result.stats.gpu_runtime = superzip::snapshot_gpu_telemetry(*telemetry);
    return result;
}

// Purpose: Execute one benchmark-suite candidate with separate forced-CPU and required-GPU lanes.
// Inputs: `options` supplies shared workload settings, `level` is the compression level, and `block_size` is the SUZIP block size.
// Outputs: Returns CPU/GPU timings, scores, speedup, and compression ratio; throws if required HIP is unavailable.
BenchmarkSuiteCase run_benchmark_suite_case(
    const BenchmarkSuiteOptions& options,
    int level,
    std::uint32_t block_size) {
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
              << " block_size_kib=" << (candidate.block_size / 1024U)
              << " cpu_score=" << candidate.cpu_score
              << " gpu_score=" << candidate.gpu_score
              << " speedup_vs_cpu=" << candidate.speedup
              << " compression_ratio=" << candidate.compression_ratio
              << " cpu_compress_mib_s=" << mib_per_second(candidate.cpu.stats.input_bytes, candidate.cpu.compress_seconds)
              << " gpu_compress_mib_s=" << mib_per_second(candidate.gpu.stats.input_bytes, candidate.gpu.compress_seconds)
              << " gpu_encode_chunks=" << candidate.gpu.stats.gpu_runtime.encode_chunks
              << " gpu_decode_chunks=" << candidate.gpu.stats.gpu_runtime.decode_chunks
              << " gpu_kernel_launches=" << candidate.gpu.stats.gpu_runtime.kernel_launches
              << " gpu_kernel_ms=" << candidate.gpu.stats.gpu_runtime.kernel_ms
              << " memory_only=true"
              << " disk_write_bytes=0"
              << "\n";
}

// Purpose: Select the best autotune candidate without silently degrading compression below the balanced baseline.
// Inputs: `candidates` is a non-empty set of measured benchmark-suite cases and `tune_levels` controls ratio guarding.
// Outputs: Returns a reference to the recommended candidate.
const BenchmarkSuiteCase& choose_benchmark_suite_recommendation(
    const std::vector<BenchmarkSuiteCase>& candidates,
    bool tune_levels) {
    const BenchmarkSuiteCase* baseline = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.compression_level == superzip::kDefaultCompressionLevel
            && candidate.block_size == superzip::kDefaultArchiveBlockBytes) {
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

// Purpose: Run the built-in RAM-only scoring and autotuning suite.
// Inputs: `options` selects workload size, profile, worker count, block-size sweep, and compression-level sweep.
// Outputs: Prints candidate results and one recommendation; throws on invalid settings, CPU/GPU mismatch, or hidden CPU fallback.
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
              << " block_size_kib=" << (recommendation.block_size / 1024U)
              << " gpu_score=" << recommendation.gpu_score
              << " speedup_vs_cpu=" << recommendation.speedup
              << " compression_ratio=" << recommendation.compression_ratio
              << " memory_only=true"
              << " disk_write_bytes=0"
              << "\n";
}

// Purpose: Print SHA-256 integrity data for an archive.
// Inputs: `path` is an existing file to hash.
// Outputs: Writes algorithm and digest lines to stdout; throws if hashing fails.
void print_integrity_hash(const std::filesystem::path& path) {
    const auto hash = superzip::hash_file(path, superzip::IntegrityMode::Sha256);
    std::cout << "integrity_algorithm=" << hash.algorithm << "\n";
    std::cout << "integrity_sha256=" << hash.hex_digest << "\n";
}

// Purpose: Run and print an opt-in Microsoft Defender scan result.
// Inputs: `path` is the target file or directory and `block_if_detected` controls whether a non-clean attempted scan aborts the operation.
// Outputs: Writes scan state to stdout; throws `SecurityError` only when Defender reports a scanned target is not clean and blocking is requested.
void print_defender_scan(const std::filesystem::path& path, bool block_if_detected) {
    const auto scan = superzip::scan_with_windows_defender(path, superzip::DefenderScanMode::FullPath);
    std::cout << "defender_attempted=" << (scan.attempted ? "true" : "false") << "\n";
    std::cout << "defender_clean=" << (scan.clean ? "true" : "false") << "\n";
    std::cout << "defender_exit_code=" << scan.exit_code << "\n";
    if (block_if_detected && scan.attempted && !scan.clean) {
        throw superzip::SecurityError("Microsoft Defender did not report the target as clean: " + path.string());
    }
}

// Purpose: Print AMD HIP dependency state in a stable machine-readable format.
// Inputs: `info` is the GPU/runtime status returned by the backend.
// Outputs: Writes one key/value block to stdout.
void print_gpu_info(const superzip::GpuInfo& info) {
    std::cout << "hip_compiled=" << (info.hip_compiled ? "true" : "false") << "\n";
    std::cout << "hip_runtime_loadable=" << (info.hip_runtime_loadable ? "true" : "false") << "\n";
    std::cout << "hip_runtime_name=" << info.runtime_name << "\n";
    std::cout << "available=" << (info.available ? "true" : "false") << "\n";
    std::cout << "device_count=" << info.device_count << "\n";
    std::cout << "selected_device=" << info.selected_device << "\n";
    std::cout << "vram_total_bytes=" << info.vram_total_bytes << "\n";
    std::cout << "vram_free_bytes=" << info.vram_free_bytes << "\n";
    std::cout << "device_name=" << info.device_name << "\n";
    std::cout << "gcn_arch=" << info.gcn_arch << "\n";
    std::cout << "status=" << info.status << "\n";
}

// Purpose: Print HIP-only diagnostic results in a stable machine-readable format.
// Inputs: `result` is the completed diagnostic output.
// Outputs: Writes one key/value block to stdout.
void print_gpu_diagnostic(const superzip::GpuDiagnosticResult& result) {
    std::cout << "hip_compiled=" << (result.info.hip_compiled ? "true" : "false") << "\n";
    std::cout << "hip_runtime_loadable=" << (result.info.hip_runtime_loadable ? "true" : "false") << "\n";
    std::cout << "hip_runtime_name=" << result.info.runtime_name << "\n";
    std::cout << "available=" << (result.info.available ? "true" : "false") << "\n";
    std::cout << "device_name=" << result.info.device_name << "\n";
    std::cout << "gcn_arch=" << result.info.gcn_arch << "\n";
    std::cout << "diagnostic_bytes=" << result.bytes << "\n";
    std::cout << "diagnostic_kernel_launches=" << result.kernel_launches << "\n";
    std::cout << "diagnostic_kernel_ms=" << result.kernel_ms << "\n";
    std::cout << "diagnostic_h2d_bytes=" << result.h2d_bytes << "\n";
    std::cout << "diagnostic_d2h_bytes=" << result.d2h_bytes << "\n";
    std::cout << "diagnostic_device_allocation_bytes=" << result.device_allocation_bytes << "\n";
    std::cout << "diagnostic_checksum=" << result.checksum << "\n";
    std::cout << "diagnostic_wall_seconds=" << result.wall_seconds << "\n";
}

// Purpose: Convert AMD HIP dependency status into deterministic installer-friendly process codes.
// Inputs: `info` is the GPU/runtime status returned by the backend.
// Outputs: Returns 0 when a HIP build, runtime, and AMD device are available; otherwise returns a stable nonzero code.
int dependency_exit_code(const superzip::GpuInfo& info) {
    if (!info.hip_compiled) {
        return 10;
    }
    if (!info.hip_runtime_loadable) {
        return 11;
    }
    if (!info.available) {
        return 12;
    }
    return 0;
}

// Purpose: Read the value following a named command-line flag.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` is the flag label for diagnostics.
// Outputs: Returns the following argument value; throws `ArchiveError` when the value is missing.
std::string require_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    if (index + 1 >= args.size()) {
        throw superzip::ArchiveError(std::string("missing value for ") + name);
    }
    ++index;
    return args[index];
}

// Purpose: Parse an unsigned 32-bit tuning argument.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` is the flag label for diagnostics.
// Outputs: Returns the parsed integer; throws `ArchiveError` when the value is invalid.
std::uint32_t require_u32_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    const auto text = require_arg(args, index, name);
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > std::numeric_limits<std::uint32_t>::max()) {
        throw superzip::ArchiveError(std::string("invalid value for ") + name + ": " + text);
    }
    return static_cast<std::uint32_t>(value);
}

// Purpose: Parse a bounded signed integer tuning argument.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, `name` labels diagnostics, and `minimum`/`maximum` define the accepted range.
// Outputs: Returns the parsed integer; throws `ArchiveError` when the value is invalid or out of range.
int require_int_arg(const std::vector<std::string>& args, std::size_t& index, const char* name, int minimum, int maximum) {
    const auto text = require_arg(args, index, name);
    int value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum || value > maximum) {
        throw superzip::ArchiveError(std::string("invalid value for ") + name + ": " + text);
    }
    return value;
}

// Purpose: Parse a supported SUZIP block size in KiB for compression and benchmarking.
// Inputs: `args` is the full argument vector, `index` is the current flag index and is advanced, and `name` labels diagnostics.
// Outputs: Returns a production-supported block size in bytes; throws `ArchiveError` for unsupported values.
std::uint32_t require_block_size_kib_arg(const std::vector<std::string>& args, std::size_t& index, const char* name) {
    const auto kib = require_u32_arg(args, index, name);
    switch (kib) {
    case 256U:
    case 1024U:
    case 4096U:
    case 16384U:
        return kib * 1024U;
    default:
        throw superzip::ArchiveError("unsupported block size; use 256, 1024, 4096, or 16384 KiB");
    }
}

}  // namespace

// Purpose: Execute the SuperZip command-line interface.
// Inputs: `argc`/`argv` are process command-line arguments encoded by the platform C runtime.
// Outputs: Returns 0 on success, 1 on operation failure, and 2 on invalid usage.
int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.empty()) {
            usage();
            return 2;
        }

        // `gpu-info` is intentionally separate from archive commands so
        // installers and CI can validate AMD HIP readiness without side
        // effects on user files.
        if (args[0] == "gpu-info") {
            const auto info = superzip::query_gpu_info();
            print_gpu_info(info);
            return info.available ? 0 : 1;
        }

        if (args[0] == "gpu-diagnostic") {
            superzip::GpuDiagnosticOptions options;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--seconds") {
                    const auto seconds = require_int_arg(args, i, "--seconds", 1, 30);
                    options.seconds = static_cast<double>(seconds);
                } else if (args[i] == "--buffer-mib") {
                    options.buffer_mib = require_u32_arg(args, i, "--buffer-mib");
                } else if (args[i] == "--inner-iterations") {
                    options.inner_iterations = require_u32_arg(args, i, "--inner-iterations");
                } else {
                    throw superzip::ArchiveError("unknown gpu-diagnostic argument: " + args[i]);
                }
            }
            print_gpu_diagnostic(superzip::run_gpu_diagnostic(options));
            return 0;
        }

        if (args[0] == "dependency-check") {
            const auto info = superzip::query_gpu_info();
            print_gpu_info(info);
            const int code = dependency_exit_code(info);
            std::cout << "dependency_status=" << (code == 0 ? "ready" : code == 10 ? "cpu_only_build" : code == 11 ? "missing_hip_runtime" : "missing_amd_gpu") << "\n";
            return code;
        }

        if (args[0] == "formats") {
            if (args.size() != 1) {
                usage();
                return 2;
            }
            print_format_registry();
            return 0;
        }

        if (args[0] == "identify") {
            if (args.size() != 2) {
                usage();
                return 2;
            }
            const auto format = resolve_cli_archive_format("auto", args[1], true);
            const auto& info = superzip::archive_format_info(format);
            std::cout << "format=" << info.key
                      << " display=\"" << info.display_name << "\""
                      << " can_create=" << (info.can_create ? "true" : "false")
                      << " can_extract=" << (info.can_extract ? "true" : "false")
                      << " gpu_native=" << (info.gpu_native ? "true" : "false")
                      << " bundled_native=" << (info.bundled_native ? "true" : "false")
                      << "\n";
            return 0;
        }

        if (args[0] == "memory-benchmark") {
            MemoryBenchmarkOptions options;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--size-mib") {
                    options.size_mib = require_u32_arg(args, i, "--size-mib");
                } else if (args[i] == "--profile") {
                    options.profile = require_arg(args, i, "--profile");
                } else if (args[i] == "--require-gpu") {
                    options.require_gpu = true;
                } else if (args[i] == "--force-cpu") {
                    options.force_cpu = true;
                } else if (args[i] == "--workers") {
                    options.workers = require_u32_arg(args, i, "--workers");
                } else if (args[i] == "--block-size-kib") {
                    options.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
                } else if (args[i] == "--compression-level") {
                    options.compression_level = require_int_arg(
                        args,
                        i,
                        "--compression-level",
                        superzip::kMinCompressionLevel,
                        superzip::kMaxCompressionLevel);
                } else {
                    throw superzip::ArchiveError("unknown memory-benchmark argument: " + args[i]);
                }
            }
            print_memory_benchmark_stats(run_memory_benchmark(options));
            return 0;
        }

        if (args[0] == "benchmark-suite") {
            BenchmarkSuiteOptions options;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--size-mib") {
                    options.size_mib = require_u32_arg(args, i, "--size-mib");
                } else if (args[i] == "--profile") {
                    options.profile = require_arg(args, i, "--profile");
                } else if (args[i] == "--workers") {
                    options.workers = require_u32_arg(args, i, "--workers");
                } else if (args[i] == "--block-size-kib") {
                    options.block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
                } else if (args[i] == "--compression-level") {
                    options.compression_level = require_int_arg(
                        args,
                        i,
                        "--compression-level",
                        superzip::kMinCompressionLevel,
                        superzip::kMaxCompressionLevel);
                } else if (args[i] == "--tune") {
                    options.tune = true;
                } else if (args[i] == "--tune-levels") {
                    options.tune = true;
                    options.tune_levels = true;
                } else {
                    throw superzip::ArchiveError("unknown benchmark-suite argument: " + args[i]);
                }
            }
            run_benchmark_suite(options);
            return 0;
        }

        // Compression keeps ZIP compatibility and native SUZIP explicit.
        // `--force-cpu` is only for diagnostics and benchmarking.
        if (args[0] == "compress") {
            std::string format = "suzip";
            bool require_gpu = false;
            bool force_cpu = false;
            std::uint32_t workers = 0;
            std::uint32_t inflight = 0;
            std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
            bool suzip_tuning_requested = false;
            int compression_level = superzip::kDefaultCompressionLevel;
            bool verify_after_write = false;
            bool sha256 = false;
            bool defender_scan = false;
            std::filesystem::path output;
            std::vector<std::filesystem::path> sources;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--format") {
                    format = require_arg(args, i, "--format");
                } else if (args[i] == "--output") {
                    output = require_arg(args, i, "--output");
                } else if (args[i] == "--require-gpu") {
                    require_gpu = true;
                } else if (args[i] == "--force-cpu") {
                    force_cpu = true;
                } else if (args[i] == "--workers") {
                    workers = require_u32_arg(args, i, "--workers");
                    suzip_tuning_requested = true;
                } else if (args[i] == "--inflight") {
                    inflight = require_u32_arg(args, i, "--inflight");
                    suzip_tuning_requested = true;
                } else if (args[i] == "--block-size-kib") {
                    block_size = require_block_size_kib_arg(args, i, "--block-size-kib");
                    suzip_tuning_requested = true;
                } else if (args[i] == "--compression-level") {
                    compression_level = require_int_arg(
                        args,
                        i,
                        "--compression-level",
                        superzip::kMinCompressionLevel,
                        superzip::kMaxCompressionLevel);
                    suzip_tuning_requested = true;
                } else if (args[i] == "--verify-after-write") {
                    verify_after_write = true;
                    suzip_tuning_requested = true;
                } else if (args[i] == "--sha256") {
                    sha256 = true;
                } else if (args[i] == "--defender-scan") {
                    defender_scan = true;
                } else {
                    sources.emplace_back(args[i]);
                }
            }
            if (output.empty() || sources.empty()) {
                usage();
                return 2;
            }
            if (require_gpu && force_cpu) {
                throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
            }
            const auto archive_format = resolve_cli_archive_format(format, output, false);
            reject_unsupported_cli_format(archive_format, "create");
            if (archive_format == superzip::ArchiveFormat::SuperZip) {
                superzip::CompressOptions options;
                options.gpu_required = require_gpu;
                options.force_cpu = force_cpu;
                options.worker_count = workers;
                options.max_inflight_chunks = inflight;
                options.block_size = block_size;
                options.compression_level = compression_level;
                options.verify_after_write = verify_after_write;
                print_stats(superzip::compress_suzip(sources, output, options));
            } else if (archive_format == superzip::ArchiveFormat::Zip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("ZIP compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_zip(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::Tar) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_tar(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::TarGzip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.GZ compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_tar_gzip(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::TarBzip2) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.BZ2 compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_tar_bzip2(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::TarZstd) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.ZST compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_tar_zstd(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::Gzip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Gzip compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_gzip(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::Bzip2) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Bzip2 compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_bzip2(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::Zstd) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Zstandard compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_zstd(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::UnixCompress) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Unix Compress compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_unix_compress(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::Cpio) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("CPIO compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_cpio(sources, output));
            } else if (archive_format == superzip::ArchiveFormat::Ar) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("AR compatibility does not support SUZIP GPU, worker, block-size, compression-level, or verify-after-write flags");
                }
                print_stats(superzip::compress_ar(sources, output));
            }
            if (sha256) {
                print_integrity_hash(output);
            }
            if (defender_scan) {
                print_defender_scan(output, false);
            }
            return 0;
        }

        // Extraction performs optional integrity and Defender checks before
        // writing files, then relies on archive-layer overwrite and path
        // validation for the actual restore operation.
        if (args[0] == "extract") {
            std::string format = "auto";
            bool require_gpu = false;
            bool force_cpu = false;
            std::uint32_t workers = 0;
            std::uint32_t inflight = 0;
            bool suzip_tuning_requested = false;
            bool overwrite = false;
            bool sha256 = false;
            bool defender_scan = false;
            std::filesystem::path output;
            std::filesystem::path archive;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--format") {
                    format = require_arg(args, i, "--format");
                } else if (args[i] == "--output") {
                    output = require_arg(args, i, "--output");
                } else if (args[i] == "--require-gpu") {
                    require_gpu = true;
                } else if (args[i] == "--force-cpu") {
                    force_cpu = true;
                } else if (args[i] == "--workers") {
                    workers = require_u32_arg(args, i, "--workers");
                    suzip_tuning_requested = true;
                } else if (args[i] == "--inflight") {
                    inflight = require_u32_arg(args, i, "--inflight");
                    suzip_tuning_requested = true;
                } else if (args[i] == "--overwrite") {
                    overwrite = true;
                } else if (args[i] == "--sha256") {
                    sha256 = true;
                } else if (args[i] == "--defender-scan") {
                    defender_scan = true;
                } else {
                    archive = args[i];
                }
            }
            if (output.empty() || archive.empty()) {
                usage();
                return 2;
            }
            if (require_gpu && force_cpu) {
                throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
            }
            if (sha256) {
                print_integrity_hash(archive);
            }
            if (defender_scan) {
                print_defender_scan(archive, true);
            }
            const auto archive_format = resolve_cli_archive_format(format, archive, true);
            reject_unsupported_cli_format(archive_format, "extract");
            if (archive_format == superzip::ArchiveFormat::SuperZip) {
                superzip::ExtractOptions options;
                options.gpu_required = require_gpu;
                options.force_cpu = force_cpu;
                options.overwrite = overwrite;
                options.worker_count = workers;
                options.max_inflight_chunks = inflight;
                print_stats(superzip::extract_suzip(archive, output, options));
            } else if (archive_format == superzip::ArchiveFormat::Zip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("ZIP compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_zip(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::SevenZip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("7z compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_7z(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Tar) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_tar(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::TarGzip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.GZ compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_tar_gzip(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::TarBzip2) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.BZ2 compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_tar_bzip2(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::TarXz) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.XZ compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_tar_xz(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::TarZstd) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("TAR.ZST compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_tar_zstd(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Gzip) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Gzip compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_gzip_file(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Bzip2) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Bzip2 compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_bzip2_file(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Xz) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("XZ compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_xz_file(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Zstd) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Zstandard compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_zstd_file(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::UnixCompress) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("Unix Compress compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_unix_compress_file(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Cab) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("CAB compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_cab(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Iso) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("ISO compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_iso(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Cpio) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("CPIO compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_cpio(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Ar ||
                archive_format == superzip::ArchiveFormat::Deb) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("AR/DEB compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_ar(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Rpm) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("RPM compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_rpm(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Lha) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("LHA compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_lha(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Wim) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("WIM compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_wim(archive, output, overwrite));
            } else if (archive_format == superzip::ArchiveFormat::Xar) {
                if (require_gpu || force_cpu || suzip_tuning_requested) {
                    throw superzip::ArchiveError("XAR compatibility does not support SUZIP GPU, worker, or in-flight flags");
                }
                print_stats(superzip::extract_xar(archive, output, overwrite));
            }
            if (defender_scan) {
                print_defender_scan(output, false);
            }
            return 0;
        }

        // Verification reads SUZIP metadata and payload through the decoder
        // without creating output files, then optionally reports extra security
        // signals requested by the caller.
        if (args[0] == "verify") {
            bool require_gpu = false;
            bool force_cpu = false;
            std::uint32_t workers = 0;
            std::uint32_t inflight = 0;
            bool sha256 = false;
            bool defender_scan = false;
            std::filesystem::path archive;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--require-gpu") {
                    require_gpu = true;
                } else if (args[i] == "--force-cpu") {
                    force_cpu = true;
                } else if (args[i] == "--workers") {
                    workers = require_u32_arg(args, i, "--workers");
                } else if (args[i] == "--inflight") {
                    inflight = require_u32_arg(args, i, "--inflight");
                } else if (args[i] == "--sha256") {
                    sha256 = true;
                } else if (args[i] == "--defender-scan") {
                    defender_scan = true;
                } else {
                    archive = args[i];
                }
            }
            if (archive.empty()) {
                usage();
                return 2;
            }
            if (require_gpu && force_cpu) {
                throw superzip::GpuError("--require-gpu and --force-cpu are mutually exclusive");
            }
            superzip::ExtractOptions options;
            options.gpu_required = require_gpu;
            options.force_cpu = force_cpu;
            options.worker_count = workers;
            options.max_inflight_chunks = inflight;
            print_stats(superzip::verify_suzip(archive, options));
            if (sha256) {
                print_integrity_hash(archive);
            }
            if (defender_scan) {
                print_defender_scan(archive, false);
            }
            return 0;
        }

        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
