#pragma once

#include "app/performance_counters.hpp"

#include <string>
#include <utility>

namespace flitzzip::app {

inline constexpr wchar_t kSystemCpuCounterPath[] = L"\\Processor Information(_Total)\\% Processor Time";

struct NativePdhBackend {
    // Purpose: Open an owned local query. Inputs: Output handle. Outputs: Native status and acquired query.
    PDH_STATUS open(PDH_HQUERY* query) noexcept {
        return PdhOpenQueryW(nullptr, 0, query);
    }
    // Purpose: Bind a locale-independent counter. Inputs: Query, path, output handle. Outputs: Native status.
    PDH_STATUS add(PDH_HQUERY query, const wchar_t* path, PDH_HCOUNTER* counter) noexcept {
        return PdhAddEnglishCounterW(query, path, 0, counter);
    }
    // Purpose: Advance the native rate baseline. Inputs: Owned query. Outputs: Native collection status.
    PDH_STATUS collect(PDH_HQUERY query) noexcept {
        return PdhCollectQueryData(query);
    }
    // Purpose: Read a formatted rate. Inputs: Counter and output value. Outputs: Native status and sample.
    PDH_STATUS read(PDH_HCOUNTER counter, PDH_FMT_COUNTERVALUE* value) noexcept {
        return PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, value);
    }
    // Purpose: Release a query and its counters. Inputs: Owned query. Outputs: Native resources released.
    void close(PDH_HQUERY query) noexcept {
        (void)PdhCloseQuery(query);
    }
};

// The backend seam exercises native failure/lifetime contracts without requiring a damaged host counter service.
template <class Backend = NativePdhBackend> class PdhPercentageSampler {
  public:
    // Purpose: Own one explicit counter path and its platform boundary without acquiring native resources yet.
    // Inputs: Counter path and backend value. Outputs: An initially unmeasured sampler with unique query ownership.
    explicit PdhPercentageSampler(std::wstring path, Backend backend = {})
        : path_(std::move(path)), backend_(std::move(backend)) {}
    PdhPercentageSampler(const PdhPercentageSampler&) = delete;
    PdhPercentageSampler& operator=(const PdhPercentageSampler&) = delete;

    // Purpose: Release every acquired query on destruction. Inputs: Owned state. Outputs: No retained handles.
    ~PdhPercentageSampler() {
        reset();
    }

    // Purpose: Invalidate a failed rate baseline and close its query exactly once.
    // Inputs: Owned query/counter state. Outputs: An unmeasured sampler ready for a subsequent independent query.
    void reset() noexcept {
        if (query_ != nullptr) {
            backend_.close(query_);
        }
        query_ = nullptr;
        counter_ = nullptr;
    }

    // Purpose: Sample a bounded percentage while preserving warmup and every independent native failure.
    // Inputs: Owned path, backend and preceding native rate baseline.
    // Outputs: An observed percentage including zero, or no value; failed query acquisition/collection is cleaned up.
    std::optional<double> sample() noexcept {
        if (query_ == nullptr) {
            if (backend_.open(&query_) != ERROR_SUCCESS || query_ == nullptr ||
                backend_.add(query_, path_.c_str(), &counter_) != ERROR_SUCCESS || counter_ == nullptr) {
                reset();
                return std::nullopt;
            }
        }
        if (backend_.collect(query_) != ERROR_SUCCESS) {
            reset();
            return std::nullopt;
        }
        PDH_FMT_COUNTERVALUE formatted{};
        const auto status = backend_.read(counter_, &formatted);
        const auto value = admitted_pdh_counter(status, formatted.CStatus, formatted.doubleValue);
        return value && *value <= 100.0 ? value : std::nullopt;
    }

  private:
    std::wstring path_;
    Backend backend_;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};

}  // namespace flitzzip::app
