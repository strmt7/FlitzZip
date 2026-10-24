#include "app/main_window.hpp"
#include "app/main_window_support.hpp"
#include "app/form_layout.hpp"
#include "app/history_publication.hpp"
#include "test_environment.hpp"

#include <fstream>
#include <future>
#include <memory>
#include <type_traits>
#include <system_error>
#include <shlobj.h>

namespace {

using namespace std::chrono_literals;

class GatedLauncher final : public flitzzip::app::JobThreadLauncher {
  public:
    mutable std::atomic_uint attempts{0};
    mutable std::promise<void> entered;
    mutable std::promise<void> first_completed;
    mutable std::atomic_bool first_notified{false};
    std::promise<void> released;
    std::shared_future<void> release = released.get_future().share();
    bool refuse_first = false;
    mutable std::atomic_bool release_timed_out{false};

    // Purpose: Control real admission and work timing without replacing the production job.
    // Inputs: Owned task and an instance-local release event. Outputs: Real owning thread, or explicit refusal.
    std::thread launch(std::function<void()> task) const override {
        if (attempts.fetch_add(1) == 0 && refuse_first) {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return JobThreadLauncher::launch([this, task = std::move(task)] {
            const bool first = !first_notified.exchange(true);
            if (first) {
                entered.set_value();
            }
            // The test must release before this deadline; a failed test cannot strand the native worker.
            if (release.wait_for(10s) != std::future_status::ready) {
                release_timed_out.store(true);
            }
            task();
            if (first) {
                first_completed.set_value();
            }
        });
    }
};

class HiddenController final {
  public:
    // Purpose: Run the actual controller on its own native UI thread without displaying or activating it.
    // Inputs: Launcher outlives this owner. Outputs: Running message pump, with exceptions retained for the test.
    explicit HiddenController(const flitzzip::app::JobThreadLauncher& launcher) : completed_(result_.get_future()) {
        thread_ = std::thread([this, &launcher] {
            thread_id_.store(GetCurrentThreadId());
            try {
                flitzzip::app::MainWindow controller(launcher);
                result_.set_value(controller.run(GetModuleHandleW(nullptr), SW_HIDE));
            } catch (...) {
                result_.set_exception(std::current_exception());
            }
        });
    }

    // Purpose: Retain all UI owners through orderly closure, including assertion failure cleanup.
    // Inputs: The test releases gated work before destruction. Outputs: No borrowed state or native thread remains.
    ~HiddenController() {
        if (const auto window = find_window()) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        thread_.join();
    }

    // Purpose: Discover only this test's owned HWND and confirm that creation has completed.
    // Inputs: Bounded deadline. Outputs: Invisible, responsive native FlitzZip window or test failure.
    HWND wait_window() {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        do {
            if (const auto window = find_window()) {
                send(window, WM_NULL);
                REQUIRE_TRUE(IsWindowVisible(window) == FALSE);
                return window;
            }
            if (completed_.wait_for(10ms) == std::future_status::ready) {
                static_cast<void>(completed_.get());
                throw std::runtime_error("controller exited before native window creation");
            }
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("controller window creation deadline expired");
    }

    // Purpose: Confirm actual native teardown and propagate unexpected controller failure.
    // Inputs: Close requested and gated work released. Outputs: Exact successful message-loop result.
    void require_closed() {
        REQUIRE_TRUE(completed_.wait_for(10s) == std::future_status::ready);
        REQUIRE_EQ(completed_.get(), 0);
    }

    // Purpose: Deliver a native event without permitting a blocked UI thread to hang the test driver.
    // Inputs: This controller's HWND and message. Outputs: Completed dispatch within one second or failure.
    static void send(HWND window, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
        DWORD_PTR result = 0;
        REQUIRE_TRUE(
            SendMessageTimeoutW(window, message, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) != 0);
    }

  private:
    // Purpose: Avoid global window lookup and accidental interaction with an unrelated product instance.
    // Inputs: The owned UI thread id. Outputs: Its FlitzZip HWND only, or null during creation/teardown.
    HWND find_window() const noexcept {
        const DWORD thread_id = thread_id_.load();
        if (thread_id == 0) {
            return nullptr;
        }
        HWND result = nullptr;
        EnumThreadWindows(
            thread_id,
            [](HWND window, LPARAM context) -> BOOL {
                wchar_t name[64]{};
                if (GetClassNameW(window, name, 64) && std::wstring_view(name) == L"FlitzZipMainWindow") {
                    *reinterpret_cast<HWND*>(context) = window;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&result));
        return result;
    }

    std::promise<int> result_;
    std::future<int> completed_;
    std::atomic<DWORD> thread_id_{0};
    std::thread thread_;
};

class ControllerFixture final {
  public:
    const std::filesystem::path root = test_temp_dir("hidden-controller");
    const std::filesystem::path input = root / L"source.txt";
    const std::filesystem::path output = root / L"archives";

    // Purpose: Isolate every settings, log and archive write from real user data.
    // Inputs: Existing documented smoke redirect and an owned temporary root. Outputs: Small CPU ZIP fixture.
    ControllerFixture() {
        temp_.set(root.c_str());
        tmp_.set(root.c_str());
        redirect_.set(L"1");
        destination_.set(output.c_str());
        suppress_shell_.set(L"1");
        auto_close_.set(nullptr);
        close_file_.set(nullptr);
        std::filesystem::create_directories(output);
        std::ofstream(input, std::ios::binary) << "controller ownership fixture\n";
        flitzzip::app::AppSettings settings;
        settings.gpu_required = false;
        settings.compression_format_index = 1;
        settings.show_operation_summary = false;
        settings.log_level_index = 2;
        flitzzip::app::ensure_app_storage();
        flitzzip::app::write_settings_file(flitzzip::app::settings_file_path(), settings);
    }

    // Purpose: Drive normal native input into the hidden product controller.
    // Inputs: Owned HWND and one existing source. Outputs: Queued source, selected destination, Compress page.
    void prepare_compression(HWND window) const {
        const auto dpi = GetDpiForWindow(window);
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        const auto name = flitzzip::windows_regular_path(input).native();
        const auto bytes = sizeof(DROPFILES) + (name.size() + 2) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GHND, bytes);
        REQUIRE_TRUE(memory != nullptr);
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        if (drop == nullptr) {
            GlobalFree(memory);
            throw std::runtime_error("test HDROP allocation could not be locked");
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        drop->pt = POINT{scale(180), scale(220)};
        std::copy(name.begin(), name.end(), reinterpret_cast<wchar_t*>(drop + 1));
        GlobalUnlock(memory);
        HiddenController::send(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory));
        show_compression(window);
        const auto layout = compression_layout(window);
        click(window, midpoint(layout.destination));
    }

    // Purpose: Activate the normal Compress button, allowing repeat attempts to expose stuck admission.
    // Inputs: Owned HWND on the Compress page. Outputs: Native mouse dispatch through actual hit testing.
    static void start(HWND window) {
        click(window, midpoint(compression_layout(window).start));
    }

    // Purpose: Exercise the real Stop control instead of treating window closure as equivalent cancellation.
    // Inputs: Owned Compress page with admitted work. Outputs: Native Stop press/release through actual hit testing.
    static void stop(HWND window) {
        click(window, midpoint(compression_layout(window).stop));
    }

    // Purpose: Select the empty compression page through normal input.
    // Inputs: Owned idle controller. Outputs: Compress page without adding sources or admitting work.
    static void show_compression(HWND window) {
        const auto dpi = static_cast<int>(GetDpiForWindow(window));
        click(window, POINT{MulDiv(40, dpi, 96), MulDiv(156, dpi, 96)});
    }

    // Purpose: Return to Queue before dropping a valid source after an empty-start refusal.
    // Inputs: Owned idle controller. Outputs: Queue page through its native navigation hit region.
    static void show_queue(HWND window) {
        const auto dpi = static_cast<int>(GetDpiForWindow(window));
        click(window, POINT{MulDiv(40, dpi, 96), MulDiv(104, dpi, 96)});
    }

  private:
    // Purpose: Derive actual DPI/client form bounds without hardcoded host display dimensions.
    // Inputs: Owned HWND. Outputs: Production layout at current DPI.
    static flitzzip::app::CompressLayout compression_layout(HWND window) {
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        const auto dpi = GetDpiForWindow(window);
        client.left += MulDiv(flitzzip::app::kRailWidth, static_cast<int>(dpi), 96);
        client.top += MulDiv(flitzzip::app::kTopBar, static_cast<int>(dpi), 96);
        client.bottom -= MulDiv(flitzzip::app::kStatusBar, static_cast<int>(dpi), 96);
        return flitzzip::app::make_compress_layout(client, dpi);
    }

    // Purpose: Select an interior native hit point. Inputs: Physical rectangle. Outputs: Its integer midpoint.
    static POINT midpoint(const RECT& rect) {
        return POINT{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    }

    // Purpose: Deliver owned-window mouse messages without moving the system cursor or activating a window.
    // Inputs: Client point and owned HWND. Outputs: Complete native press/release sequence.
    static void click(HWND window, POINT point) {
        const auto position = MAKELPARAM(point.x, point.y);
        HiddenController::send(window, WM_LBUTTONDOWN, MK_LBUTTON, position);
        HiddenController::send(window, WM_LBUTTONUP, 0, position);
    }

    flitzzip_test::ScopedEnvironment temp_{L"TEMP"};
    flitzzip_test::ScopedEnvironment tmp_{L"TMP"};
    flitzzip_test::ScopedEnvironment redirect_{L"FLITZZIP_GUI_SMOKE_SETTINGS_REDIRECT"};
    flitzzip_test::ScopedEnvironment destination_{L"FLITZZIP_GUI_SMOKE_DESTINATION"};
    flitzzip_test::ScopedEnvironment suppress_shell_{L"FLITZZIP_GUI_SMOKE_SUPPRESS_SHELL_OPEN"};
    flitzzip_test::ScopedEnvironment auto_close_{L"FLITZZIP_GUI_SMOKE_AUTO_CLOSE_MS"};
    flitzzip_test::ScopedEnvironment close_file_{L"FLITZZIP_GUI_SMOKE_CLOSE_FILE"};
};

// Purpose: Release controlled work before the controller destructor even when a regression assertion fails.
// Inputs: Promise owned by an enclosing launcher. Outputs: Scope-exit release, without throwing.
class ReleaseOnExit final {
  public:
    // Purpose: Borrow the test gate. Inputs: Live promise. Outputs: An armed scope guard.
    explicit ReleaseOnExit(std::promise<void>& signal) : signal_(signal) {}
    // Purpose: Release work on failed assertions. Inputs: Armed guard. Outputs: Signal exactly once.
    ~ReleaseOnExit() {
        release();
    }
    // Purpose: Permit normal close verification before scope exit. Inputs: Armed gate. Outputs: One release.
    void release() {
        if (armed_) {
            signal_.set_value();
            armed_ = false;
        }
    }

  private:
    std::promise<void>& signal_;
    bool armed_ = true;
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
