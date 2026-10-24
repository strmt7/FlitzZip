#include "app/main_window.hpp"
#include "app/main_window_support.hpp"
#include "app/form_layout.hpp"
#include "app/history_publication.hpp"
#include "app/gdi_dc_state.hpp"
#include "app/table_columns.hpp"
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
    explicit ControllerFixture(bool integrity = false) {
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
        settings.integrity_hash_opt_in = integrity;
        settings.compression_format_index = 1;
        settings.show_operation_summary = false;
        settings.log_level_index = 2;
        flitzzip::app::ensure_app_storage();
        flitzzip::app::write_settings_file(flitzzip::app::settings_file_path(), settings);
    }

    // Purpose: Drive normal native input into the hidden product controller.
    // Inputs: Owned HWND and one existing source. Outputs: Queued source, selected destination, Compress page.
    void prepare_compression(HWND window) const {
        queue_path(window, input);
        show_compression(window);
        const auto layout = compression_layout(window);
        click(window, midpoint(layout.destination));
    }

    // Purpose: Admit a real file or folder through the normal Queue drop boundary.
    // Inputs: Owned Queue HWND and existing local path. Outputs: Complete native HDROP admission.
    static void queue_path(HWND window, const std::filesystem::path& path) {
        const auto dpi = GetDpiForWindow(window);
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        const auto name = flitzzip::windows_regular_path(path).native();
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
    }

    // Purpose: End existing Queue lifetimes through its visible Clear action.
    // Inputs: Owned Queue HWND. Outputs: Normal toolbar dispatch without private state access.
    static void clear_queue(HWND window) {
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        const auto dpi = GetDpiForWindow(window);
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        click(window, POINT{client.right - scale(flitzzip::app::kPageInsetX) - scale(36),
                            scale(flitzzip::app::kTopBar) + scale(flitzzip::app::kPageInsetY) +
                                scale(flitzzip::app::kPageHeaderHeight) / 2});
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

// Purpose: Own a bounded off-screen native capture without changing the desktop or retaining controller fonts.
// Inputs: Positive client dimensions. Outputs: Independent 32-bit bitmap and memory DC, released on every exit.
class QueueCapture final {
  public:
    // Purpose: Acquire a real memory-only target. Inputs: Pixel dimensions. Outputs: Owned DC/bitmap or assertion.
    QueueCapture(int width, int height) : dc_(CreateCompatibleDC(nullptr), DeleteDC), bitmap_(nullptr, DeleteObject) {
        REQUIRE_TRUE(dc_ != nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap_.reset(CreateDIBSection(dc_.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
        REQUIRE_TRUE(bitmap_ != nullptr);
        original_ = SelectObject(dc_.get(), bitmap_.get());
        REQUIRE_TRUE(original_ != nullptr && original_ != HGDI_ERROR);
    }
    // Purpose: Detach storage before destruction. Inputs: Owned native objects. Outputs: No retained GDI objects.
    ~QueueCapture() {
        if (original_ && original_ != HGDI_ERROR) {
            SelectObject(dc_.get(), original_);
        }
    }
    // Purpose: Borrow the live capture DC. Inputs: None. Outputs: DC valid only through this owner's lifetime.
    HDC dc() const noexcept {
        return dc_.get();
    }

  private:
    std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)> dc_;
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> bitmap_;
    HGDIOBJ original_ = nullptr;
};

// Purpose: Derive the first Queue size-cell from the public layout constants and the untouched column defaults.
// Inputs: Owned native window. Outputs: Current-DPI text rectangle without accessing private controller state.
RECT first_queue_size_cell(HWND window) {
    using namespace flitzzip::app;
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    const auto dpi = GetDpiForWindow(window);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int table_left = scale(kRailWidth) + scale(kPageInsetX);
    const int table_right = client.right - scale(kPageInsetX);
    const int available = std::max(scale(360), table_right - table_left - scale(kQueueCheckboxColumnWidth) - scale(22));
    const auto widths =
        fit_table_columns(std::array<double, 4>{276, 100, 96, 468}, kQueueColumnMinimums, available, dpi);
    const int left = table_left + scale(kQueueCheckboxColumnWidth) + widths[0];
    const int top = scale(kTopBar) + scale(kPageInsetY) + scale(54) + scale(kQueueHeaderHeight);
    return RECT{left + scale(8), top, left + widths[1] - scale(8), top + scale(kQueueRowHeight)};
}

// Purpose: Independently recognize one exact rendered table status using native reference glyphs.
// Inputs: Actual captured frame, size-cell bounds, DPI and literal expected text; no product formatter is called.
// Outputs: Exact RGB comparison against DrawTextW, excluding the scaled table border.
bool native_cell_text_matches(HDC actual, const RECT& cell, UINT dpi, std::wstring_view expected,
                              COLORREF color = flitzzip::app::kMuted) {
    const int width = cell.right - cell.left;
    const int height = cell.bottom - cell.top;
    QueueCapture reference(width, height);
    const int border = MulDiv(1, static_cast<int>(dpi), 96);
    const auto background = GetPixel(actual, cell.left, cell.top + border);
    REQUIRE_TRUE(background != CLR_INVALID);
    SetDCBrushColor(reference.dc(), background);
    RECT bounds{0, 0, width, height};
    REQUIRE_TRUE(FillRect(reference.dc(), &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH))) != FALSE);
    using OwnedFont = std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)>;
    OwnedFont font(CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI"),
                   DeleteObject);
    REQUIRE_TRUE(font != nullptr);
    flitzzip::app::GdiDcState saved(reference.dc());
    REQUIRE_TRUE(saved.valid());
    REQUIRE_TRUE(SelectObject(reference.dc(), font.get()) != nullptr);
    SetBkMode(reference.dc(), TRANSPARENT);
    SetTextColor(reference.dc(), color);
    REQUIRE_TRUE(DrawTextW(reference.dc(), expected.data(), static_cast<int>(expected.size()), &bounds,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE) != 0);
    for (int y = border; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (GetPixel(actual, cell.left + x, cell.top + y) != GetPixel(reference.dc(), x, y)) {
                return false;
            }
        }
    }
    return true;
}

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
