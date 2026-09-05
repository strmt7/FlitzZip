#include "app/operation_summary.hpp"
#include "test_util.hpp"

TEST_CASE(operation_summary_selects_current_primary_result_not_auxiliary_checks) {
    const std::vector<superzip::app::HistoryEntry> history{
        {.operation = "Compress"}, {.operation = "Settings"}, {.operation = "Compress"}, {.operation = "Security"}};
    REQUIRE_EQ(superzip::app::operation_summary_index(history, 2, "Compress"), std::optional<int>{2});
    REQUIRE_TRUE(!superzip::app::operation_summary_index(history, 3, "Compress"));
    REQUIRE_TRUE(!superzip::app::operation_summary_index(history, history.size(), "Compress"));
    REQUIRE_TRUE(!superzip::app::operation_summary_index(history, history.size() + 1, "Compress"));
    REQUIRE_TRUE(!superzip::app::operation_summary_index({}, 0, "Extract"));
}

TEST_CASE(operation_summary_selects_last_batch_result_or_failure_without_changing_history) {
    const std::vector<superzip::app::HistoryEntry> history{
        {.operation = "Extract", .success = true},
        {.operation = "Extract", .success = true},
        {.operation = "Extract", .detail = "Operation stopped by the user", .success = false}};
    REQUIRE_EQ(superzip::app::operation_summary_index(history, 0, "Extract"), std::optional<int>{2});
    REQUIRE_EQ(history.back().detail, "Operation stopped by the user");
    REQUIRE_TRUE(!history.back().success);
}

TEST_CASE(operation_summary_clear_and_new_job_discard_stale_selections) {
    superzip::app::OperationSummarySelection summary;
    std::vector<superzip::app::HistoryEntry> history{{.operation = "Compress"}, {.operation = "Settings"}};
    summary.begin(history.size());
    history.clear();
    summary.begin(0);
    history.push_back({.operation = "Compress", .success = false});
    summary.complete(history, "Compress", true);
    REQUIRE_EQ(summary.take(), std::optional<int>{0});
    REQUIRE_TRUE(!summary.take());

    summary.complete(history, "Compress", true);
    history.clear();
    summary.begin(0);
    history.push_back({.operation = "Settings"});
    REQUIRE_TRUE(!summary.take());
    summary.complete(history, "Compress", true);
    REQUIRE_TRUE(!summary.take());

    history.push_back({.operation = "Compress"});
    summary.complete(history, "Compress", true);
    summary.begin(history.size());
    REQUIRE_TRUE(!summary.take());
    summary.complete(history, "Compress", true);
    REQUIRE_TRUE(!summary.take());
    history.push_back({.operation = "Compress"});
    summary.complete(history, "Compress", false);
    REQUIRE_TRUE(!summary.take());
}
