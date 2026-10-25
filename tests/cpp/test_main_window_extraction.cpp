#include "native_window_test_environment.hpp"
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

}  // namespace

// Purpose: Preserve existing files on cancelled confirmation and keep overwrite approval local to one operation.
// Inputs: Real ZIP, pre-existing destination bytes, keyboard cancel/approval and one gated native extraction.
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
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    REQUIRE_EQ(read_native_fixture(fixture.input), "controller ownership fixture\n");
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
