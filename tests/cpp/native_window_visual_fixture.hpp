#pragma once

#include "native_window_test_environment.hpp"

namespace flitzzip_test::native_window {

// Purpose: Return acknowledged native client bounds. Inputs: Owned live HWND. Outputs: Physical rectangle.
inline RECT native_client_bounds(HWND window) {
    RECT bounds{};
    REQUIRE_TRUE(GetClientRect(window, &bounds) != FALSE);
    return bounds;
}

// Purpose: Share real hidden-window ownership and independent pixel observations across ordinary interaction tests.
// Inputs: Small redirected fixture. Outputs: Owned UI thread, memory-only frame and no access to private UI state.
class VisualSession final {
  public:
    ControllerFixture fixture;
    GatedLauncher launcher;
    HiddenController controller{launcher};
    HWND window = controller.wait_window();
    RECT client = native_client_bounds(window);
    QueueCapture frame{client.right, client.bottom};

    // Purpose: Scale public design geometry. Inputs: DIP extent. Outputs: Current-window physical pixels.
    int px(int value) const {
        return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
    }

    // Purpose: Observe native rendering without a visible window or image files.
    // Inputs: Owned frame. Outputs: Synchronously painted memory bitmap.
    void paint() {
        HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame.dc()), PRF_CLIENT);
    }

    // Purpose: Capture a small observable region without depending on product state or text formatters.
    // Inputs: In-client physical rectangle. Outputs: Exact RGB pixels, including glyph and background differences.
    std::vector<COLORREF> pixels(const RECT& area) {
        REQUIRE_TRUE(area.left >= 0 && area.top >= 0 && area.right <= client.right && area.bottom <= client.bottom);
        REQUIRE_TRUE(area.right > area.left && area.bottom > area.top);
        paint();
        std::vector<COLORREF> result;
        result.reserve(static_cast<std::size_t>((area.right - area.left) * (area.bottom - area.top)));
        for (int y = area.top; y < area.bottom; ++y) {
            for (int x = area.left; x < area.right; ++x) {
                const auto color = GetPixel(frame.dc(), x, y);
                REQUIRE_TRUE(color != CLR_INVALID);
                result.push_back(color);
            }
        }
        return result;
    }

    // Purpose: Deliver a native virtual key to the owned controller. Inputs: Virtual key. Outputs: Acknowledgement.
    void key(WPARAM value) const {
        HiddenController::send(window, WM_KEYDOWN, value);
    }

    // Purpose: Deliver wheel input using the Win32 screen-coordinate contract without moving the user's cursor.
    // Inputs: Signed wheel delta and client point. Outputs: One acknowledged native wheel event.
    void wheel(short delta, POINT client_point) const {
        REQUIRE_TRUE(ClientToScreen(window, &client_point) != FALSE);
        HiddenController::send(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)),
                               MAKELPARAM(client_point.x, client_point.y));
    }

    // Purpose: Complete an interaction-only case and prove it did not launch archive work or alter its source.
    // Inputs: Live idle controller. Outputs: Joined window thread, unchanged source and empty archive destination.
    void finish() {
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
        REQUIRE_EQ(launcher.attempts.load(), 0U);
        REQUIRE_EQ(read_native_fixture(fixture.input), "controller ownership fixture\n");
        REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
    }
};

// Purpose: Poll an observable condition while preserving a finite deadline and the original failing assertion.
// Inputs: Instance-local observation and diagnostic. Outputs: Success or a descriptive timeout; never retries work.
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

}  // namespace flitzzip_test::native_window
