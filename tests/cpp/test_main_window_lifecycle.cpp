#include "native_window_test_environment.hpp"
#include "app/main_window.hpp"
#include "app/main_window_support.hpp"
#include "app/form_layout.hpp"
#include "app/history_publication.hpp"
#include "app/gdi_dc_state.hpp"
#include "app/table_columns.hpp"
#include "test_environment.hpp"
#include "zip/zip_adapter.hpp"

#include <fstream>
#include <future>
#include <memory>
#include <type_traits>
#include <system_error>
#include <shlobj.h>

namespace {

using namespace std::chrono_literals;
using namespace flitzzip_test::native_window;

// Purpose: Refuse one folder worker before native acquisition, then retain real start/cleanup evidence.
// Inputs: Owned task; this launcher outlives its controller. Outputs: One refusal or actual native owning thread.
class FolderAdmissionLauncher final : public flitzzip::app::JobThreadLauncher {
  public:
    mutable std::atomic_uint attempts{0};
    mutable std::promise<void> refused;
    mutable std::promise<void> entered;
    mutable std::promise<void> finished;
    mutable std::atomic_bool measured{false};

    // Purpose: Control external admission only. Inputs: Owned production task. Outputs: Real thread or refusal.
    std::thread launch(std::function<void()> task) const override {
        if (attempts.fetch_add(1) == 0U) {
            refused.set_value();
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return JobThreadLauncher::launch([this, task = std::move(task)] {
            const bool observe = !measured.exchange(true);
            if (observe) {
                entered.set_value();
            }
            task();
            if (observe) {
                finished.set_value();
            }
        });
    }
};

}  // namespace

namespace {

// Purpose: Exercise the real controller close boundary with controlled work and independent filesystem assertions.
// Inputs: Native close message and optional initial admission refusal. Outputs: Responsive cancellation and cleanup.
void exercise_active_close(UINT close_message, WPARAM close_argument, bool refuse_first) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    launcher.refuse_first = refuse_first;
    auto entered = launcher.entered.get_future();
    HiddenController controller(launcher);
    ReleaseOnExit release(launcher.released);
    const auto window = controller.wait_window();
    fixture.prepare_compression(window);
    fixture.start(window);
    if (refuse_first) {
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        fixture.start(window);
    }
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    REQUIRE_EQ(launcher.attempts.load(), refuse_first ? 2U : 1U);
    HiddenController::send(window, close_message, close_argument);
    HiddenController::send(window, WM_NULL);
    REQUIRE_TRUE(IsWindow(window) != FALSE);
    REQUIRE_TRUE(IsWindowVisible(window) == FALSE);
    release.release();
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    std::ifstream input(fixture.input, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE_EQ(original, std::string("controller ownership fixture\n"));
}

}  // namespace

// Purpose: Prove native startup refusal is consumed and a subsequent actual controller job can close responsively.
// Inputs: First-launch refusal and second-launch held before real ZIP execution. Outputs: Reuse and clean close.
TEST_CASE(main_window_lifecycle_failed_admission_then_responsive_active_close) {
    exercise_active_close(WM_CLOSE, 0, true);
}

// Purpose: Prevent the automatic smoke timer from bypassing the same orderly worker close boundary.
// Inputs: Actual native timer dispatch while a worker is held. Outputs: Responsive cancellation and cleanup.
TEST_CASE(main_window_lifecycle_auto_close_timer_preserves_message_pump) {
    exercise_active_close(WM_TIMER, flitzzip::app::kSmokeAutoCloseTimer, false);
}

// Purpose: Keep ordinary idle closure immediate without starting unnecessary archive work.
// Inputs: Hidden initialized native window and a close request. Outputs: Clean exit with no thread admission.
TEST_CASE(main_window_lifecycle_idle_close_does_not_launch_work) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    HiddenController controller(launcher);
    const auto window = controller.wait_window();
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(launcher.attempts.load(), 0U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
}

// Purpose: Keep automatic failure summaries renderable when an exception contains non-UTF-8 bytes.
// Inputs: Actual History formatter with valid Unicode detail, then opaque narrow exception bytes.
// Outputs: Normal Unicode unchanged; malformed detail displayed losslessly with its explicit label and failure status.
TEST_CASE(main_window_history_detail_preserves_unicode_and_opaque_error_bytes) {
    flitzzip::app::HistoryEntry entry;
    entry.operation = "Verify";
    entry.archive_name = "queued.zip";
    entry.archive_path = "queued.zip";
    entry.success = false;
    entry.detail = "Archive error: \xCE\xB4";
    const auto valid = flitzzip::app::history_detail_text(entry);
    REQUIRE_TRUE(valid.find(L"Operation: Verify") != std::wstring::npos);
    REQUIRE_TRUE(valid.find(L"Status: Failure") != std::wstring::npos);
    REQUIRE_TRUE(valid.ends_with(L"Details: Archive error: \u03B4"));
    entry.detail = "Archive error: ";
    entry.detail.push_back(static_cast<char>(0xFF));
    entry.detail += "\\xFF";
    const auto malformed = flitzzip::app::history_detail_text(entry);
    REQUIRE_TRUE(malformed.ends_with(L"Details: [invalid UTF-8 bytes] Archive error: \\xFF\\x5CxFF"));
    REQUIRE_TRUE(malformed.find(L"Status: Failure") != std::wstring::npos);
}

// Purpose: Preserve caller-owned GDI state across both standard native client-print messages.
// Inputs: Actual hidden controller and an owned memory DC with independently selected font/color/background state.
// Outputs: Rendering leaves the caller's state intact, so the caller cannot retain UI-owned fonts accidentally.
TEST_CASE(main_window_print_preserves_borrowed_device_context_state) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    HiddenController controller(launcher);
    const auto window = controller.wait_window();
    using OwnedMemoryDc = std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)>;
    // This owner follows controller construction, so a failed assertion releases the DC before fonts are destroyed.
    OwnedMemoryDc target(CreateCompatibleDC(nullptr), DeleteDC);
    REQUIRE_TRUE(target != nullptr);
    const auto original_font = GetCurrentObject(target.get(), OBJ_FONT);
    REQUIRE_TRUE(original_font != nullptr);
    const COLORREF original_color = RGB(5, 17, 29);
    REQUIRE_TRUE(SetTextColor(target.get(), original_color) != CLR_INVALID);
    REQUIRE_TRUE(SetBkMode(target.get(), OPAQUE) != 0);
    for (const UINT message : {WM_PRINTCLIENT, WM_PRINT}) {
        HiddenController::send(window, message, reinterpret_cast<WPARAM>(target.get()), PRF_CLIENT);
        REQUIRE_TRUE(GetCurrentObject(target.get(), OBJ_FONT) == original_font);
        REQUIRE_EQ(GetTextColor(target.get()), original_color);
        REQUIRE_EQ(GetBkMode(target.get()), OPAQUE);
    }
    target.reset();
    HiddenController::send(window, WM_PRINTCLIENT, 0, PRF_CLIENT);
    HiddenController::send(window, WM_NULL);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(launcher.attempts.load(), 0U);
}

// Purpose: Contain refused folder-thread admission and keep failure final until the actual Queue lifetime changes.
// Inputs: Real hidden Queue, a once-refusing launcher, memory-only printed frames and files of 17 and 23 bytes.
// Outputs: Independently rendered -- persists without retries; re-addition runs the real worker, displays 40 B,
// preserves caller bytes and joins actual native work before releasing the launcher.
TEST_CASE(main_window_folder_admission_failure_is_unavailable_until_readd_and_joins_real_work) {
    ControllerFixture fixture;
    const auto folder = fixture.root / L"queued-folder";
    REQUIRE_TRUE(std::filesystem::create_directory(folder));
    {
        std::ofstream(folder / "a.txt", std::ios::binary) << std::string(17, 'a');
        std::ofstream(folder / "b.txt", std::ios::binary) << std::string(23, 'b');
    }
    FolderAdmissionLauncher launcher;
    auto refused = launcher.refused.get_future();
    auto entered = launcher.entered.get_future();
    auto finished = launcher.finished.get_future();
    HWND window = nullptr;
    {
        HiddenController controller(launcher);
        window = controller.wait_window();
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        QueueCapture frame(client.right, client.bottom);
        const auto dpi = GetDpiForWindow(window);
        const auto cell = first_queue_size_cell(window);
        ControllerFixture::queue_path(window, folder);
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (refused.wait_for(0s) != std::future_status::ready) {
            REQUIRE_TRUE(std::chrono::steady_clock::now() < deadline);
            HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame.dc()), PRF_CLIENT);
        }
        for (unsigned repaint = 0; repaint < 3U; ++repaint) {
            HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame.dc()), PRF_CLIENT);
            REQUIRE_TRUE(native_cell_text_matches(frame.dc(), cell, dpi, L"--"));
            REQUIRE_TRUE(!native_cell_text_matches(frame.dc(), cell, dpi, L"..."));
            HiddenController::send(window, WM_NULL);
        }
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        REQUIRE_TRUE(entered.wait_for(0s) == std::future_status::timeout);
        ControllerFixture::clear_queue(window);
        ControllerFixture::queue_path(window, folder);
        bool completed_size = false;
        deadline = std::chrono::steady_clock::now() + 5s;
        do {
            HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame.dc()), PRF_CLIENT);
            completed_size = native_cell_text_matches(frame.dc(), cell, dpi, L"40 B");
        } while (!completed_size && std::chrono::steady_clock::now() < deadline);
        REQUIRE_TRUE(completed_size);
        REQUIRE_TRUE(entered.wait_for(0s) == std::future_status::ready);
        REQUIRE_EQ(launcher.attempts.load(), 2U);
        HiddenController::send(window, WM_NULL);
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
    }
    REQUIRE_TRUE(finished.wait_for(0s) == std::future_status::ready);
    REQUIRE_TRUE(IsWindow(window) == FALSE);
    for (const auto& [name, expected] :
         {std::pair{"a.txt", std::string(17, 'a')}, std::pair{"b.txt", std::string(23, 'b')}}) {
        std::ifstream input(folder / name, std::ios::binary);
        REQUIRE_EQ(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()), expected);
    }
    REQUIRE_EQ(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{}), 2);
}

// Purpose: Bind the selected result and its reset request to one synchronized publication, including coalescing.
// Inputs: Real worker publication under a shared mutex, interleaved with UI snapshot consumption.
// Outputs: Exact Unicode detail survives; requests occur once per publication batch and are not lost after a frame.
TEST_CASE(main_window_history_publication_preserves_result_and_reset_order) {
    using namespace flitzzip::app;
    UiState shared;
    std::mutex mutex;
    const std::string detail = "Details: \xCE\xB4";
    const auto publish = [&] {
        std::lock_guard lock(mutex);
        publish_history_result(shared, HistoryEntry{.operation = "Extract", .detail = detail, .success = true});
    };
    auto worker = std::async(std::launch::async, [&] {
        publish();
        publish();
    });
    worker.get();
    UiState first;
    {
        std::lock_guard lock(mutex);
        first = consume_ui_snapshot(shared);
        REQUIRE_TRUE(!shared.history_details_reset_pending);
        REQUIRE_TRUE(!consume_ui_snapshot(shared).history_details_reset_pending);
    }
    REQUIRE_TRUE(first.history_details_reset_pending);
    REQUIRE_EQ(first.history.size(), std::size_t{2});
    REQUIRE_EQ(first.selected_history_index, 1);
    REQUIRE_EQ(first.history.back().detail, detail);
    // A later publication must survive the previous frame's consumption; its snapshot owns its result bytes.
    auto next = std::async(std::launch::async, publish);
    next.get();
    std::lock_guard lock(mutex);
    const auto second = consume_ui_snapshot(shared);
    REQUIRE_TRUE(second.history_details_reset_pending);
    REQUIRE_EQ(second.selected_history_index, 2);
    REQUIRE_EQ(first.history.size(), std::size_t{2});
    REQUIRE_TRUE(!consume_ui_snapshot(shared).history_details_reset_pending);
}

namespace {

// Purpose: Exercise successful and failed real worker publication while native animation and rendering remain active.
// Inputs: Owned tiny ZIP source, optionally removed after admission to inject actual input-open failure.
// Outputs: Correct terminal log and archive existence; the window stays responsive and closes after actual cleanup.
void exercise_history_worker_publication(bool remove_source, bool reject_empty_first = false) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    auto entered = launcher.entered.get_future();
    HiddenController controller(launcher);
    ReleaseOnExit release(launcher.released);
    const auto window = controller.wait_window();
    if (reject_empty_first) {
        fixture.show_compression(window);
        fixture.start(window);
        HiddenController::send(window, WM_NULL);
        REQUIRE_EQ(launcher.attempts.load(), 0U);
        REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
        fixture.show_queue(window);
    }
    fixture.prepare_compression(window);
    fixture.start(window);
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    if (remove_source) {
        REQUIRE_TRUE(std::filesystem::remove(fixture.input));
    }
    const auto log = flitzzip::app::log_file_path();
    const std::string expected = remove_source ? "Compress: failed" : "Compress: completed";
    release.release();
    std::string observed;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        HiddenController::send(window, WM_TIMER, flitzzip::app::kAnimationTimer);
        std::ifstream input(log, std::ios::binary);
        observed.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (observed.find(expected) != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE_TRUE(observed.find(expected) != std::string::npos);
    REQUIRE_TRUE(observed.find("History presentation state requires") == std::string::npos);
    REQUIRE_TRUE(observed.find(remove_source ? "Compress: completed" : "Compress: failed") == std::string::npos);
    using OwnedMemoryDc = std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)>;
    OwnedMemoryDc target(CreateCompatibleDC(nullptr), DeleteDC);
    REQUIRE_TRUE(target != nullptr);
    HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(target.get()), PRF_CLIENT);
    HiddenController::send(window, WM_NULL);
    target.reset();
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    const auto archive = fixture.output / L"FlitzZip-output.zip";
    REQUIRE_EQ(std::filesystem::exists(archive), !remove_source);
    if (!remove_source) {
        REQUIRE_TRUE(std::filesystem::file_size(archive) > 0);
        std::ifstream source(fixture.input, std::ios::binary);
        const std::string original((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        REQUIRE_EQ(original, std::string("controller ownership fixture\n"));
    }
}

}  // namespace

// Purpose: Prevent successful worker results from accessing window-owned scroll or animation fields.
// Inputs: Actual hidden controller compressing an owned tiny ZIP. Outputs: Successful publication and UI consumption.
TEST_CASE(main_window_history_successful_worker_publishes_on_window_thread) {
    exercise_history_worker_publication(false);
}

// Purpose: Keep the sibling failure-result path on the same synchronized publication boundary.
// Inputs: Actual missing-source failure after admission. Outputs: Failed result, responsive UI and no output archive.
TEST_CASE(main_window_history_failed_worker_publishes_on_window_thread) {
    exercise_history_worker_publication(true);
}

// Purpose: Refuse an empty source set without acquiring worker admission, then prove actual valid reuse.
// Inputs: Empty Compress page followed by one owned source. Outputs: No empty output, then a completed ZIP job.
TEST_CASE(main_window_lifecycle_empty_compression_refuses_work_and_allows_reuse) {
    exercise_history_worker_publication(false, true);
}

// Purpose: Separate Stop from window close and verify recovery only after the real task has returned.
// Inputs: Actual Stop while ZIP work is held before execution, followed by a fresh compression request.
// Outputs: Responsive cancellation, no cancelled output, unchanged source and a successful second admission.
TEST_CASE(main_window_lifecycle_stop_then_success_preserves_input_and_admission) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    auto entered = launcher.entered.get_future();
    auto first_completed = launcher.first_completed.get_future();
    HiddenController controller(launcher);
    ReleaseOnExit release(launcher.released);
    const auto window = controller.wait_window();
    fixture.prepare_compression(window);
    fixture.start(window);
    REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
    fixture.stop(window);
    HiddenController::send(window, WM_NULL);
    REQUIRE_TRUE(IsWindow(window) != FALSE);
    REQUIRE_EQ(launcher.attempts.load(), 1U);
    release.release();
    REQUIRE_TRUE(first_completed.wait_for(5s) == std::future_status::ready);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    // Consume the actual worker completion and join before observing subsequent native admission.
    HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
    fixture.start(window);
    REQUIRE_EQ(launcher.attempts.load(), 2U);
    const auto log = flitzzip::app::log_file_path();
    std::string observed;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        std::ifstream input(log, std::ios::binary);
        observed.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (observed.find("Compress: completed") != std::string::npos) {
            break;
        }
        HiddenController::send(window, WM_NULL);
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE_TRUE(observed.find("Compress: stopped") != std::string::npos);
    REQUIRE_TRUE(observed.find("Compress: completed") != std::string::npos);
    REQUIRE_TRUE(observed.find("Compress: failed") == std::string::npos);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    REQUIRE_TRUE(std::filesystem::file_size(fixture.output / L"FlitzZip-output.zip") > 0);
    std::ifstream input(fixture.input, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE_EQ(original, std::string("controller ownership fixture\n"));
}

namespace {

// Purpose: Consume a fixed independently encoded valid archive instead of trusting a product writer/reader round trip.
// Inputs: Owned destination. Outputs: Stored ZIP member security.txt with literal security admission fixture plus LF.
std::string write_security_admission_zip(const std::filesystem::path& path) {
    // Python standard-library zipfile, ZIP_STORED, fixed DOS timestamp 2020-01-02 03:04:06.
    constexpr unsigned char bytes[]{
        0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0x18, 0x22, 0x50, 0xe6, 0x44, 0x0c,
        0x84, 0x1b, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x73, 0x65, 0x63, 0x75,
        0x72, 0x69, 0x74, 0x79, 0x2e, 0x74, 0x78, 0x74, 0x73, 0x65, 0x63, 0x75, 0x72, 0x69, 0x74, 0x79, 0x20,
        0x61, 0x64, 0x6d, 0x69, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x20, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65,
        0x0a, 0x50, 0x4b, 0x01, 0x02, 0x14, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0x18, 0x22, 0x50,
        0xe6, 0x44, 0x0c, 0x84, 0x1b, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x73, 0x65, 0x63, 0x75,
        0x72, 0x69, 0x74, 0x79, 0x2e, 0x74, 0x78, 0x74, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x3a, 0x00, 0x00, 0x00, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const std::string expected(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    std::ofstream output(path, std::ios::binary);
    output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    output.close();
    REQUIRE_TRUE(output.good());
    return expected;
}

// Purpose: Wait for actual asynchronous metadata before asking Security to admit an archive.
// Inputs: Hidden Queue with one regular archive and owned frame. Outputs: Independently rendered Archive type.
void require_archive_queue_ready(HWND window, HDC frame) {
    const auto dpi = GetDpiForWindow(window);
    auto cell = first_queue_size_cell(window);
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int available = std::max(scale(360), static_cast<int>(client.right) - scale(flitzzip::app::kRailWidth) -
                                                   scale(2 * flitzzip::app::kPageInsetX) -
                                                   scale(flitzzip::app::kQueueCheckboxColumnWidth) - scale(22));
    const auto widths = flitzzip::app::fit_table_columns(std::array<double, 4>{276, 100, 96, 468},
                                                         flitzzip::app::kQueueColumnMinimums, available, dpi);
    cell.left += widths[1];
    cell.right = cell.left + widths[2] - scale(16);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame), PRF_CLIENT);
        if (native_cell_text_matches(frame, cell, dpi, L"Archive")) {
            return;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("archive metadata was not rendered before Security admission");
}

// Purpose: Recognize actual Security check status cells with literal native text/color references.
// Inputs: Owned frame and HWND; expected state for the three selected checks. Outputs: Exact RGB assertions.
void require_security_statuses(HWND window, HDC frame, std::wstring_view expected, COLORREF color) {
    const auto dpi = GetDpiForWindow(window);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int left = scale(flitzzip::app::kRailWidth) + scale(flitzzip::app::kPageInsetX);
    const int top = scale(flitzzip::app::kTopBar) + scale(flitzzip::app::kPageInsetY) + scale(54) + scale(36);
    HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame), PRF_CLIENT);
    for (int row = 0; row < 4; ++row) {
        const RECT cell{left + scale(230), top + row * scale(38), left + scale(414), top + (row + 1) * scale(38)};
        REQUIRE_TRUE(native_cell_text_matches(frame, cell, dpi, row < 3 ? expected : L"Not selected",
                                              row < 3 ? color : flitzzip::app::kMuted));
    }
}

// Purpose: Dispatch only public native mouse input to Security's real navigation and Verify hit regions.
// Inputs: Hidden HWND and whether navigation precedes the command. Outputs: One actual Verify button activation.
void click_security_verify(HWND window, bool navigate) {
    const auto dpi = GetDpiForWindow(window);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    auto click = [window](POINT point) {
        const auto position = MAKELPARAM(point.x, point.y);
        HiddenController::send(window, WM_LBUTTONDOWN, MK_LBUTTON, position);
        HiddenController::send(window, WM_LBUTTONUP, 0, position);
    };
    if (navigate) {
        click(POINT{scale(40), scale(flitzzip::app::kTopBar) + scale(10) + 3 * scale(63) + scale(31)});
    }
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    click(POINT{client.right - scale(flitzzip::app::kPageInsetX) - scale(55),
                client.bottom - scale(flitzzip::app::kStatusBar) - scale(flitzzip::app::kPageInsetY) - scale(36)});
}

}  // namespace

// Purpose: Prevent phantom running checks after real native thread refusal, then prove actual review and ownership
// reuse. Inputs: Independently encoded stored ZIP, SHA-256 selected, Defender unselected and existing once-refusing
// launcher. Outputs: Incomplete without work, Running during held retry, then Passed; source unchanged and all native
// owners joined.
TEST_CASE(main_window_security_admission_failure_is_incomplete_then_real_retry_passes_and_joins) {
    ControllerFixture fixture(true);
    const auto archive = fixture.root / L"security.zip";
    const auto expected_bytes = write_security_admission_zip(archive);
    GatedLauncher launcher;
    launcher.refuse_first = true;
    auto entered = launcher.entered.get_future();
    auto completed = launcher.first_completed.get_future();
    HWND window = nullptr;
    {
        HiddenController controller(launcher);
        ReleaseOnExit release(launcher.released);
        window = controller.wait_window();
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        QueueCapture frame(client.right, client.bottom);
        ControllerFixture::queue_path(window, archive);
        require_archive_queue_ready(window, frame.dc());
        click_security_verify(window, true);
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        REQUIRE_TRUE(entered.wait_for(0s) == std::future_status::timeout);
        for (unsigned repaint = 0; repaint < 3U; ++repaint) {
            require_security_statuses(window, frame.dc(), L"Incomplete", flitzzip::app::kWarn);
        }
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        click_security_verify(window, false);
        REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
        REQUIRE_EQ(launcher.attempts.load(), 2U);
        require_security_statuses(window, frame.dc(), L"Running", flitzzip::app::kInfo);
        release.release();
        REQUIRE_TRUE(completed.wait_for(5s) == std::future_status::ready);
        HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
        require_security_statuses(window, frame.dc(), L"Passed", flitzzip::app::kOk);
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
    }
    REQUIRE_TRUE(IsWindow(window) == FALSE);
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    std::ifstream input(archive, std::ios::binary);
    REQUIRE_EQ(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()), expected_bytes);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    std::ifstream log(flitzzip::app::log_file_path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    const auto completion = text.find("Security: completed");
    REQUIRE_TRUE(completion != std::string::npos);
    REQUIRE_TRUE(text.find("Security: completed", completion + 1) == std::string::npos);
    REQUIRE_TRUE(text.find("Security: failed") == std::string::npos);
}

// Purpose: Preserve the running job's page and prove normal native navigation recovers after actual completion.
// Inputs: Real CPU ZIP work held by the existing instance-local gate, eight native rail selections and literal input.
// Outputs: No premature archive, one real worker, locked selection, joined completion and exact recovered bytes.
TEST_CASE(main_window_active_job_blocks_navigation_then_idle_reuses_native_commands) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    auto entered = launcher.entered.get_future();
    auto completed = launcher.first_completed.get_future();
    HWND window = nullptr;
    {
        HiddenController controller(launcher);
        ReleaseOnExit release(launcher.released);
        window = controller.wait_window();
        fixture.prepare_compression(window);
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        QueueCapture frame(client.right, client.bottom);
        require_native_navigation_selection(window, frame.dc(), 1);
        fixture.start(window);
        REQUIRE_TRUE(entered.wait_for(3s) == std::future_status::ready);
        for (int page = 0; page < 8; ++page) {
            click_native_navigation(window, page);
            require_native_navigation_selection(window, frame.dc(), 1);
        }
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        REQUIRE_TRUE(completed.wait_for(0s) == std::future_status::timeout);
        REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
        release.release();
        REQUIRE_TRUE(completed.wait_for(5s) == std::future_status::ready);
        HiddenController::send(window, WM_TIMER, flitzzip::app::kClockTimer);
        for (int page = 0; page < 8; ++page) {
            click_native_navigation(window, page);
            require_native_navigation_selection(window, frame.dc(), page);
        }
        REQUIRE_EQ(launcher.attempts.load(), 1U);
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
    }
    REQUIRE_TRUE(IsWindow(window) == FALSE);
    REQUIRE_TRUE(!launcher.release_timed_out.load());
    const auto archive = fixture.output / L"FlitzZip-output.zip";
    const auto restored = fixture.root / L"restored";
    const auto stats = flitzzip::extract_zip(archive, restored, false);
    REQUIRE_EQ(stats.output_bytes, 29U);
    std::ifstream decoded(restored / L"source.txt", std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(decoded), {}), "controller ownership fixture\n");
    std::ifstream unchanged(fixture.input, std::ios::binary);
    REQUIRE_EQ(std::string(std::istreambuf_iterator<char>(unchanged), {}), "controller ownership fixture\n");
}
