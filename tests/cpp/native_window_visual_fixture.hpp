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

    // Purpose: Start a hidden native window with the requested saved integrity preference.
    // Inputs: Whether successful compression must record an archive SHA-256 result.
    // Outputs: An isolated controller whose settings exist before window initialization.
    explicit VisualSession(bool integrity = false) : fixture(integrity) {}

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

    // Purpose: Commit a public dropdown choice through ordinary native keyboard input.
    // Inputs: Owned field and bounded zero-based option. Outputs: One Home/Down/Enter selection.
    void choose_option(const RECT& field, int index) const {
        REQUIRE_TRUE(index >= 0 && index < 64);
        click_native_control(window, field);
        key(VK_HOME);
        for (int step = 0; step < index; ++step) {
            key(VK_DOWN);
        }
        key(VK_RETURN);
    }

    // Purpose: Recognize public field values independently of product state and formatting.
    // Inputs: Field geometry, literal value and select/enabled appearance. Outputs: Exact interior glyph equality.
    void require_field(const RECT& field, std::wstring_view text, bool select = true, bool enabled = true) {
        const RECT cell{field.left + px(10), field.top + px(21), field.right - px(select ? 30 : 10),
                        field.bottom - px(1)};
        require_native_observation(
            [&] {
                paint();
                return native_cell_text_matches(frame.dc(), cell, GetDpiForWindow(window), text,
                                                enabled ? flitzzip::app::kText : flitzzip::app::kSubtle);
            },
            "Native field does not display the independently expected value");
    }

    // Purpose: Deliver wheel input using the Win32 screen-coordinate contract without moving the user's cursor.
    // Inputs: Signed wheel delta and client point. Outputs: One acknowledged native wheel event.
    void wheel(short delta, POINT client_point) const {
        REQUIRE_TRUE(ClientToScreen(window, &client_point) != FALSE);
        HiddenController::send(window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)),
                               MAKELPARAM(client_point.x, client_point.y));
    }

    // Purpose: Join the real UI and verify exact worker accounting, source preservation and bounded writes.
    // Inputs: Expected admission/completion counts. Outputs: Closed controller and no timeout; idle output is empty.
    void finish(unsigned attempts = 0, unsigned completions = 0) {
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
        REQUIRE_EQ(launcher.attempts.load(), attempts);
        REQUIRE_EQ(launcher.completed.load(), completions);
        REQUIRE_TRUE(!launcher.release_timed_out.load());
        REQUIRE_EQ(read_native_fixture(fixture.input), "controller ownership fixture\n");
        if (completions == 0) {
            REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
        }
        std::uintmax_t footprint = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture.root)) {
            if (entry.is_regular_file()) {
                footprint += entry.file_size();
            }
        }
        REQUIRE_TRUE(footprint <= 64ULL * 1024ULL * 1024ULL);
    }
};

}  // namespace flitzzip_test::native_window
