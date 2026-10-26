#include "native_window_visual_fixture.hpp"

namespace {

using namespace flitzzip_test::native_window;

// Purpose: Exercise applied logging through a real shell-suppressed public command.
// Inputs: Shared hidden window and isolated settings/log files. Outputs: Native draft and runtime-policy oracles.
class LoggingSession final {
  public:
    VisualSession ui;

    // Purpose: Reach Settings with its independently configured Debug baseline.
    // Inputs: Fixture-owned native window. Outputs: Selected Settings page with acknowledged Debug field.
    LoggingSession() {
        settings();
        ui.require_field(native_settings_layout(ui.window).log_level, L"Debug");
    }

    // Purpose: Reach actual Settings without injecting model values.
    // Inputs: Idle window. Outputs: Selected Settings page through ordinary navigation.
    void settings() {
        click_native_navigation(ui.window, 6);
        require_native_navigation_selection(ui.window, ui.frame.dc(), 6);
    }

    // Purpose: Change only the draft log level and recognize its literal display value.
    // Inputs: One of three ordinary policy indices. Outputs: Selected native field, without pressing Apply.
    void draft(int index) {
        constexpr std::array<std::wstring_view, 3> labels{L"Information", L"Warning", L"Debug"};
        const auto field = native_settings_layout(ui.window).log_level;
        ui.choose_option(field, index);
        ui.require_field(field, labels.at(index));
    }

    // Purpose: Attempt real atomic preference publication, including controlled Windows refusal.
    // Inputs: Current draft. Outputs: Acknowledged Apply command, never a direct model mutation.
    void apply() const {
        click_native_control(ui.window, native_settings_layout(ui.window).apply);
    }

    // Purpose: Exercise the real reset command without restoring private state from the test.
    // Inputs: Current draft and file-sharing conditions. Outputs: Acknowledged default-reset attempt.
    void defaults() const {
        click_native_control(ui.window, native_settings_layout(ui.window).restore_defaults);
    }

    // Purpose: Generate a genuine Debug-category event without launching Explorer.
    // Inputs: Actual Settings command and the shared process-local smoke flag. Outputs: Completed native handler.
    void open_log() const {
        click_native_control(ui.window, native_settings_layout(ui.window).open_log_file);
    }

    // Purpose: Observe the saved policy independently of its parser.
    // Inputs: Existing fixture-owned settings file. Outputs: Exact bytes with no retained file stream.
    std::string saved() const {
        return read_native_fixture(flitzzip::app::settings_file_path());
    }
};

constexpr std::string_view kOpened = "Log file location opened";

}  // namespace

// Purpose: A narrower unapplied draft must not suppress events allowed by the active Debug policy.
// Inputs: Real log-open positive control, Warning draft and subsequent abandoned draft.
// Outputs: Saved bytes unchanged and all three Debug events retained through ordinary commands.
TEST_CASE(main_window_logging_warning_draft_preserves_applied_debug_policy) {
    LoggingSession s;
    s.open_log();
    require_native_log_count(kOpened, 1);
    const auto before = s.saved();
    s.draft(1);
    REQUIRE_EQ(s.saved(), before);
    s.open_log();
    require_native_log_count(kOpened, 2);
    click_native_navigation(s.ui.window, 7);
    s.settings();
    s.ui.require_field(native_settings_layout(s.ui.window).log_level, L"Debug");
    s.open_log();
    require_native_log_count(kOpened, 3);
    s.ui.finish();
}

// Purpose: A broader unapplied draft must not enable events refused by the active Warning policy.
// Inputs: Applied Warning baseline, Debug draft and later successful Apply.
// Outputs: No Debug output before publication, exact saved bytes preserved, then one event after publication.
TEST_CASE(main_window_logging_debug_draft_does_not_enable_runtime_events) {
    LoggingSession s;
    s.draft(1);
    s.apply();
    s.open_log();
    require_native_log_count(kOpened, 0);
    const auto before = s.saved();
    s.draft(2);
    s.open_log();
    require_native_log_count(kOpened, 0);
    REQUIRE_EQ(s.saved(), before);
    s.apply();
    s.open_log();
    require_native_log_count(kOpened, 1);
    s.ui.finish();
}

// Purpose: Failed persistence must leave the applied log policy and saved file unchanged until a valid retry.
// Inputs: Debug baseline, Warning draft and real no-delete-sharing refusal on the settings file.
// Outputs: Debug events still recorded after failure; later successful Warning Apply suppresses them.
TEST_CASE(main_window_logging_failed_apply_preserves_runtime_policy_and_recovers) {
    LoggingSession s;
    s.open_log();
    const auto before = s.saved();
    s.draft(1);
    {
        auto held = hold_native_replacement_target(flitzzip::app::settings_file_path());
        s.apply();
        REQUIRE_EQ(s.saved(), before);
        s.open_log();
        require_native_log_count(kOpened, 2);
    }
    s.apply();
    s.open_log();
    require_native_log_count(kOpened, 2);
    s.ui.finish();
}

// Purpose: Failed default restoration must preserve the active policy even when the current draft differs.
// Inputs: Applied Warning policy, Debug draft and real settings-file replacement refusal.
// Outputs: Old bytes and visible draft survive, Debug remains suppressed, and a subsequent Apply recovers.
TEST_CASE(main_window_logging_failed_defaults_preserves_runtime_policy_and_draft) {
    LoggingSession s;
    s.draft(1);
    s.apply();
    s.draft(2);
    const auto before = s.saved();
    {
        auto held = hold_native_replacement_target(flitzzip::app::settings_file_path());
        s.defaults();
        REQUIRE_EQ(s.saved(), before);
        s.ui.require_field(native_settings_layout(s.ui.window).log_level, L"Debug");
        s.open_log();
        require_native_log_count(kOpened, 0);
    }
    s.apply();
    s.open_log();
    require_native_log_count(kOpened, 1);
    s.ui.finish();
}

// Purpose: Successful default restoration must replace both the draft and active logging policy together.
// Inputs: Debug positive control and a different draft. Outputs: Persisted Information policy and no new Debug event.
TEST_CASE(main_window_logging_successful_defaults_publishes_information_policy) {
    LoggingSession s;
    s.open_log();
    require_native_log_count(kOpened, 1);
    s.draft(1);
    s.defaults();
    s.ui.require_field(native_settings_layout(s.ui.window).log_level, L"Information");
    REQUIRE_TRUE(s.saved().find("\"logLevel\": \"information\"") != std::string::npos);
    s.open_log();
    require_native_log_count(kOpened, 1);
    s.ui.finish();
}

// Purpose: Applied policies must affect actual event admission rather than merely update a serialized index.
// Inputs: Five ordinary policy transitions through the public dropdown and Apply.
// Outputs: Exact cumulative Debug-event truth table and matching saved selection at every step.
TEST_CASE(main_window_logging_applied_transitions_follow_independent_event_truth_table) {
    LoggingSession s;
    constexpr std::array<std::pair<int, std::size_t>, 5> cases{{{1, 0}, {0, 0}, {2, 1}, {1, 1}, {2, 2}}};
    for (const auto [level, count] : cases) {
        s.draft(level);
        s.apply();
        s.open_log();
        require_native_log_count(kOpened, count);
        constexpr std::array<std::string_view, 3> identifiers{"information", "warning", "debug"};
        REQUIRE_TRUE(s.saved().find("\"logLevel\": \"" + std::string(identifiers[static_cast<std::size_t>(level)]) +
                                    '"') != std::string::npos);
    }
    s.ui.finish();
}
