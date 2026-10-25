#include "native_window_job_fixture.hpp"

using namespace flitzzip_test::native_window;

// Purpose: Select the successful primary result despite incompatible History filters at job start.
// Inputs: Real ZIP worker held before work and ordinary applied summary preference.
// Outputs: Completion resets filters, selects the literal result and preserves source/archive readback.
TEST_CASE(main_window_job_summary_success_resets_filters_and_selects_result) {
    NativeJobSession s;
    s.preferences(true, false);
    s.history.open();
    s.history.filter(4, 2);
    s.start();
    s.complete();
    s.require_summary();
    s.more_input();
    s.require_log_count("Operation summary shown: Compress", 1);
    s.finish();
}

// Purpose: Present an actual failed writer result with the same selection contract as success.
// Inputs: Real destination sharing refusal. Outputs: Failure glyphs/selection and untouched preexisting bytes.
TEST_CASE(main_window_job_summary_failure_selects_real_result) {
    NativeJobSession s;
    s.preferences(true, false);
    s.start(NativeJobResult::Failure);
    s.complete();
    s.require_summary();
    s.finish();
}

// Purpose: Preserve user cancellation as a visible completed result without publishing an archive.
// Inputs: Native Stop while the admitted worker is gated. Outputs: Selected Compress stopped row, empty output.
TEST_CASE(main_window_job_summary_cancel_selects_stopped_result) {
    NativeJobSession s;
    s.preferences(true, false);
    s.start(NativeJobResult::Cancelled);
    s.history.ui.key(VK_HOME);
    for (int index = 0; index < 6; ++index) {
        s.history.ui.key(VK_RIGHT);
    }
    s.history.ui.key(VK_RETURN);
    require_native_navigation_selection(s.history.ui.window, s.history.ui.frame.dc(), 1);
    s.complete();
    s.require_summary();
    s.finish();
}

// Purpose: Keep applied preferences unchanged when active work refuses page transitions.
// Inputs: Held ZIP job and native pointer requests for Settings and About.
// Outputs: Compression remains selected, saved bytes stay exact and completion honors disabled presentation.
TEST_CASE(main_window_job_active_navigation_preserves_applied_settings) {
    NativeJobSession s;
    const auto settings = read_native_fixture(flitzzip::app::settings_file_path());
    s.start();
    for (const int page : {6, 7, 4, 0}) {
        click_native_navigation(s.history.ui.window, page);
        require_native_navigation_selection(s.history.ui.window, s.history.ui.frame.dc(), 1);
        REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), settings);
    }
    s.complete();
    require_native_navigation_selection(s.history.ui.window, s.history.ui.frame.dc(), 1);
    s.require_log_count("Operation summary shown: Compress", 0);
    s.require_destination(s.history.ui.fixture.output, 0);
    s.finish();
}

// Purpose: Apply a changed summary preference only to later admission without retaining a stale selection.
// Inputs: First successful job with summary disabled, followed by an applied preference and second real ZIP.
// Outputs: First completion stays on Compress; next completion selects only its own primary result.
TEST_CASE(main_window_job_summary_enabled_next_job_selects_new_result) {
    NativeJobSession s;
    s.start();
    s.complete();
    require_native_navigation_selection(s.history.ui.window, s.history.ui.frame.dc(), 1);
    s.require_log_count("Operation summary shown: Compress", 0);
    s.preferences(true, false);
    s.history.compress(true);
    s.settle();
    s.require_summary();
    s.finish();
}

// Purpose: Drop a consumed summary when later admission disables presentation.
// Inputs: Successful summary-enabled job, applied disabled preference and a second real ZIP.
// Outputs: The second job stays on Compress and never replays the prior History selection.
TEST_CASE(main_window_job_summary_disabled_next_job_does_not_replay_selection) {
    NativeJobSession s;
    s.preferences(true, false);
    s.start();
    s.complete();
    s.require_summary();
    s.preferences(false, false);
    s.history.compress(true);
    s.settle();
    require_native_navigation_selection(s.history.ui.window, s.history.ui.frame.dc(), 1);
    s.require_log_count("Operation summary shown: Compress", 1);
    s.finish();
}

// Purpose: Keep a visible unapplied Settings draft separate from an active job and subsequent admission.
// Inputs: Both preferences changed without Apply before native compression is started.
// Outputs: Neither summary nor destination dispatch occurs, and both abandoned switches restore on re-entry.
TEST_CASE(main_window_job_unapplied_preferences_do_not_change_admission) {
    NativeJobSession s;
    s.preferences(true, true, false);
    s.start();
    s.complete();
    require_native_navigation_selection(s.history.ui.window, s.history.ui.frame.dc(), 1);
    s.require_log_count("Operation summary shown: Compress", 0);
    s.require_destination(s.history.ui.fixture.output, 0);
    s.preferences(false, false, false);
    s.finish();
}

// Purpose: Clear idle History between jobs without losing later selection or archive admission.
// Inputs: Summary-enabled ZIP jobs and real Clear before admission and after completion.
// Outputs: Each new primary row selects safely at index zero, both archives remain readable and reusable.
TEST_CASE(main_window_job_history_clear_between_jobs_preserves_completion_and_reuse) {
    NativeJobSession s;
    s.preferences(true, false);
    s.history.open();
    s.history.clear();
    s.start();
    s.complete();
    s.require_summary();
    s.history.clear();
    s.history.compress(true);
    s.settle();
    s.require_summary(2);
    s.finish();
}

// Purpose: Dispatch exactly one captured destination request after actual successful publication.
// Inputs: Applied open preference, shell-suppressed owned destination and real ZIP worker.
// Outputs: One full-path request, no visible Shell operation and no duplicate on subsequent input.
TEST_CASE(main_window_job_destination_success_dispatches_once) {
    NativeJobSession s;
    s.preferences(false, true);
    s.start();
    s.complete();
    s.require_destination(s.history.ui.fixture.output, 1);
    s.more_input();
    s.require_destination(s.history.ui.fixture.output, 1);
    s.finish();
}

// Purpose: An actual publication failure must never open a destination as though work succeeded.
// Inputs: Captured open preference and real Windows no-delete-sharing target.
// Outputs: No open request, one failed History entry and preserved sentinel.
TEST_CASE(main_window_job_destination_failure_does_not_dispatch) {
    NativeJobSession s;
    s.preferences(false, true);
    s.start(NativeJobResult::Failure);
    s.complete();
    s.require_destination(s.history.ui.fixture.output, 0);
    s.history.open();
    s.history.require_row(1, 1);
    s.finish();
}

// Purpose: Cancellation must not dispatch the captured successful-destination action.
// Inputs: Open preference on and Stop before work. Outputs: No open request or archive publication.
TEST_CASE(main_window_job_destination_cancel_does_not_dispatch) {
    NativeJobSession s;
    s.preferences(false, true);
    s.start(NativeJobResult::Cancelled);
    s.complete();
    s.require_destination(s.history.ui.fixture.output, 0);
    s.finish();
}

// Purpose: Apply destination opening only to jobs admitted after the preference is saved.
// Inputs: First job with opening disabled, then Apply and a second real ZIP into a distinct destination.
// Outputs: No first request and exactly one request for the later job's directory.
TEST_CASE(main_window_job_destination_enabled_next_job_uses_new_capture) {
    NativeJobSession s;
    s.start();
    s.complete();
    s.require_destination(s.history.ui.fixture.output, 0);
    s.preferences(false, true);
    s.history.compress(true);
    s.settle();
    s.require_destination(s.history.ui.fixture.output / L"job-1", 1);
    s.finish();
}

// Purpose: Do not replay an older successful destination action when later admission disables opening.
// Inputs: One successful enabled job, Apply disabled, then another real ZIP.
// Outputs: Exactly one original request and no request for the second directory.
TEST_CASE(main_window_job_destination_disabled_next_job_does_not_replay_request) {
    NativeJobSession s;
    s.preferences(false, true);
    s.start();
    s.complete();
    s.require_destination(s.history.ui.fixture.output, 1);
    s.preferences(false, false);
    s.history.compress(true);
    s.settle();
    s.require_destination(s.history.ui.fixture.output, 1);
    s.finish();
}

// Purpose: Keep a running writer and its completion action bound to the destination captured together at start.
// Inputs: Two owned destinations and a picker change while work is held.
// Outputs: Archive/readback and one open request in the original directory; alternate directory remains empty.
TEST_CASE(main_window_job_destination_change_midjob_preserves_captured_path) {
    NativeJobSession s;
    s.preferences(false, true);
    s.start();
    const auto alternate = s.history.ui.fixture.root / L"alternate-destination";
    std::filesystem::create_directory(alternate);
    flitzzip_test::ScopedEnvironment picked(L"FLITZZIP_GUI_SMOKE_DESTINATION");
    picked.set(alternate.c_str());
    click_native_control(s.history.ui.window, native_compression_layout(s.history.ui.window).destination);
    s.complete();
    s.require_destination(s.history.ui.fixture.output, 1);
    REQUIRE_TRUE(std::filesystem::is_empty(alternate));
    s.finish();
}

// Purpose: Present both captured completion actions without one consuming or blocking the other.
// Inputs: Applied summary and destination preferences, real success and observation-only FIFO fence.
// Outputs: Primary result selected and one destination request, both still one-shot after further input.
TEST_CASE(main_window_job_summary_and_destination_are_independent_one_shot_actions) {
    NativeJobSession s;
    s.preferences(true, true);
    s.start();
    s.complete();
    s.require_summary();
    s.require_destination(s.history.ui.fixture.output, 1);
    s.more_input();
    s.require_log_count("Operation summary shown: Compress", 1);
    s.require_destination(s.history.ui.fixture.output, 1);
    s.finish();
}
