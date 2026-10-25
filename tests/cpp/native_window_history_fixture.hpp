#pragma once

#include "native_window_visual_fixture.hpp"
#include "core/windows_handle.hpp"

namespace flitzzip_test::native_window {

inline constexpr std::array<std::wstring_view, 5> kExpectedHistoryOperations{L"All operations", L"Compress", L"Extract",
                                                                             L"Security", L"Settings"};
inline constexpr std::array<std::wstring_view, 3> kExpectedHistoryStatuses{L"All", L"Success", L"Failure"};

// Purpose: Specify public History values independently. Inputs: Literal operation/name and outcome.
// Outputs: Fixture-owned comparison data, separate from the controller's model.
struct ExpectedHistoryRow {
    std::wstring operation;
    std::wstring name;
    bool success;
};

// Purpose: Exercise History through real Settings and small ZIP jobs, retaining an independent row model.
// Inputs: Existing isolated native fixture. Outputs: Literal row/status oracles and owned artifact identities.
class HistorySession final {
  public:
    VisualSession ui;
    std::vector<ExpectedHistoryRow> rows;
    std::vector<std::pair<std::filesystem::path, std::string>> artifacts;
    unsigned jobs = 0;
    int operation = 0;
    int status = 0;
    bool queued = false;

    // Purpose: Release the existing worker gate for ordinary serial jobs. Inputs: Owned session. Outputs: No retry.
    HistorySession() {
        ui.launcher.released.set_value();
    }

    // Purpose: Derive History geometry from the documented public DPI layout.
    // Inputs: Actual client size. Outputs: Filter, table and details bounds without accessing private state.
    flitzzip::app::HistoryLayout layout() const {
        const auto content = native_content_rect(ui.window);
        const RECT area{content.left + ui.px(30), content.top + ui.px(22), content.right - ui.px(30),
                        content.bottom - ui.px(22)};
        flitzzip::app::HistoryLayout result{};
        result.area = area;
        result.operation_filter = RECT{area.left, area.top + ui.px(48), area.left + ui.px(220), area.top + ui.px(92)};
        result.status_filter =
            RECT{area.left + ui.px(238), area.top + ui.px(48), area.left + ui.px(458), area.top + ui.px(92)};
        result.table = RECT{area.left, area.top + ui.px(112), area.right, area.bottom - ui.px(118)};
        result.details = RECT{area.left, result.table.bottom + ui.px(20), area.right, area.bottom};
        return result;
    }

    // Purpose: Reach ordinary History through the native rail. Inputs: Live session. Outputs: Selected public page.
    void open() {
        click_native_navigation(ui.window, 4);
        require_native_navigation_selection(ui.window, ui.frame.dc(), 4);
    }

    // Purpose: Record a real saved Settings result. Inputs: Current draft. Outputs: One independently expected row.
    void settings() {
        click_native_navigation(ui.window, 6);
        const auto form =
            flitzzip::app::make_settings_layout(native_content_rect(ui.window), GetDpiForWindow(ui.window));
        click_native_control(ui.window, form.apply);
        rows.push_back({L"Settings", L"Settings", true});
    }

    // Purpose: Generate success or real Windows publication refusal through the native ZIP consumer.
    // Inputs: Expected result and optional long owned destination. Outputs: Completed worker and retained file bytes.
    void compress(bool success, bool long_path = false) {
        auto destination = ui.fixture.output / (L"job-" + std::to_wstring(jobs));
        if (long_path) {
            while ((destination / L"destination-component").native().size() < 210) {
                destination /= L"destination-component";
            }
        }
        std::filesystem::create_directories(destination);
        const auto archive = destination / L"FlitzZip-output.zip";
        flitzzip::windows_detail::HandleOwner publication_blocker;
        if (!success) {
            std::ofstream(archive, std::ios::binary) << "preserve existing destination\n";
            publication_blocker.reset(CreateFileW(flitzzip::windows_api_path(archive).c_str(), GENERIC_READ,
                                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                                  nullptr));
            REQUIRE_TRUE(publication_blocker.get() != INVALID_HANDLE_VALUE);
        }
        if (!queued) {
            click_native_navigation(ui.window, 0);
            ControllerFixture::queue_path(ui.window, ui.fixture.input);
            queued = true;
        }
        ControllerFixture::show_compression(ui.window);
        flitzzip_test::ScopedEnvironment selected(L"FLITZZIP_GUI_SMOKE_DESTINATION");
        selected.set(destination.c_str());
        const auto form = native_compression_layout(ui.window);
        click_native_control(ui.window, form.destination);
        click_native_control(ui.window, form.start);
        ++jobs;
        require_native_observation([&] { return ui.launcher.completed.load() == jobs; },
                                   "History fixture archive worker did not complete");
        const auto bytes = read_native_fixture(archive);
        if (success) {
            REQUIRE_TRUE(bytes.size() > 30 && bytes.substr(0, 4) == std::string("PK\003\004", 4));
        } else {
            REQUIRE_EQ(bytes, "preserve existing destination\n");
        }
        artifacts.emplace_back(archive, bytes);
        rows.push_back({L"Compress", success ? L"FlitzZip-output.zip" : L"Compress failure", success});
    }

    // Purpose: Supply a nonperiodic mixture of public row types and real results.
    // Inputs: Bounded row count. Outputs: Settings successes and compression successes/failures in known order.
    void seed(int count) {
        REQUIRE_TRUE(count > 0 && count <= 32);
        for (int index = 0; index < count; ++index) {
            if (index % 5 == 1 || index % 7 == 3) {
                compress(index % 2 == 0);
            } else {
                settings();
            }
        }
        open();
    }

    // Purpose: Model public filter conjunction independently of the implementation.
    // Inputs: Known literal rows and requested filters. Outputs: Expected original row indices in insertion order.
    std::vector<std::size_t> visible_rows() const {
        std::vector<std::size_t> result;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const auto& row = rows[index];
            const bool kind = operation == 0 || row.operation == kExpectedHistoryOperations.at(operation);
            if (kind && (status == 0 || (status == 1 && row.success) || (status == 2 && !row.success))) {
                result.push_back(index);
            }
        }
        return result;
    }

    // Purpose: Commit both public filters and prove their literal values.
    // Inputs: Ordinary operation/status indices. Outputs: Filtered expected model and acknowledged fields.
    void filter(int requested_operation, int requested_status) {
        REQUIRE_TRUE(requested_operation >= 0 && requested_operation < 5);
        REQUIRE_TRUE(requested_status >= 0 && requested_status < 3);
        const auto form = layout();
        ui.choose_option(form.operation_filter, requested_operation);
        ui.choose_option(form.status_filter, requested_status);
        operation = requested_operation;
        status = requested_status;
        ui.require_field(form.operation_filter, kExpectedHistoryOperations.at(operation));
        ui.require_field(form.status_filter, kExpectedHistoryStatuses.at(status));
    }

    // Purpose: Count complete public rows and locate one visible row.
    // Inputs: Minus one for the fixed header. Outputs: Native row rectangle inside the actual table.
    RECT row_rect(int row) const {
        const auto table = layout().table;
        const int top = row < 0 ? table.top : table.top + ui.px(36) + row * ui.px(34);
        return RECT{table.left, top, table.right, top + ui.px(row < 0 ? 36 : 34)};
    }

    // Purpose: Derive ordinary default column rectangles through the shared public geometry primitive.
    // Inputs: Visible row. Outputs: Independent cells; overflow reserves the documented scrollbar gutter.
    std::array<RECT, 5> cells(int row) const {
        auto bounds = row_rect(row);
        const auto table = layout().table;
        const int count = (table.bottom - table.top - ui.px(36)) / ui.px(34);
        if (static_cast<int>(visible_rows().size()) > count) {
            bounds.right -= ui.px(20);
        }
        const int available = std::max(ui.px(360), static_cast<int>(bounds.right - bounds.left) - ui.px(30));
        const auto widths = flitzzip::app::fit_table_columns(std::array<double, 5>{112, 100, 300, 410, 102},
                                                             flitzzip::app::kHistoryColumnMinimums, available,
                                                             GetDpiForWindow(ui.window));
        std::array<RECT, 5> result{};
        int left = bounds.left + ui.px(14);
        for (std::size_t index = 0; index < result.size(); ++index) {
            const int right = index == 4 ? bounds.right - ui.px(16) : left + widths[index];
            // Symmetric insets retain glyph centring while excluding focus outlines and table separators.
            result[index] = RECT{left + ui.px(8), bounds.top + ui.px(4), right - ui.px(8), bounds.bottom - ui.px(4)};
            left = right;
        }
        return result;
    }

    // Purpose: Recognize row identity and outcome through independently expected literal glyphs.
    // Inputs: Visible position and original expected row. Outputs: Exact operation, archive-name and status match.
    void require_row(int position, std::size_t original) {
        const auto& expected = rows.at(original);
        const auto columns = cells(position);
        require_native_observation(
            [&] {
                ui.paint();
                const auto dpi = GetDpiForWindow(ui.window);
                return native_cell_text_matches(ui.frame.dc(), columns[1], dpi, expected.operation,
                                                flitzzip::app::kText) &&
                       native_cell_text_matches(ui.frame.dc(), columns[2], dpi, expected.name) &&
                       native_cell_text_matches(ui.frame.dc(), columns[4], dpi,
                                                expected.success ? L"Success" : L"Failure",
                                                expected.success ? flitzzip::app::kOk : flitzzip::app::kDanger);
            },
            "History row differs from the independently expected operation or result");
    }

    // Purpose: Distinguish empty session, empty filter and absent selection through actual visible text.
    // Inputs: Literal expected placeholder and target panel. Outputs: Exact native wrapped glyph match.
    void require_placeholder(std::wstring_view expected, bool details = false) {
        const auto form = layout();
        const RECT text = details ? RECT{form.details.left + ui.px(18), form.details.top + ui.px(10),
                                         form.details.right - ui.px(18), form.details.bottom - ui.px(1)}
                                  : RECT{form.table.left + ui.px(18), form.table.top + ui.px(46),
                                         form.table.right - ui.px(18), form.table.top + ui.px(88)};
        require_native_observation(
            [&] {
                ui.paint();
                return native_cell_text_matches(ui.frame.dc(), text, GetDpiForWindow(ui.window), expected,
                                                flitzzip::app::kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
            },
            "History placeholder does not describe the actual session or filter state");
    }

    // Purpose: Check selection without reading or changing the controller model.
    // Inputs: Visible selected row. Outputs: Exact native selection fill at its text-free left edge.
    void require_selected(int position) {
        const auto bounds = row_rect(position);
        // Selection survives pointer leave; sample inside both the focus outline and the text-free row padding.
        HiddenController::send(ui.window, WM_MOUSELEAVE);
        require_native_observation(
            [&] {
                ui.paint();
                return GetPixel(ui.frame.dc(), bounds.left + ui.px(10), bounds.top + ui.px(6)) ==
                       flitzzip::app::kPanel3;
            },
            "History selection moved to an unexpected visible row");
        const auto table = layout().table;
        const int count = std::min(static_cast<int>(visible_rows().size()),
                                   static_cast<int>((table.bottom - table.top - ui.px(36)) / ui.px(34)));
        for (int index = 0; index < count; ++index) {
            if (index != position) {
                const auto other = row_rect(index);
                REQUIRE_TRUE(GetPixel(ui.frame.dc(), other.left + ui.px(10), other.top + ui.px(6)) !=
                             flitzzip::app::kPanel3);
            }
        }
    }

    // Purpose: Clear History through its actual keyboard-focus command, independent of dynamic button width.
    // Inputs: History page. Outputs: Clear action, empty model and retained filter choices.
    void clear() {
        ui.key(VK_HOME);
        for (int index = 0; index < 10; ++index) {
            ui.key(VK_RIGHT);
        }
        ui.key(VK_RETURN);
        rows.clear();
        require_placeholder(L"No completed operations in this session yet.");
    }

    // Purpose: Complete the native lifecycle and prove presentation did not change archive or source bytes.
    // Inputs: Completed jobs and retained artifacts. Outputs: Joined UI, stable file identities and bounded footprint.
    void finish() {
        HiddenController::send(ui.window, WM_CLOSE);
        ui.controller.require_closed();
        REQUIRE_EQ(ui.launcher.attempts.load(), jobs);
        REQUIRE_EQ(ui.launcher.completed.load(), jobs);
        REQUIRE_EQ(read_native_fixture(ui.fixture.input), "controller ownership fixture\n");
        for (const auto& [path, bytes] : artifacts) {
            REQUIRE_EQ(read_native_fixture(path), bytes);
        }
        std::uintmax_t footprint = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(ui.fixture.root)) {
            if (entry.is_regular_file()) {
                footprint += entry.file_size();
            }
        }
        REQUIRE_TRUE(footprint <= 64ULL * 1024ULL * 1024ULL);
    }
};

// Purpose: Wait for visual convergence without retrying input or relying on one elapsed sleep.
// Inputs: Owned region. Outputs: Exact pixels stable across a 160ms observation interval within the shared deadline.
inline std::vector<COLORREF> settled_history_pixels(HistorySession& s, const RECT& area) {
    auto previous = s.ui.pixels(area);
    auto stable = std::chrono::steady_clock::now();
    require_native_observation(
        [&] {
            auto next = s.ui.pixels(area);
            if (next != previous) {
                previous = std::move(next);
                stable = std::chrono::steady_clock::now();
            }
            return std::chrono::steady_clock::now() - stable >= 160ms;
        },
        "History animation did not converge to stable native pixels");
    return previous;
}

// Purpose: Provide actual details text bounds, excluding the independent scrollbar and frame.
// Inputs: Public panel geometry. Outputs: Native text region suitable for identity and scroll observations.
inline RECT history_detail_region(const HistorySession& s) {
    const auto panel = s.layout().details;
    return RECT{panel.left + s.ui.px(18), panel.top + s.ui.px(10), panel.right - s.ui.px(28),
                panel.bottom - s.ui.px(8)};
}

}  // namespace flitzzip_test::native_window
