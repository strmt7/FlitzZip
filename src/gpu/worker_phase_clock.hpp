#pragma once

#include <chrono>
#include <utility>

namespace flitzzip {

struct SteadyWorkerClock {
    [[nodiscard]] auto operator()() const noexcept {
        return std::chrono::steady_clock::now();
    }
};

// Thread-local phase boundaries for optional worker profiling. A disabled clock
// never reads its clock or invokes a recorder. Copies retain the same boundary,
// allowing nested classification without resetting the enclosing encode phase.
template <typename Clock = SteadyWorkerClock> class WorkerPhaseClock {
  public:
    explicit WorkerPhaseClock(bool enabled, Clock clock = {})
        : clock_(std::move(clock)), started_(enabled ? clock_() : TimePoint{}), enabled_(enabled) {}

    // The recorder borrows the interval synchronously. Advance only after it
    // succeeds, preserving the preceding boundary if recording throws.
    template <typename Recorder> void finish(Recorder&& recorder) {
        if (!enabled_) {
            return;
        }
        const auto finished = clock_();
        std::forward<Recorder>(recorder)(finished - started_);
        started_ = finished;
    }

  private:
    using TimePoint = decltype(std::declval<Clock&>()());
    [[no_unique_address]] Clock clock_;
    TimePoint started_;
    const bool enabled_;
};

}  // namespace flitzzip
