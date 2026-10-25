#include "app/main_window_support.hpp"
#include "test_util.hpp"

namespace {

using namespace flitzzip::app;

// Purpose: Keep draft observations independent of runtime pruning and policy helpers.
// Inputs: Three timestamped literal rows spanning every retention choice. Outputs: Owned model with a month draft.
UiState state_with_retained_rows() {
    UiState state;
    state.log_retention_index = 2;
    const auto now = std::chrono::system_clock::now();
    state.logs = {{LogSeverity::Warning, "twenty-day row", now - std::chrono::days(20)},
                  {LogSeverity::Information, "ten-day row", now - std::chrono::days(10)},
                  {LogSeverity::Debug, "current row", now}};
    return state;
}

// Purpose: Reject deletion, rewriting or timestamp changes caused by editing an unapplied draft.
// Inputs: Actual rows and an independently retained original. Outputs: Exact row order, value and time equality.
void require_same_log_rows(const std::vector<LogEntry>& actual, const std::vector<LogEntry>& expected) {
    REQUIRE_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_EQ(actual[index].severity, expected[index].severity);
        REQUIRE_EQ(actual[index].message, expected[index].message);
        REQUIRE_EQ(actual[index].timestamp, expected[index].timestamp);
    }
}

}  // namespace

// Purpose: Shortening an unapplied retention draft must not irreversibly delete rows from an active month policy.
// Inputs: Every public retention choice beside twenty-day, ten-day and current rows.
// Outputs: All original rows survive; the draft and literal feedback still reflect the selected choice.
TEST_CASE(settings_retention_draft_preserves_rows_before_apply) {
    constexpr std::array<std::string_view, 3> feedback_text{
        "Log retention changed to 1 week", "Log retention changed to 2 weeks", "Log retention changed to 1 month"};
    for (const int choice : {0, 1, 2}) {
        auto state = state_with_retained_rows();
        const auto original = state.logs;
        DropdownSelectionFeedback feedback;
        apply_settings_dropdown_selection(state, DropdownId::SettingsLogRetention, choice, feedback);
        require_same_log_rows(state.logs, original);
        REQUIRE_EQ(state.log_retention_index, choice);
        REQUIRE_TRUE(feedback.add_log);
        REQUIRE_EQ(feedback.log_message, feedback_text.at(choice));
    }
}

// Purpose: Clamp draft policy inputs without changing retained runtime data or unrelated preference fields.
// Inputs: Both extremes and all legal log-level indices. Outputs: Independent draft truth table and exact row bytes.
TEST_CASE(settings_log_level_draft_clamps_without_runtime_side_effects) {
    constexpr std::array<std::pair<int, int>, 5> cases{{{-999, 0}, {0, 0}, {1, 1}, {2, 2}, {999, 2}}};
    for (const auto [input, expected] : cases) {
        auto state = state_with_retained_rows();
        const auto original = state.logs;
        DropdownSelectionFeedback feedback;
        apply_settings_dropdown_selection(state, DropdownId::SettingsLogLevel, input, feedback);
        REQUIRE_EQ(state.log_level_index, expected);
        REQUIRE_EQ(state.log_retention_index, 2);
        require_same_log_rows(state.logs, original);
        REQUIRE_TRUE(feedback.add_log);
        REQUIRE_TRUE(!feedback.io_drive_changed && feedback.performance_seconds == 0);
    }
}

// Purpose: A non-Settings dropdown must not accidentally publish or mutate a Settings draft.
// Inputs: Compression-format id and an extreme option index. Outputs: Preserved preferences, rows and feedback.
TEST_CASE(settings_dropdown_draft_ignores_other_page_choices) {
    auto state = state_with_retained_rows();
    state.log_level_index = 1;
    const auto original = state.logs;
    DropdownSelectionFeedback feedback;
    apply_settings_dropdown_selection(state, DropdownId::CompressFormat, 999, feedback);
    REQUIRE_EQ(state.log_level_index, 1);
    REQUIRE_EQ(state.log_retention_index, 2);
    require_same_log_rows(state.logs, original);
    REQUIRE_TRUE(!feedback.add_log && !feedback.io_drive_changed);
    REQUIRE_TRUE(feedback.log_message.empty());
    REQUIRE_EQ(feedback.performance_seconds, 0);
}
