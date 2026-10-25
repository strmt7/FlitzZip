#include "native_window_visual_fixture.hpp"

namespace {

using namespace flitzzip_test::native_window;

// Purpose: Find a requested format in the public ordered selector without duplicating its popularity ordering.
// Inputs: Documented extension. Outputs: Current public option index or a test failure for a missing format.
int public_format_index(std::wstring_view extension) {
    for (int index = 0; index <= flitzzip::app::kCompressionFormatMaxIndex; ++index) {
        if (flitzzip::app::compression_format_extension(index) == extension) {
            return index;
        }
    }
    throw std::runtime_error("Required public compression format is unavailable");
}

// Purpose: Open Compress without adding input or starting work. Inputs: Isolated native session. Outputs: Form bounds.
flitzzip::app::CompressLayout open_compression(VisualSession& ui) {
    click_native_navigation(ui.window, 1);
    require_native_navigation_selection(ui.window, ui.frame.dc(), 1);
    return flitzzip::app::make_compress_layout(native_content_rect(ui.window), GetDpiForWindow(ui.window));
}

}  // namespace

// Purpose: Discard speculative dropdown keyboard choices on Escape or an outside click.
// Inputs: ZIP form and uncommitted first/last-format highlights. Outputs: Unchanged format, filename and saved bytes.
TEST_CASE(main_window_compression_dropdown_cancel_preserves_selection) {
    VisualSession ui;
    const auto form = open_compression(ui);
    const auto persisted = read_native_fixture(flitzzip::app::settings_file_path());
    ui.require_field(form.format, L"ZIP (.zip)");
    click_native_control(ui.window, form.format);
    ui.key(VK_END);
    ui.key(VK_ESCAPE);
    ui.require_field(form.format, L"ZIP (.zip)");
    click_native_control(ui.window, form.format);
    ui.key(VK_HOME);
    // Long menus may open upward across the archive-name field. The title band is outside every page dropdown.
    click_native_control(ui.window, RECT{ui.px(200), ui.px(12), ui.px(220), ui.px(32)});
    ui.require_field(form.format, L"ZIP (.zip)");
    ui.require_field(form.archive_name, L"FlitzZip-output.zip", false);
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), persisted);
    ui.finish();
}

// Purpose: Keep standard-format efforts at numeric one through nine, including wraparound in both directions.
// Inputs: ZIP level dropdown and Home/End/arrows/Enter. Outputs: Literal boundary efforts; no Neutron option or work.
TEST_CASE(main_window_compression_zip_effort_boundaries_and_wrap) {
    VisualSession ui;
    const auto form = open_compression(ui);
    ui.choose_option(form.compression_level, 0);
    ui.require_field(form.compression_level, L"1");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_UP);
    ui.key(VK_RETURN);
    ui.require_field(form.compression_level, L"9");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_DOWN);
    ui.key(VK_RETURN);
    ui.require_field(form.compression_level, L"1");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_END);
    ui.key(VK_RETURN);
    ui.require_field(form.compression_level, L"9");
    ui.finish();
}

// Purpose: Keep native-only tuning disabled on ZIP and all tuning disabled on plain TAR.
// Inputs: Clicks and keyboard changes on disabled controls. Outputs: Disabled literal dashes and correct
// format/extension.
TEST_CASE(main_window_compression_disabled_controls_do_not_open_menus) {
    VisualSession ui;
    const auto form = open_compression(ui);
    for (const auto field : {form.method, form.block_size}) {
        click_native_control(ui.window, field);
        ui.key(VK_END);
        ui.key(VK_ESCAPE);
        ui.require_field(field, L"-", false, false);
    }
    ui.choose_option(form.format, public_format_index(L".tar"));
    ui.require_field(form.format, L"TAR (.tar)");
    ui.require_field(form.archive_name, L"FlitzZip-output.tar", false);
    for (const auto field : {form.compression_level, form.method, form.block_size}) {
        click_native_control(ui.window, field);
        ui.key(VK_DOWN);
        ui.key(VK_ESCAPE);
        ui.require_field(field, L"-", false, false);
    }
    ui.finish();
}

// Purpose: Enforce native-mode GPU policy in the user-facing level selector without launching a device workload.
// Inputs: Native format with explicitly preferred rather than required HIP. Outputs: Native label, CPU-eligible policy
// and numeric maximum nine.
TEST_CASE(main_window_compression_native_preferred_policy_has_no_neutron) {
    VisualSession ui;
    const auto form = open_compression(ui);
    ui.choose_option(form.format, 0);
    ui.require_field(form.format, L"FlitzZip (.flzip)");
    ui.require_field(form.archive_name, L"FlitzZip-output.flzip", false);
    ui.choose_option(form.method, 1);
    ui.require_field(form.method, L"AMD HIP preferred");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_END);
    ui.key(VK_RETURN);
    ui.require_field(form.compression_level, L"9");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_DOWN);
    ui.key(VK_RETURN);
    ui.require_field(form.compression_level, L"1");
    ui.finish();
}

// Purpose: Preserve native block-size selection across a compatibility-format visit and explicit Settings Apply.
// Inputs: Native minimum/maximum block choices and ZIP/native transitions. Outputs: Literal byte-unit boundaries and
// persisted index.
TEST_CASE(main_window_compression_block_choices_survive_format_switch_and_apply) {
    VisualSession ui;
    const auto form = open_compression(ui);
    ui.choose_option(form.format, 0);
    ui.choose_option(form.block_size, 0);
    ui.require_field(form.block_size, L"256 KiB");
    click_native_control(ui.window, form.block_size);
    ui.key(VK_END);
    ui.key(VK_RETURN);
    ui.require_field(form.block_size, L"16 MiB");
    ui.choose_option(form.format, 1);
    ui.require_field(form.block_size, L"-", false, false);
    ui.choose_option(form.format, 0);
    ui.require_field(form.block_size, L"16 MiB");
    click_native_navigation(ui.window, 6);
    const auto settings =
        flitzzip::app::make_settings_layout(native_content_rect(ui.window), GetDpiForWindow(ui.window));
    click_native_control(ui.window, settings.apply);
    REQUIRE_TRUE(read_native_fixture(flitzzip::app::settings_file_path()).find("\"compressionBlockSizeIndex\": 6") !=
                 std::string::npos);
    click_native_navigation(ui.window, 1);
    ui.require_field(form.block_size, L"16 MiB");
    ui.finish();
}

// Purpose: Keep format dropdown navigation and output extension synchronized through both ends of a long menu.
// Inputs: End-to-first keyboard wrap and long traversal to TAR.ZST. Outputs: Independently specified format/filename
// pairs.
TEST_CASE(main_window_compression_format_scroll_wrap_and_extension_agree) {
    VisualSession ui;
    const auto form = open_compression(ui);
    click_native_control(ui.window, form.format);
    ui.key(VK_END);
    ui.key(VK_DOWN);
    ui.key(VK_RETURN);
    ui.require_field(form.format, L"FlitzZip (.flzip)");
    ui.require_field(form.archive_name, L"FlitzZip-output.flzip", false);
    ui.choose_option(form.format, public_format_index(L".tar.zst"));
    ui.require_field(form.format, L"TAR.ZST (.tar.zst)");
    ui.require_field(form.archive_name, L"FlitzZip-output.tar.zst", false);
    ui.choose_option(form.format, 1);
    ui.require_field(form.format, L"ZIP (.zip)");
    ui.require_field(form.archive_name, L"FlitzZip-output.zip", false);
    ui.finish();
}
