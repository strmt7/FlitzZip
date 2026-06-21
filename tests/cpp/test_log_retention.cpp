#include "app/log_retention.hpp"
#include "app/log_policy.hpp"
#include "test_util.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace {

struct ProbeLogEntry {
    std::chrono::system_clock::time_point timestamp;
    int id = 0;
};

// Purpose: Build a log-retention probe entry at an age relative to `now`.
// Inputs: `now` is the test clock, `age` is the desired entry age, and `id` identifies the row.
// Outputs: Returns a timestamped row compatible with the shared retention helper.
ProbeLogEntry probe_entry(std::chrono::system_clock::time_point now, std::chrono::hours age, int id) {
    return ProbeLogEntry{.timestamp = now - age, .id = id};
}

}  // namespace

TEST_CASE(log_retention_labels_are_exact_gui_choices) {
    using superzip::app::log_retention_days;
    using superzip::app::log_retention_display_text;
    using superzip::app::log_retention_log_text;

    REQUIRE_EQ(log_retention_display_text(0), std::wstring_view(L"1 week"));
    REQUIRE_EQ(log_retention_display_text(1), std::wstring_view(L"2 weeks"));
    REQUIRE_EQ(log_retention_display_text(2), std::wstring_view(L"1 month"));
    REQUIRE_EQ(log_retention_display_text(-1), std::wstring_view(L"1 week"));
    REQUIRE_EQ(log_retention_display_text(99), std::wstring_view(L"1 week"));
    REQUIRE_EQ(log_retention_log_text(0), std::string_view("1 week"));
    REQUIRE_EQ(log_retention_log_text(1), std::string_view("2 weeks"));
    REQUIRE_EQ(log_retention_log_text(2), std::string_view("1 month"));

    REQUIRE_EQ(log_retention_days(0), 7);
    REQUIRE_EQ(log_retention_days(1), 14);
    REQUIRE_EQ(log_retention_days(2), 30);
    REQUIRE_EQ(log_retention_days(-1), 7);
    REQUIRE_EQ(log_retention_days(99), 7);
}

TEST_CASE(log_level_filter_matrix_matches_settings_labels) {
    using superzip::app::log_level_allows;
    using superzip::app::log_severity_text;
    using superzip::app::LogSeverity;

    REQUIRE_TRUE(log_level_allows(0, LogSeverity::Information));
    REQUIRE_TRUE(log_level_allows(0, LogSeverity::Warning));
    REQUIRE_TRUE(!log_level_allows(0, LogSeverity::Debug));

    REQUIRE_TRUE(!log_level_allows(1, LogSeverity::Information));
    REQUIRE_TRUE(log_level_allows(1, LogSeverity::Warning));
    REQUIRE_TRUE(!log_level_allows(1, LogSeverity::Debug));

    REQUIRE_TRUE(log_level_allows(2, LogSeverity::Information));
    REQUIRE_TRUE(log_level_allows(2, LogSeverity::Warning));
    REQUIRE_TRUE(log_level_allows(2, LogSeverity::Debug));

    REQUIRE_EQ(log_severity_text(LogSeverity::Information), std::string_view("Information"));
    REQUIRE_EQ(log_severity_text(LogSeverity::Warning), std::string_view("Warning"));
    REQUIRE_EQ(log_severity_text(LogSeverity::Debug), std::string_view("Debug"));
}

TEST_CASE(log_retention_prunes_expired_entries_and_caps_count) {
    using superzip::app::prune_log_entries;

    const auto now = std::chrono::system_clock::time_point{} + std::chrono::hours(24 * 40);

    std::vector<ProbeLogEntry> week_entries{
        probe_entry(now, std::chrono::hours(24 * 8), 1),
        probe_entry(now, std::chrono::hours(24 * 7), 2),
        probe_entry(now, std::chrono::hours(24 * 6), 3),
    };
    prune_log_entries(week_entries, now, 0, 128);
    REQUIRE_EQ(week_entries.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(week_entries[0].id, 2);
    REQUIRE_EQ(week_entries[1].id, 3);

    std::vector<ProbeLogEntry> two_week_entries{
        probe_entry(now, std::chrono::hours(24 * 15), 4),
        probe_entry(now, std::chrono::hours(24 * 14), 5),
        probe_entry(now, std::chrono::hours(24 * 13), 6),
    };
    prune_log_entries(two_week_entries, now, 1, 128);
    REQUIRE_EQ(two_week_entries.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(two_week_entries[0].id, 5);
    REQUIRE_EQ(two_week_entries[1].id, 6);

    std::vector<ProbeLogEntry> month_entries{
        probe_entry(now, std::chrono::hours(24 * 31), 7),
        probe_entry(now, std::chrono::hours(24 * 30), 8),
        probe_entry(now, std::chrono::hours(24 * 29), 9),
    };
    prune_log_entries(month_entries, now, 2, 128);
    REQUIRE_EQ(month_entries.size(), static_cast<std::size_t>(2));
    REQUIRE_EQ(month_entries[0].id, 8);
    REQUIRE_EQ(month_entries[1].id, 9);

    std::vector<ProbeLogEntry> capped_entries;
    for (int index = 0; index < 6; ++index) {
        capped_entries.push_back(probe_entry(now, std::chrono::hours(index), index));
    }
    prune_log_entries(capped_entries, now, 2, 3);
    REQUIRE_EQ(capped_entries.size(), static_cast<std::size_t>(3));
    REQUIRE_EQ(capped_entries[0].id, 3);
    REQUIRE_EQ(capped_entries[1].id, 4);
    REQUIRE_EQ(capped_entries[2].id, 5);
}
