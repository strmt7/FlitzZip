#pragma once

#include "core/archive.hpp"

#include <cstdint>
#include <string>

namespace superzip::cli {

struct MemoryBenchmarkOptions {
    std::uint64_t size_mib = 10240;
    std::string profile = "Mixed";
    bool require_gpu = false;
    bool force_cpu = false;
    std::uint32_t workers = 0;
    std::uint32_t block_size = superzip::kDefaultArchiveBlockBytes;
    int compression_level = superzip::kDefaultCompressionLevel;
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

// Purpose: Run a no-filesystem CPU or AMD-HIP benchmark over generated in-memory chunks.
// Inputs: `options` controls virtual workload size, data profile, backend lane, worker count, and compression level.
// Outputs: Returns timing, byte counts, GPU telemetry, and correctness-checked phase statistics.
MemoryBenchmarkResult run_memory_benchmark(const MemoryBenchmarkOptions& options);

// Purpose: Print memory-only benchmark statistics in the stable SuperZip CLI key/value format.
// Inputs: `result` is the completed memory benchmark result.
// Outputs: Writes one parseable line to stdout.
void print_memory_benchmark_stats(const MemoryBenchmarkResult& result);

// Purpose: Run the built-in RAM-only scoring and autotuning suite.
// Inputs: `options` selects workload size, profile, worker count, block-size sweep, and compression-level sweep.
// Outputs: Prints candidate results and one recommendation; throws on invalid settings or hidden CPU fallback.
void run_benchmark_suite(const BenchmarkSuiteOptions& options);

}  // namespace superzip::cli
