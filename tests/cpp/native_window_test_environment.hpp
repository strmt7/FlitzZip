#pragma once

#include "app/main_window.hpp"
#include "app/main_window_support.hpp"
#include "app/form_layout.hpp"
#include "app/gdi_dc_state.hpp"
#include "app/table_columns.hpp"
#include "core/windows_handle.hpp"
#include "test_environment.hpp"

#include <fstream>
#include <future>
#include <memory>
#include <source_location>
#include <system_error>
#include <type_traits>
#include <shlobj.h>

namespace flitzzip_test::native_window {

using namespace std::chrono_literals;

// Purpose: Poll an observable condition without retrying input or work.
// Inputs: Instance-local predicate and diagnostic. Outputs: Success within three seconds or a descriptive failure.
template <class Predicate> void require_native_observation(Predicate&& observe, const char* diagnostic) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        if (observe()) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(diagnostic);
}

class GatedLauncher final : public flitzzip::app::JobThreadLauncher {
  public:
    mutable std::atomic_uint attempts{0};
    mutable std::atomic_uint completed{0};
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
            completed.fetch_add(1);
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
                wait_for_startup(window, deadline);
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

    // Purpose: Await initialization within the existing creation budget, before applying ordinary input deadlines.
    // Inputs: Newly discovered owned HWND and the original ten-second creation deadline. Outputs: Acknowledged
    // native dispatch or a diagnostic failure; discovering an HWND alone does not establish message-pump readiness.
    static void wait_for_startup(HWND window, std::chrono::steady_clock::time_point deadline) {
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining <= 0ms || remaining > 10s) {
            throw std::runtime_error("controller window creation deadline is expired or invalid");
        }
        DWORD_PTR result = 0;
        SetLastError(ERROR_SUCCESS);
        // Initialization can legitimately own this thread before GetMessage starts. This is not the live-input test.
        const auto sent =
            SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_BLOCK, static_cast<UINT>(remaining.count()), &result);
        if (sent == 0) {
            throw std::runtime_error("controller initialization dispatch failed: win32_error=" +
                                     std::to_string(GetLastError()));
        }
    }

    // Purpose: Confirm actual native teardown and propagate unexpected controller failure.
    // Inputs: Close requested and gated work released. Outputs: Exact successful message-loop result.
    void require_closed() {
        REQUIRE_TRUE(completed_.wait_for(10s) == std::future_status::ready);
        REQUIRE_EQ(completed_.get(), 0);
    }

    // Purpose: Deliver a native event without permitting a blocked UI thread to hang the test driver.
    // Inputs: Owned HWND/message, bounded millisecond deadline and implicit call site.
    // Outputs: Acknowledged dispatch or exact native/caller evidence; input retains the one-second default.
    static void send(HWND window, UINT message, WPARAM wparam = 0, LPARAM lparam = 0, DWORD timeout_ms = 1000,
                     std::source_location caller = std::source_location::current()) {
        DWORD_PTR result = 0;
        const auto started = std::chrono::steady_clock::now();
        SetLastError(ERROR_SUCCESS);
        const auto sent =
            SendMessageTimeoutW(window, message, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout_ms, &result);
        const auto error = GetLastError();
        if (sent == 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            throw std::runtime_error(
                "Native message dispatch failed at " + std::filesystem::path(caller.file_name()).filename().string() +
                ":" + std::to_string(caller.line()) + " message=" + std::to_string(message) +
                " win32_error=" + std::to_string(error) + " elapsed_ms=" + std::to_string(elapsed.count()) +
                " timeout_ms=" + std::to_string(timeout_ms) +
                " live_window=" + std::to_string(IsWindow(window) != FALSE));
        }
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

// Purpose: Derive the actual content bounds for native form interactions.
// Inputs: Owned hidden HWND. Outputs: DPI-aware client content rectangle, without private controller access.
inline RECT native_content_rect(HWND window) {
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    const auto dpi = static_cast<int>(GetDpiForWindow(window));
    client.left += MulDiv(flitzzip::app::kRailWidth, dpi, 96);
    client.top += MulDiv(flitzzip::app::kTopBar, dpi, 96);
    client.bottom -= MulDiv(flitzzip::app::kStatusBar, dpi, 96);
    return client;
}

// Purpose: Find the native center of a control. Inputs: Physical bounds. Outputs: Integer client point.
inline POINT native_rect_center(const RECT& rect) {
    return POINT{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
}

// Purpose: Locate the shared compression form. Inputs: Owned window. Outputs: Current public layout at actual DPI.
inline flitzzip::app::CompressLayout native_compression_layout(HWND window) {
    return flitzzip::app::make_compress_layout(native_content_rect(window), GetDpiForWindow(window));
}

// Purpose: Locate Settings through the public DPI layout shared by drawing and hit testing.
// Inputs: Owned HWND. Outputs: Complete physical control rectangles without private model access.
inline flitzzip::app::SettingsLayout native_settings_layout(HWND window) {
    return flitzzip::app::make_settings_layout(native_content_rect(window), GetDpiForWindow(window));
}

// Purpose: Deliver native input to one owned point without moving the desktop cursor.
// Inputs: Live HWND and client point. Outputs: Acknowledged press/release messages.
inline void click_native_point(HWND window, POINT center) {
    const auto point = MAKELPARAM(center.x, center.y);
    HiddenController::send(window, WM_LBUTTONDOWN, MK_LBUTTON, point);
    HiddenController::send(window, WM_LBUTTONUP, 0, point);
}

// Purpose: Activate a real control without changing system input or another window's state.
// Inputs: Owned HWND and physical control rectangle. Outputs: Acknowledged native press/release.
inline void click_native_control(HWND window, const RECT& rect) {
    click_native_point(window, native_rect_center(rect));
}

// Purpose: Recognize a settled Settings switch without accessing controller state.
// Inputs: Owned HWND/frame, public switch bounds and expected value. Outputs: Independent knob/track pixel match.
inline void require_native_toggle(HWND window, HDC frame, const RECT& rect, bool enabled) {
    const auto dpi = static_cast<int>(GetDpiForWindow(window));
    const int left = rect.left + MulDiv(10, dpi, 96);
    const int right = rect.left + MulDiv(32, dpi, 96);
    const int middle = (rect.top + rect.bottom) / 2;
    require_native_observation(
        [&] {
            HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame), PRF_CLIENT);
            return GetPixel(frame, left, middle) == (enabled ? RGB(43, 111, 72) : flitzzip::app::kMuted) &&
                   GetPixel(frame, right, middle) == (enabled ? flitzzip::app::kOk : RGB(57, 69, 75));
        },
        "Settings switch did not render its expected native state");
}

// Purpose: Refuse replacement through actual Windows sharing while preserving an existing fixture file.
// Inputs: Owned regular file. Outputs: Adopted no-delete-sharing handle; no file content is changed.
inline flitzzip::windows_detail::HandleOwner hold_native_replacement_target(const std::filesystem::path& path) {
    flitzzip::windows_detail::HandleOwner held(CreateFileW(flitzzip::windows_api_path(path).c_str(), GENERIC_READ,
                                                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                           FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE_TRUE(held.get() != INVALID_HANDLE_VALUE);
    return held;
}

// Purpose: Induce real Windows publication refusal without replacing the writer or consuming host resources.
// Inputs: Owned temporary target. Outputs: Sentinel file and an adopted no-delete-sharing handle; source unchanged.
inline flitzzip::windows_detail::HandleOwner hold_native_publication_target(const std::filesystem::path& path) {
    {
        std::ofstream output(path, std::ios::binary);
        output << "preserve existing destination\n";
        REQUIRE_TRUE(output.good());
    }
    return hold_native_replacement_target(path);
}

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
        flitzzip::app::write_settings_file(flitzzip::app::settings_file_path(), settings,
                                           flitzzip::app::read_settings_document(flitzzip::app::settings_file_path()));
    }

    // Purpose: Drive normal native input into the hidden product controller.
    // Inputs: Owned HWND and one existing source. Outputs: Queued source, selected destination, Compress page.
    void prepare_compression(HWND window) const {
        queue_path(window, input);
        show_compression(window);
        const auto layout = native_compression_layout(window);
        click_native_control(window, layout.destination);
    }

    // Purpose: Deliver a bounded shell DROPFILES list through the native window handler.
    // Inputs: Owned HWND, literal path strings and client drop point. Outputs: Handler owns and releases the HDROP.
    static void drop_paths(HWND window, const std::vector<std::wstring>& paths, POINT point) {
        REQUIRE_TRUE(paths.size() <= 64U);
        std::size_t characters = 1;
        for (const auto& path : paths) {
            REQUIRE_TRUE(path.find(L'\0') == std::wstring::npos);
            REQUIRE_TRUE(path.size() < 32768U);
            characters += path.size() + 1U;
        }
        characters = std::max<std::size_t>(characters, 2U);
        REQUIRE_TRUE(characters <= 65536U);
        const auto bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GHND, bytes);
        REQUIRE_TRUE(memory != nullptr);
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        if (drop == nullptr) {
            GlobalFree(memory);
            throw std::runtime_error("test HDROP allocation could not be locked");
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        drop->pt = point;
        auto* cursor = reinterpret_cast<wchar_t*>(drop + 1);
        for (const auto& path : paths) {
            cursor = std::copy(path.begin(), path.end(), cursor) + 1;
        }
        GlobalUnlock(memory);
        HiddenController::send(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory));
    }

    // Purpose: Admit a real file or folder through the normal Queue drop boundary.
    // Inputs: Owned Queue HWND and existing local path. Outputs: Complete native HDROP admission.
    static void queue_path(HWND window, const std::filesystem::path& path) {
        const auto dpi = GetDpiForWindow(window);
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        drop_paths(window, {flitzzip::windows_regular_path(path).native()}, POINT{scale(180), scale(220)});
    }

    // Purpose: End existing Queue lifetimes through its visible Clear action.
    // Inputs: Owned Queue HWND. Outputs: Normal toolbar dispatch without private state access.
    static void clear_queue(HWND window) {
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        const auto dpi = GetDpiForWindow(window);
        const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
        click_native_point(window, POINT{client.right - scale(flitzzip::app::kPageInsetX) - scale(36),
                                         scale(flitzzip::app::kTopBar) + scale(flitzzip::app::kPageInsetY) +
                                             scale(flitzzip::app::kPageHeaderHeight) / 2});
    }

    // Purpose: Activate the normal Compress button, allowing repeat attempts to expose stuck admission.
    // Inputs: Owned HWND on the Compress page. Outputs: Native mouse dispatch through actual hit testing.
    static void start(HWND window) {
        click_native_control(window, native_compression_layout(window).start);
    }

    // Purpose: Exercise the real Stop control instead of treating window closure as equivalent cancellation.
    // Inputs: Owned Compress page with admitted work. Outputs: Native Stop press/release through actual hit testing.
    static void stop(HWND window) {
        click_native_control(window, native_compression_layout(window).stop);
    }

    // Purpose: Select the empty compression page through normal input.
    // Inputs: Owned idle controller. Outputs: Compress page without adding sources or admitting work.
    static void show_compression(HWND window) {
        const auto dpi = static_cast<int>(GetDpiForWindow(window));
        click_native_point(window, POINT{MulDiv(40, dpi, 96), MulDiv(156, dpi, 96)});
    }

    // Purpose: Return to Queue before dropping a valid source after an empty-start refusal.
    // Inputs: Owned idle controller. Outputs: Queue page through its native navigation hit region.
    static void show_queue(HWND window) {
        const auto dpi = static_cast<int>(GetDpiForWindow(window));
        click_native_point(window, POINT{MulDiv(40, dpi, 96), MulDiv(104, dpi, 96)});
    }

  private:
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

// Purpose: Observe the selected native rail item independently of the controller's private state.
// Inputs: Owned hidden window, complete off-screen bitmap and the expected public navigation ordinal.
// Outputs: Exactly one accent strip at the requested page, with all seven other strips inactive.
inline void require_native_navigation_selection(HWND window, HDC frame, int expected) {
    HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame), PRF_CLIENT);
    const auto dpi = static_cast<int>(GetDpiForWindow(window));
    for (int page = 0; page < 8; ++page) {
        const int x = MulDiv(2, dpi, 96);
        const int y = MulDiv(flitzzip::app::kTopBar + 10 + 63 * page + 31, dpi, 96);
        REQUIRE_EQ(GetPixel(frame, x, y), page == expected ? flitzzip::app::kAccent : flitzzip::app::kRail);
    }
}

// Purpose: Exercise the public rail hit region without moving the system cursor or showing a native dialog.
// Inputs: Owned window and one of the eight documented page ordinals. Outputs: Complete native click dispatch.
inline void click_native_navigation(HWND window, int page) {
    const auto dpi = static_cast<int>(GetDpiForWindow(window));
    click_native_point(window,
                       POINT{MulDiv(40, dpi, 96), MulDiv(flitzzip::app::kTopBar + 10 + 63 * page + 31, dpi, 96)});
}

// Purpose: Observe bounded fixture bytes independently of the product's codecs and settings parser.
// Inputs: Existing fixture-owned file. Outputs: Exact bytes with the input stream already closed.
inline std::string read_native_fixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE_TRUE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Purpose: Check exact dispatch counts in the fixture's log after acknowledged native events.
// Inputs: Nonempty literal marker and count. Outputs: Exact occurrences with bounded, useful failure evidence.
inline void require_native_log_count(std::string_view marker, std::size_t expected) {
    REQUIRE_TRUE(!marker.empty());
    const auto text = read_native_fixture(flitzzip::app::log_file_path());
    std::size_t count = 0;
    for (auto at = text.find(marker); at != std::string::npos; at = text.find(marker, at + marker.size())) {
        ++count;
    }
    if (count != expected) {
        throw std::runtime_error("Native log count for '" + std::string(marker) +
                                 "': expected=" + std::to_string(expected) + " actual=" + std::to_string(count));
    }
}

// Purpose: Derive the first Queue size-cell from the public layout constants and the untouched column defaults.
// Inputs: Owned native window. Outputs: Current-DPI text rectangle without accessing private controller state.
inline RECT first_queue_size_cell(HWND window) {
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
// Inputs: Actual frame, bounds, DPI, literal text, color and native text flags; no product formatter is called.
// Outputs: Exact RGB comparison against DrawTextW, excluding the scaled table border.
inline bool native_cell_text_matches(HDC actual, const RECT& cell, UINT dpi, std::wstring_view expected,
                                     COLORREF color = flitzzip::app::kMuted,
                                     UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
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
    REQUIRE_TRUE(DrawTextW(reference.dc(), expected.data(), static_cast<int>(expected.size()), &bounds, flags) != 0);
    for (int y = border; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (GetPixel(actual, cell.left + x, cell.top + y) != GetPixel(reference.dc(), x, y)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace flitzzip_test::native_window
