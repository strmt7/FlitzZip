#pragma once

#include "core/archive.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace superzip::cli {

struct MemoryBenchmarkOptions {
    // Borrowed immutable snapshot; caller retains it through all joined tasks. Empty requires metadata or generated
    // input.
    std::span<const std::byte> source;
    // Metadata-only planning reserves this future resident input; measured runs require the actual snapshot.
    std::uint64_t corpus_bytes = 0;
    std::string expected_source_sha256;
    std::uint64_t size_mib = 10240;
    std::string profile = "Mixed";
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    // Zero selects admission automatically; nonzero depths are exact and never silently reduced.
    std::uint32_t inflight_chunks = 0;
    std::uint32_t decode_inflight_chunks = 0;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
    NativeCompressionMode compression_mode = NativeCompressionMode::Standard;
};

struct MemoryBenchmarkPlan {
    NativeCompressionMode compression_mode = NativeCompressionMode::Standard;
    std::string expected_source_sha256;
    std::uint64_t input_bytes = 0;
    std::uint32_t workers = 0;
    std::uint32_t inflight_chunks = 0;
    std::uint32_t codec_workers = 0;
    std::uint32_t decode_inflight_chunks = 0;
    std::uint32_t decode_codec_workers = 0;
};

struct MemoryBenchmarkResult {
    std::string source_sha256;
    superzip::OperationStats stats;
    std::uint64_t archive_bytes = 0;
    std::uint32_t codec_workers = 1;
    std::uint32_t decode_inflight_chunks = 1;
    std::uint32_t decode_codec_workers = 1;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
    NativeCompressionMode compression_mode = NativeCompressionMode::Standard;
    double compress_seconds = 0.0;
    // Summed task time, including overlap; neither field is elapsed wall time.
    double source_generation_worker_seconds = 0.0;
    double codec_encode_worker_seconds = 0.0;
    double verify_seconds = 0.0;
    double extract_seconds = 0.0;
    // Independent post-phase decode/regeneration; excluded from product phase timings and telemetry.
    std::uint64_t validated_bytes = 0;
    double validation_seconds = 0.0;
    double wall_seconds = 0.0;
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

// Purpose: Run a no-filesystem CPU or AMD-HIP benchmark over generated in-memory chunks.
// Inputs: `options` controls virtual workload size, data profile, backend lane, worker count, and compression level.
// Outputs: Returns timing, byte counts, GPU telemetry, and correctness-checked phase statistics.
MemoryBenchmarkResult run_memory_benchmark(const MemoryBenchmarkOptions& options);

// Purpose: Resolve exact benchmark geometry without generating data or executing codecs.
// Inputs: options selects the same workload and admission policy as a measured run.
// Outputs: Returns admitted geometry or throws if a requested depth cannot safely fit now.
MemoryBenchmarkPlan plan_memory_benchmark(const MemoryBenchmarkOptions& options);

// Purpose: Print admission planning fields without presenting a plan as measured GPU or timing evidence.
// Inputs: plan is a validated, allocation-free benchmark configuration.
// Outputs: Writes one parseable plan_only=true line to stdout.
void print_memory_benchmark_plan(const MemoryBenchmarkPlan& plan);

// Purpose: Print memory-only benchmark statistics in the stable SuperZip CLI key/value format.
// Inputs: `result` is the completed memory benchmark result.
// Outputs: Writes one parseable line to stdout.
void print_memory_benchmark_stats(const MemoryBenchmarkResult& result);

// Purpose: Run the built-in RAM-only scoring and autotuning suite.
// Inputs: `options` selects workload size, profile, worker count, block-size sweep, and compression-level sweep.
// Outputs: Prints candidate results and one recommendation; throws on invalid settings or hidden CPU fallback.
void run_benchmark_suite(const BenchmarkSuiteOptions& options);

}  // namespace superzip::cli
