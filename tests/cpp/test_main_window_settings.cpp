#include "native_window_test_environment.hpp"
#include "core/windows_handle.hpp"

namespace {

using namespace flitzzip_test::native_window;
using namespace std::chrono_literals;

// Purpose: Observe the persisted file independently of the product's settings parser.
// Inputs: The fixture-owned, bounded settings file. Outputs: Exact serialized bytes with the stream already closed.
std::string current_settings_bytes() {
    std::ifstream input(flitzzip::app::settings_file_path(), std::ios::binary);
    REQUIRE_TRUE(input.good());
    return std::string(std::istreambuf_iterator<char>(input), {});
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

// Purpose: Keep a compatible profile byte-exact on startup even when this binary does not understand every field.
// Inputs: Hidden controller and isolated current-schema profile containing precise extension data.
// Outputs: Starting and closing the real window neither rewrites the document nor starts archive work.
TEST_CASE(main_window_settings_startup_preserves_opaque_profile_bytes) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    const std::string original = R"( {"schema":"flitzzip.settings.v4","compressionFormat":".zip",
        "gpuRequired":false,"extension":{"integer":9007199254740993,"nested":[true,null,{"x":1e20}]}} )";
    {
        std::ofstream file(flitzzip::app::settings_file_path(), std::ios::binary);
        file << original;
    }
    HiddenController controller(launcher);
    const auto window = controller.wait_window();
    REQUIRE_EQ(current_settings_bytes(), original);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(current_settings_bytes(), original);
    REQUIRE_EQ(launcher.attempts.load(), 0U);
}

// Purpose: Prevent an older window from silently downgrading a future profile during startup or Apply.
// Inputs: Isolated unsupported generation and ordinary Settings gestures. Outputs: Original bytes remain intact.
TEST_CASE(main_window_settings_rejects_future_schema_without_overwrite) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    const std::string original = R"({"schema":"flitzzip.settings.v999","unknown":{"required":"keep"}})";
    {
        std::ofstream file(flitzzip::app::settings_file_path(), std::ios::binary);
        file << original;
    }
    HiddenController controller(launcher);
    const auto window = controller.wait_window();
    REQUIRE_EQ(current_settings_bytes(), original);
    click_native_navigation(window, 6);
    const auto layout = native_settings_layout(window);
    click_native_control(window, layout.show_operation_summary);
    click_native_control(window, layout.apply);
    REQUIRE_EQ(current_settings_bytes(), original);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(current_settings_bytes(), original);
    REQUIRE_EQ(launcher.attempts.load(), 0U);
}

// Purpose: Preserve another instance's successful save when an already-open Settings page applies a stale draft.
// Inputs: Hidden window, externally committed revision and ordinary Apply. Outputs: Newer bytes survive unchanged.
TEST_CASE(main_window_settings_rejects_stale_window_apply) {
    ControllerFixture fixture;
    GatedLauncher launcher;
    HiddenController controller(launcher);
    const auto window = controller.wait_window();
    const auto path = flitzzip::app::settings_file_path();
    auto other = flitzzip::app::read_settings_file(path);
    other.overwrite = !other.overwrite;
    const auto committed = flitzzip::app::write_settings_file(path, other, flitzzip::app::read_settings_document(path));
    click_native_navigation(window, 6);
    const auto layout = native_settings_layout(window);
    click_native_control(window, layout.show_operation_summary);
    click_native_control(window, layout.apply);
    REQUIRE_EQ(current_settings_bytes(), committed);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(launcher.attempts.load(), 0U);
}

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
        require_native_toggle(window, frame.dc(), layout.show_operation_summary, false);
        keyboard_toggle_summary(window);
        require_native_toggle(window, frame.dc(), layout.show_operation_summary, true);
        REQUIRE_EQ(current_settings_bytes(), original);
        click_native_navigation(window, 0);
        click_native_navigation(window, 6);
        require_native_toggle(window, frame.dc(), layout.show_operation_summary, false);
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
        REQUIRE_TRUE(applied.find("\"logRetentionDays\": 30") != std::string::npos);
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
        require_native_toggle(window, frame.dc(), native_settings_layout(window).show_operation_summary, true);
        REQUIRE_TRUE(current_settings_bytes().find("\"logRetentionDays\": 30") != std::string::npos);
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
    require_native_toggle(window, frame.dc(), layout.show_operation_summary, true);
    {
        auto held = hold_native_replacement_target(flitzzip::app::settings_file_path());
        click_native_control(window, layout.apply);
        REQUIRE_EQ(current_settings_bytes(), original);
        require_native_toggle(window, frame.dc(), layout.show_operation_summary, true);
        click_native_navigation(window, 0);
        click_native_navigation(window, 6);
        require_native_toggle(window, frame.dc(), layout.show_operation_summary, false);
        REQUIRE_EQ(current_settings_bytes(), original);
    }
    click_native_control(window, layout.show_operation_summary);
    click_native_control(window, layout.apply);
    REQUIRE_TRUE(current_settings_bytes().find("\"showOperationSummary\": true") != std::string::npos);
    click_native_navigation(window, 0);
    click_native_navigation(window, 6);
    require_native_toggle(window, frame.dc(), layout.show_operation_summary, true);
    HiddenController::send(window, WM_CLOSE);
    controller.require_closed();
    REQUIRE_EQ(launcher.attempts.load(), 0U);
    REQUIRE_TRUE(std::filesystem::is_empty(fixture.output));
}
