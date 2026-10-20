#pragma once

#include "core/resource_limits.hpp"
#include "core/result.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace flitzzip::cli {

// Measured standard-mode observations used only for benchmark recommendation, not codec execution.
struct BenchmarkSuiteObservation {
    int compression_level = kDefaultCompressionLevel;
    std::uint32_t block_size = kDefaultArchiveBlockBytes;
    double gpu_score = 0.0;
    double compression_ratio = 1.0;  // Complete compressed bytes / input bytes; lower is better.
};

// Purpose: Choose the fastest observed standard-mode case within the balanced-level size guard.
// Inputs: Borrowed cases with finite nonnegative scores/ratios; level tuning requires a balanced observation.
// Outputs: Returns an original index with stable score ties; throws ArchiveError for missing/invalid evidence.
inline std::size_t select_benchmark_suite_recommendation(std::span<const BenchmarkSuiteObservation> observations,
                                                         bool tune_levels) {
    if (observations.empty()) {
        throw ArchiveError("benchmark suite requires observations");
    }
    auto baseline = observations.size();
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        if (!std::isfinite(observation.gpu_score) || observation.gpu_score < 0.0 ||
            !std::isfinite(observation.compression_ratio) || observation.compression_ratio < 0.0) {
            throw ArchiveError("benchmark suite requires finite nonnegative measurements");
        }
        if (observation.compression_level != kDefaultCompressionLevel) {
            continue;
        }
        // Prefer the product default geometry. If absent, retain the strongest measured balanced case.
        if (baseline == observations.size() ||
            (observation.block_size == kDefaultArchiveBlockBytes &&
             observations[baseline].block_size != kDefaultArchiveBlockBytes) ||
            (observation.block_size != kDefaultArchiveBlockBytes &&
             observations[baseline].block_size != kDefaultArchiveBlockBytes &&
             observation.compression_ratio < observations[baseline].compression_ratio)) {
            baseline = index;
        }
    }
    if (tune_levels && baseline == observations.size()) {
        throw ArchiveError("benchmark level tuning requires a balanced-level observation");
    }
    const auto max_ratio =
        tune_levels ? observations[baseline].compression_ratio * 1.02 : std::numeric_limits<double>::infinity();
    auto best = observations.size();
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const auto& observation = observations[index];
        if (observation.compression_ratio > max_ratio) {
            continue;
        }
        if (best == observations.size() || observation.gpu_score > observations[best].gpu_score) {
            best = index;
        }
    }
    // Nonempty finite observations always include an eligible case: the baseline, or all cases without tuning.
    return best;
}

}  // namespace flitzzip::cli
