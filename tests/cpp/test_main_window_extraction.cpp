#include "native_window_visual_fixture.hpp"
#include "cpio/cpio_adapter.hpp"
#include "zip/zip_adapter.hpp"

namespace {

using namespace flitzzip_test::native_window;
using namespace std::chrono_literals;

// Purpose: Observe asynchronous Queue metadata before activating a control that depends on every archive's type.
// Inputs: Owned Queue window with one or two visible archive rows. Outputs: Literal Archive text in every type cell.
void require_native_extraction_rows(HWND window, std::size_t count) {
    REQUIRE_TRUE(count > 0U && count <= 2U);
    const auto dpi = GetDpiForWindow(window);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    QueueCapture frame(client.right, client.bottom);
    const int available = std::max(scale(360), static_cast<int>(client.right) - scale(flitzzip::app::kRailWidth) -
                                                   scale(2 * flitzzip::app::kPageInsetX) -
                                                   scale(flitzzip::app::kQueueCheckboxColumnWidth) - scale(22));
    const auto widths = flitzzip::app::fit_table_columns(std::array<double, 4>{276, 100, 96, 468},
                                                         flitzzip::app::kQueueColumnMinimums, available, dpi);
    auto first_cell = first_queue_size_cell(window);
    first_cell.left += widths[1];
    first_cell.right = first_cell.left + widths[2] - scale(16);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame.dc()), PRF_CLIENT);
        bool complete = true;
        for (std::size_t row = 0; row < count; ++row) {
            auto cell = first_cell;
            OffsetRect(&cell, 0, scale(flitzzip::app::kQueueRowHeight) * static_cast<int>(row));
            complete = native_cell_text_matches(frame.dc(), cell, dpi, L"Archive") && complete;
        }
        if (complete) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Archive rows were not ready for native extraction admission");
}

// Purpose: Reach the real extraction consumer through Queue admission, navigation and destination selection.
// Inputs: Owned hidden window, existing archive files and the fixture's isolated destination override.
// Outputs: Extract page form bounds; no worker is started and no private controller state is changed.
flitzzip::app::ExtractLayout prepare_native_extraction(HWND window,
                                                       const std::vector<std::filesystem::path>& archives) {
    for (const auto& archive : archives) {
        ControllerFixture::queue_path(window, archive);
    }
    require_native_extraction_rows(window, archives.size());
    click_native_navigation(window, 2);
    const auto layout = flitzzip::app::make_extract_layout(native_content_rect(window), GetDpiForWindow(window));
    click_native_control(window, layout.destination);
    return layout;
}

// Purpose: Produce a small valid ZIP for controller tests, whose oracle is independently specified plain bytes.
// Inputs: Existing bounded fixture source. Outputs: One archive outside the extraction destination.
std::filesystem::path create_native_extraction_archive(const ControllerFixture& fixture) {
    const auto archive = fixture.root / L"input.zip";
    flitzzip::compress_zip({fixture.input}, archive);
    return archive;
}

// Purpose: Locate the two visible overwrite actions without reading private modal state.
// Inputs: Owned hidden HWND and its public DPI-scaled client/workspace geometry.
// Outputs: Continue and Cancel hit rectangles in visual order.
std::array<RECT, 2> native_overwrite_actions(HWND window) {
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    const auto content = native_content_rect(window);
    const auto dpi = static_cast<int>(GetDpiForWindow(window));
    const auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    const int modal_left = content.left + (client.right - content.left - px(620)) / 2;
    const int modal_top = content.top + (content.bottom - content.top - px(258)) / 2;
    const int modal_right = modal_left + px(620);
    const int button_bottom = modal_top + px(258) - px(22);
    const RECT cancel{modal_right - px(22) - px(110), button_bottom - px(36), modal_right - px(22), button_bottom};
    const RECT allow{cancel.left - px(12) - px(142), cancel.top, cancel.left - px(12), cancel.bottom};
    return {allow, cancel};
}

}  // namespace

// Purpose: Preserve existing files on cancelled confirmation and keep overwrite approval local to one operation.
// Inputs: Real ZIP, pre-existing destination bytes, keyboard and mouse modal actions and a gated native extraction.
// Outputs: No work before approval, exact decoded bytes afterward, unrelated files intact and retry asks again.
TEST_CASE(main_window_extract_confirmation_cancel_retry_preserves_existing_files) {
    ControllerFixture fixture;
    const auto archive = create_native_extraction_archive(fixture);
    const auto target = fixture.output / L"source.txt";
    const auto sentinel = fixture.output / L"unrelated.txt";
    std::ofstream(target, std::ios::binary) << "existing user data";
    std::ofstream(sentinel, std::ios::binary) << "preserve unrelated file";
    GatedLauncher launcher;
    auto entered = launcher.entered.get_future();
    auto completed = launcher.first_completed.get_future();
    HiddenController controller(launcher);
    ReleaseOnExit release(launcher.released);
    const auto window = controller.wait_window();
    const auto layout = prepare_native_extraction(window, {archive});
    const auto settings = read_native_fixture(flitzzip::app::settings_file_path());
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    QueueCapture frame(client.right, client.bottom);
    for (const auto cancel_key : {VK_RETURN, VK_ESCAPE}) {
        click_native_control(window, layout.start);
        click_native_navigation(window, 6);
        HiddenController::send(window, WM_KEYDOWN, cancel_key);
        require_native_navigation_selection(window, frame.dc(), 2);
        REQUIRE_EQ(launcher.attempts.load(), 0U);
        REQUIRE_EQ(read_native_fixture(target), "existing user data");
    }
    click_native_control(window, layout.start);
    HiddenController::send(window, WM_KEYDOWN, VK_LEFT);
    HiddenController::send(window, WM_KEYDOWN, VK_RETURN);
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    REQUIRE_EQ(launcher.attempts.load(), 1U);
    REQUIRE_EQ(read_native_fixture(target), "existing user data");
    release.release();
    REQUIRE_TRUE(completed.wait_for(5s) == std::future_status::ready);
    HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
    REQUIRE_EQ(read_native_fixture(target), "controller ownership fixture\n");
    REQUIRE_EQ(read_native_fixture(sentinel), "preserve unrelated file");
    click_native_control(window, layout.start);
    HiddenController::send(window, WM_KEYDOWN, VK_RETURN);
    REQUIRE_EQ(launcher.attempts.load(), 1U);
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), settings);

    const auto actions = native_overwrite_actions(window);
    click_native_control(window, layout.start);
    click_native_control(window, actions[1]);
    REQUIRE_EQ(launcher.attempts.load(), 1U);
    REQUIRE_EQ(read_native_fixture(target), "controller ownership fixture\n");
    click_native_control(window, layout.start);
    click_native_control(window, actions[0]);
    require_native_observation([&] { return launcher.completed.load() == 2U; },
                               "mouse-approved extraction did not finish");
    HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
    REQUIRE_EQ(read_native_fixture(target), "controller ownership fixture\n");
    REQUIRE_EQ(read_native_fixture(sentinel), "preserve unrelated file");
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), settings);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    REQUIRE_EQ(read_native_fixture(fixture.input), "controller ownership fixture\n");
}

// Purpose: Bind visible Extract controls to the actual overwrite policy without persisting unapplied choices.
// Inputs: One real ZIP, an existing target, keyboard dropdown choice and native toggle clicks on a hidden window.
// Outputs: Exact field/switch states, disabled ZIP encoding override, no prompt and exact target replacement.
TEST_CASE(main_window_extract_controls_drive_overwrite_without_prompt) {
    VisualSession ui;
    const auto archive = create_native_extraction_archive(ui.fixture);
    const auto target = ui.fixture.output / L"source.txt";
    std::ofstream(target, std::ios::binary) << "existing user data";
    const auto layout = prepare_native_extraction(ui.window, {archive});
    const auto settings = read_native_fixture(flitzzip::app::settings_file_path());
    // Purpose: Name the public field whose exact native glyph observation failed.
    // Inputs: Field, expected label and diagnostic name. Outputs: Exact match or attributed failure.
    const auto require_field = [&](const RECT& field, std::wstring_view expected, const char* name,
                                   bool enabled = true) {
        try {
            ui.require_field(field, expected, true, enabled);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string(name) + ": " + error.what());
        }
    };
    require_field(layout.overwrite_policy, L"Ask before overwriting", "default overwrite policy");
    ui.choose_option(layout.overwrite_policy, 1);
    require_field(layout.overwrite_policy, L"Overwrite without asking", "selected overwrite policy");
    require_field(layout.name_encoding, L"UTF-8", "ZIP member name encoding", false);
    click_native_control(ui.window, layout.name_encoding);
    require_field(layout.name_encoding, L"UTF-8", "disabled ZIP encoding override", false);

    const std::array toggles{std::pair{layout.validate_before_publish, true},
                             std::pair{layout.open_destination_after_extract, false}, std::pair{layout.sha, false},
                             std::pair{layout.defender, false}};
    for (const auto& [rect, initial] : toggles) {
        require_native_toggle(ui.window, ui.frame.dc(), rect, initial);
        click_native_control(ui.window, rect);
        require_native_toggle(ui.window, ui.frame.dc(), rect, !initial);
        click_native_control(ui.window, rect);
        require_native_toggle(ui.window, ui.frame.dc(), rect, initial);
    }

    auto entered = ui.launcher.entered.get_future();
    auto completed = ui.launcher.first_completed.get_future();
    ReleaseOnExit release(ui.launcher.released);
    click_native_control(ui.window, layout.start);
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    REQUIRE_EQ(ui.launcher.attempts.load(), 1U);
    REQUIRE_EQ(read_native_fixture(target), "existing user data");
    release.release();
    REQUIRE_TRUE(completed.wait_for(5s) == std::future_status::ready);
    HiddenController::send(ui.window, WM_TIMER, flitzzip::app::kClockTimer);
    REQUIRE_EQ(read_native_fixture(target), "controller ownership fixture\n");
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), settings);
    ui.finish(1, 1);
}

// Purpose: Offer explicit member-name encoding only for an archive format that supports it.
// Inputs: One real CPIO source through Queue admission and native Extract dropdown interaction.
// Outputs: Both supported labels render exactly, and selection returns to UTF-8 without launching work.
TEST_CASE(main_window_extract_name_encoding_enabled_for_cpio) {
    VisualSession ui;
    const auto archive = ui.fixture.root / L"input.cpio";
    flitzzip::compress_cpio({ui.fixture.input}, archive);
    const auto layout = prepare_native_extraction(ui.window, {archive});
    ui.require_field(layout.name_encoding, L"UTF-8");
    ui.choose_option(layout.name_encoding, 1);
    ui.require_field(layout.name_encoding, L"Windows ANSI");
    ui.choose_option(layout.name_encoding, 0);
    ui.require_field(layout.name_encoding, L"UTF-8");
    ui.finish();
}

// Purpose: Route two equally named archives to distinct output directories in the actual GUI worker.
// Inputs: Two ZIPs with equal member names but different literal content; one normal batch admission.
// Outputs: Both independently expected byte sequences, distinct destinations, one worker and no premature output.
TEST_CASE(main_window_extract_batch_preserves_same_named_archive_contents) {
    ControllerFixture fixture;
    const auto second_source = fixture.root / L"second source" / L"source.txt";
    std::filesystem::create_directories(second_source.parent_path());
    std::ofstream(second_source, std::ios::binary) << "different archive payload\n";
    const auto first_archive = fixture.root / L"first" / L"sample.zip";
    const auto second_archive = fixture.root / L"second" / L"sample.zip";
    std::filesystem::create_directories(first_archive.parent_path());
    std::filesystem::create_directories(second_archive.parent_path());
    flitzzip::compress_zip({fixture.input}, first_archive);
    flitzzip::compress_zip({second_source}, second_archive);
    GatedLauncher launcher;
    auto entered = launcher.entered.get_future();
    auto completed = launcher.first_completed.get_future();
    HiddenController controller(launcher);
    ReleaseOnExit release(launcher.released);
    const auto window = controller.wait_window();
    const auto layout = prepare_native_extraction(window, {first_archive, second_archive});
    click_native_control(window, layout.start);
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    REQUIRE_EQ(launcher.attempts.load(), 1U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    release.release();
    REQUIRE_TRUE(completed.wait_for(5s) == std::future_status::ready);
    HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
    REQUIRE_EQ(read_native_fixture(fixture.output / L"sample" / L"source.txt"), "controller ownership fixture\n");
    REQUIRE_EQ(read_native_fixture(fixture.output / L"sample (2)" / L"source.txt"), "different archive payload\n");
    REQUIRE_TRUE(!std::filesystem::exists(fixture.output / L"source.txt"));
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    REQUIRE_EQ(read_native_fixture(fixture.input), "controller ownership fixture\n");
    REQUIRE_EQ(read_native_fixture(second_source), "different archive payload\n");
}

// Purpose: Recover extraction admission after an actual thread-launch refusal without losing the captured inputs.
// Inputs: One valid ZIP, an instance-local launcher refusal and a successful native retry.
// Outputs: No publication on refusal; one completed extraction after retry with exact bytes and joined ownership.
TEST_CASE(main_window_extract_launch_refusal_then_retry_recovers) {
    ControllerFixture fixture;
    const auto archive = create_native_extraction_archive(fixture);
    GatedLauncher launcher;
    launcher.refuse_first = true;
    auto entered = launcher.entered.get_future();
    auto completed = launcher.first_completed.get_future();
    HiddenController controller(launcher);
    ReleaseOnExit release(launcher.released);
    const auto window = controller.wait_window();
    const auto layout = prepare_native_extraction(window, {archive});
    click_native_control(window, layout.start);
    REQUIRE_EQ(launcher.attempts.load(), 1U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    click_native_control(window, layout.start);
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    REQUIRE_EQ(launcher.attempts.load(), 2U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    release.release();
    REQUIRE_TRUE(completed.wait_for(5s) == std::future_status::ready);
    HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
    REQUIRE_EQ(read_native_fixture(fixture.output / L"source.txt"), "controller ownership fixture\n");
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
}
