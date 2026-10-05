#include "gpu/neutron_stage_clock.hpp"
#include "test_util.hpp"

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

struct StageState {
    unsigned int created = 0U;
    unsigned int destroyed = 0U;
    unsigned int submissions = 0U;
    unsigned int completions = 0U;
    bool pending = false;
    bool fail_dispatch = false;
    bool fail_completion = false;
    const int* observed_owner = nullptr;
    std::optional<double> elapsed = 0.25;
    std::vector<char> calls;
};

// A host-only event owner: tests call the production clock without importing or initializing HIP.
struct TestEvents {
    StageState* state;
    std::unique_ptr<int> identity;

    // Purpose: Account for one independently owned test event pair.
    // Inputs: State outlives the event owner and records backend interactions.
    // Outputs: Creates one identity and increments creation accounting.
    explicit TestEvents(StageState& value) : state(&value), identity(std::make_unique<int>(0)) {
        ++state->created;
    }

    // Purpose: Transfer the test owner without creating another pair.
    // Inputs: A live or already moved owner.
    // Outputs: Moves its identity; only the final owner accounts for destruction.
    TestEvents(TestEvents&&) = default;
    TestEvents(const TestEvents&) = delete;
    TestEvents& operator=(const TestEvents&) = delete;
    TestEvents& operator=(TestEvents&&) = delete;

    // Purpose: Observe final ownership release on successful and exceptional exits.
    // Inputs: The original state remains live; moved owners have no identity.
    // Outputs: Increments destruction accounting once without throwing.
    ~TestEvents() {
        if (identity) {
            ++state->destroyed;
        }
    }

    // Purpose: Detect dispatch before completion and substitution of timing owners.
    // Inputs: The production clock borrows this owner for one simulated stage.
    // Outputs: Marks work pending or throws the configured dispatch failure.
    void dispatch() const {
        ++state->submissions;
        REQUIRE_TRUE(!state->pending);
        if (state->observed_owner == nullptr) {
            state->observed_owner = identity.get();
        }
        REQUIRE_EQ(state->observed_owner, identity.get());
        state->pending = true;
        state->calls.push_back('D');
        if (state->fail_dispatch) {
            throw std::runtime_error("stage dispatch failed");
        }
    }

    // Purpose: Distinguish successful completion from a failed synchronization call.
    // Inputs: One pending simulated stage and optional configured timestamp availability.
    // Outputs: Clears pending work and returns timing, or propagates the configured completion failure.
    std::optional<double> complete() const {
        ++state->completions;
        REQUIRE_TRUE(state->pending);
        REQUIRE_EQ(state->observed_owner, identity.get());
        state->calls.push_back('C');
        if (state->fail_completion) {
            throw std::runtime_error("stage completion failed");
        }
        state->pending = false;
        return state->elapsed;
    }
};

using Clock = superzip::dictionary::NeutronStageClock<TestEvents>;
const auto dispatch = std::mem_fn(&TestEvents::dispatch);
const auto complete = std::mem_fn(&TestEvents::complete);

}  // namespace

// Purpose: Prevent per-stage owner churn and recording again before completion.
// Inputs: Five stages through the production clock with an independently instrumented host-only backend.
// Outputs: Requires one owner, alternating dispatch/completion, exact totals and one final release.
TEST_CASE(neutron_stage_clock_reuses_completed_owner) {
    StageState state;
    {
        Clock clock{TestEvents{state}};
        for (unsigned int stage = 0U; stage < 5U; ++stage) {
            clock.measure(dispatch, complete);
        }
        REQUIRE_EQ(state.created, 1U);
        REQUIRE_EQ(state.destroyed, 0U);
        REQUIRE_TRUE(!state.pending);
        REQUIRE_EQ(clock.launches(), 5U);
        REQUIRE_EQ(clock.milliseconds(), std::optional<double>{1.25});
        REQUIRE_EQ(state.calls, (std::vector<char>{'D', 'C', 'D', 'C', 'D', 'C', 'D', 'C', 'D', 'C'}));
    }
    REQUIRE_EQ(state.destroyed, 1U);
}

// Purpose: Keep independent operations from sharing mutable timing events.
// Inputs: Two simultaneously live clocks whose stages are interleaved deterministically.
// Outputs: Requires distinct stable owners and independent timing/count state.
TEST_CASE(neutron_stage_clock_operation_isolation) {
    StageState first;
    StageState second;
    second.elapsed = 0.5;
    Clock left{TestEvents{first}};
    Clock right{TestEvents{second}};
    left.measure(dispatch, complete);
    right.measure(dispatch, complete);
    left.measure(dispatch, complete);
    REQUIRE_TRUE(first.observed_owner != second.observed_owner);
    REQUIRE_EQ(left.launches(), 2U);
    REQUIRE_EQ(right.launches(), 1U);
    REQUIRE_EQ(left.milliseconds(), std::optional<double>{0.5});
    REQUIRE_EQ(right.milliseconds(), std::optional<double>{0.5});
}

// Purpose: Stop a failed session from recording another kernel on potentially incomplete events.
// Inputs: Separate failures during dispatch and completion, followed by an attempted retry on that same clock.
// Outputs: Requires the original error, no completed count, no retry backend call and one ownership release.
TEST_CASE(neutron_stage_clock_failure_rejects_reuse) {
    for (const bool during_dispatch : {false, true}) {
        StageState state;
        state.fail_dispatch = during_dispatch;
        state.fail_completion = !during_dispatch;
        {
            Clock clock{TestEvents{state}};
            bool failed = false;
            try {
                clock.measure(dispatch, complete);
            } catch (const std::runtime_error& error) {
                failed = std::string_view(error.what()) ==
                         (during_dispatch ? "stage dispatch failed" : "stage completion failed");
            }
            REQUIRE_TRUE(failed);
            REQUIRE_EQ(clock.launches(), 0U);
            REQUIRE_EQ(clock.milliseconds(), std::optional<double>{0.0});
            state.fail_dispatch = state.fail_completion = false;
            bool rejected = false;
            try {
                clock.measure(dispatch, complete);
            } catch (const superzip::GpuError&) {
                rejected = true;
            }
            REQUIRE_TRUE(rejected);
            REQUIRE_EQ(state.submissions, 1U);
            REQUIRE_EQ(state.completions, during_dispatch ? 0U : 1U);
        }
        REQUIRE_EQ(state.created, 1U);
        REQUIRE_EQ(state.destroyed, 1U);
    }
}

// Purpose: Preserve unavailable or invalid timing while retaining successful completion accounting.
// Inputs: Missing, negative, nonfinite and overflowing measurements followed by valid completed stages.
// Outputs: Requires sticky unavailable totals and counts every successfully completed dispatch.
TEST_CASE(neutron_stage_clock_timing_availability) {
    for (const auto elapsed : {std::optional<double>{}, std::optional<double>{-1.0},
                               std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
                               std::optional<double>{std::numeric_limits<double>::infinity()}}) {
        StageState state;
        state.elapsed = elapsed;
        Clock clock{TestEvents{state}};
        clock.measure(dispatch, complete);
        state.elapsed = 0.25;
        clock.measure(dispatch, complete);
        REQUIRE_TRUE(!clock.milliseconds());
        REQUIRE_EQ(clock.launches(), 2U);
    }
    StageState state;
    state.elapsed = std::numeric_limits<double>::max();
    Clock clock{TestEvents{state}};
    clock.measure(dispatch, complete);
    REQUIRE_EQ(clock.milliseconds(), state.elapsed);
    clock.measure(dispatch, complete);
    REQUIRE_TRUE(!clock.milliseconds());
    REQUIRE_EQ(clock.launches(), 2U);
}
