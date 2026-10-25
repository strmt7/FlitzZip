#include "native_window_test_environment.hpp"
#include "core/windows_handle.hpp"

namespace {

using namespace flitzzip_test::native_window;
using namespace std::chrono_literals;

// Purpose: Locate ordinary Settings controls through the current DPI-aware public form layout.
// Inputs: Owned hidden window. Outputs: Physical control rectangles independent of the host's display dimensions.
flitzzip::app::SettingsLayout native_settings_layout(HWND window) {
    return flitzzip::app::make_settings_layout(native_content_rect(window), GetDpiForWindow(window));
}

// Purpose: Observe the persisted file independently of the product's settings parser.
// Inputs: The fixture-owned, bounded settings file. Outputs: Exact serialized bytes with the stream already closed.
std::string current_settings_bytes() {
    std::ifstream input(flitzzip::app::settings_file_path(), std::ios::binary);
    REQUIRE_TRUE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Purpose: Observe the summary switch's actual rendered state rather than reading private controller fields.
// Inputs: Owned window/frame and the public switch bounds. Outputs: Correct settled knob/track pixels or failure.
void require_native_summary_toggle(HWND window, HDC frame, const RECT& rect, bool enabled) {
    const auto dpi = static_cast<int>(GetDpiForWindow(window));
    const int left = rect.left + MulDiv(10, dpi, 96);
    const int right = rect.left + MulDiv(32, dpi, 96);
    const int middle = (rect.top + rect.bottom) / 2;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    do {
        HiddenController::send(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(frame), PRF_CLIENT);
        if (GetPixel(frame, left, middle) == (enabled ? RGB(43, 111, 72) : flitzzip::app::kMuted) &&
            GetPixel(frame, right, middle) == (enabled ? flitzzip::app::kOk : RGB(57, 69, 75))) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Settings summary toggle did not render its expected native state");
}

// Purpose: Reach the ordinary summary switch through the documented tab order without activating other controls.
// Inputs: Hidden Settings page. Outputs: One keyboard activation after the eight navigation and three preceding
// targets.
void keyboard_toggle_summary(HWND window) {
    HiddenController::send(window, WM_KEYDOWN, VK_HOME);
    for (int index = 0; index < 11; ++index) {
        HiddenController::send(window, WM_KEYDOWN, VK_RIGHT);
    }
    HiddenController::send(window, WM_KEYDOWN, VK_SPACE);
}

}  // namespace

// Purpose: Keep Settings drafts isolated until Apply and retain keyboard/dropdown decisions across real restarts.
// Inputs: Hidden native controller, ordinary summary/retention controls and independent persisted-byte assertions.
// Outputs: Abandoned drafts revert, Escape preserves the last selection, applied values reload, and no work starts.
TEST_CASE(main_window_settings_keyboard_draft_apply_and_restart_preserve_intent) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    {
        HiddenController controller(launcher);
        const auto window = controller.wait_window();
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        QueueCapture frame(client.right, client.bottom);
        click_native_navigation(window, 6);
        require_native_navigation_selection(window, frame.dc(), 6);
        const auto layout = native_settings_layout(window);
        const auto original = current_settings_bytes();
        require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, false);
        keyboard_toggle_summary(window);
        require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, true);
        REQUIRE_EQ(current_settings_bytes(), original);
        click_native_navigation(window, 0);
        click_native_navigation(window, 6);
        require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, false);
        REQUIRE_EQ(current_settings_bytes(), original);
        keyboard_toggle_summary(window);
        click_native_control(window, layout.log_retention);
        for (const auto key : {VK_HOME, VK_LEFT, VK_RIGHT, VK_END, VK_UP, VK_DOWN, VK_RETURN}) {
            HiddenController::send(window, WM_KEYDOWN, key);
        }
        click_native_control(window, layout.log_retention);
        HiddenController::send(window, WM_KEYDOWN, VK_HOME);
        HiddenController::send(window, WM_KEYDOWN, VK_ESCAPE);
        click_native_control(window, layout.apply);
        const auto applied = current_settings_bytes();
        REQUIRE_TRUE(applied.find("\"showOperationSummary\": true") != std::string::npos);
        REQUIRE_TRUE(applied.find("\"logRetentionIndex\": 2") != std::string::npos);
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
    }
    {
        HiddenController controller(launcher);
        const auto window = controller.wait_window();
        RECT client{};
        REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
        QueueCapture frame(client.right, client.bottom);
        click_native_navigation(window, 6);
        require_native_summary_toggle(window, frame.dc(), native_settings_layout(window).show_operation_summary, true);
        REQUIRE_TRUE(current_settings_bytes().find("\"logRetentionIndex\": 2") != std::string::npos);
        HiddenController::send(window, WM_CLOSE);
        controller.require_closed();
    }
    REQUIRE_EQ(launcher.attempts.load(), 0U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
}

// Purpose: Preserve persisted and applied preferences when Windows refuses replacement, then recover in place.
// Inputs: Real sharing denial on the fixture settings file and ordinary summary-toggle/Apply actions.
// Outputs: Unchanged bytes after failure, abandoned draft rollback, successful retry, and no leaked file/thread owners.
TEST_CASE(main_window_settings_failed_save_preserves_applied_state_and_recovers) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    HiddenController controller(launcher);
    const auto window = controller.wait_window();
    RECT client{};
    REQUIRE_TRUE(GetClientRect(window, &client) != FALSE);
    QueueCapture frame(client.right, client.bottom);
    click_native_navigation(window, 6);
    const auto layout = native_settings_layout(window);
    const auto original = current_settings_bytes();
    click_native_control(window, layout.show_operation_summary);
    require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, true);
    {
        flitzzip::windows_detail::HandleOwner held(CreateFileW(flitzzip::app::settings_file_path().c_str(),
                                                               GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                                               FILE_ATTRIBUTE_NORMAL, nullptr));
        REQUIRE_TRUE(held.get() != INVALID_HANDLE_VALUE);
        click_native_control(window, layout.apply);
        REQUIRE_EQ(current_settings_bytes(), original);
        require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, true);
        click_native_navigation(window, 0);
        click_native_navigation(window, 6);
        require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, false);
        REQUIRE_EQ(current_settings_bytes(), original);
    }
    click_native_control(window, layout.show_operation_summary);
    click_native_control(window, layout.apply);
    REQUIRE_TRUE(current_settings_bytes().find("\"showOperationSummary\": true") != std::string::npos);
    click_native_navigation(window, 0);
    click_native_navigation(window, 6);
    require_native_summary_toggle(window, frame.dc(), layout.show_operation_summary, true);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(launcher.attempts.load(), 0U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
}
