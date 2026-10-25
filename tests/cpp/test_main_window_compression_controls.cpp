#include "native_window_visual_fixture.hpp"

namespace {

using namespace flitzzip_test::native_window;

// Purpose: Recognize native form values with independent literal glyphs and disabled-state color.
// Inputs: Real capture, public field rectangle and expected visible text. Outputs: Exact label equality or failure.
void require_compression_field(VisualSession& ui, const RECT& field, std::wstring_view text, bool select = true,
                               bool enabled = true) {
    // Compare glyphs inside the field border; symmetric insets preserve the original vertical text center.
    const RECT cell{field.left + ui.px(10), field.top + ui.px(21), field.right - ui.px(select ? 30 : 10),
                    field.bottom - ui.px(1)};
    require_native_observation(
        [&] {
            ui.paint();
            return native_cell_text_matches(ui.frame.dc(), cell, GetDpiForWindow(ui.window), text,
                                            enabled ? flitzzip::app::kText : flitzzip::app::kSubtle);
        },
        "Compression field does not display the independently expected value");
}

// Purpose: Commit a dropdown selection through the full normal keyboard path.
// Inputs: Owned control and zero-based visible option. Outputs: Home, Down and Enter activation without private state.
void choose_compression_option(VisualSession& ui, const RECT& field, int index) {
    REQUIRE_TRUE(index >= 0 && index < 64);
    click_native_control(ui.window, field);
    ui.key(VK_HOME);
    for (int offset = 0; offset < index; ++offset) {
        ui.key(VK_DOWN);
    }
    ui.key(VK_RETURN);
}

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
    require_compression_field(ui, form.format, L"ZIP (.zip)");
    click_native_control(ui.window, form.format);
    ui.key(VK_END);
    ui.key(VK_ESCAPE);
    require_compression_field(ui, form.format, L"ZIP (.zip)");
    click_native_control(ui.window, form.format);
    ui.key(VK_HOME);
    // Long menus may open upward across the archive-name field. The title band is outside every page dropdown.
    click_native_control(ui.window, RECT{ui.px(200), ui.px(12), ui.px(220), ui.px(32)});
    require_compression_field(ui, form.format, L"ZIP (.zip)");
    require_compression_field(ui, form.archive_name, L"FlitzZip-output.zip", false);
    REQUIRE_EQ(read_native_fixture(flitzzip::app::settings_file_path()), persisted);
    ui.finish();
}

// Purpose: Keep standard-format efforts at numeric one through nine, including wraparound in both directions.
// Inputs: ZIP level dropdown and Home/End/arrows/Enter. Outputs: Literal boundary efforts; no Neutron option or work.
TEST_CASE(main_window_compression_zip_effort_boundaries_and_wrap) {
    VisualSession ui;
    const auto form = open_compression(ui);
    choose_compression_option(ui, form.compression_level, 0);
    require_compression_field(ui, form.compression_level, L"1");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_UP);
    ui.key(VK_RETURN);
    require_compression_field(ui, form.compression_level, L"9");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_DOWN);
    ui.key(VK_RETURN);
    require_compression_field(ui, form.compression_level, L"1");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_END);
    ui.key(VK_RETURN);
    require_compression_field(ui, form.compression_level, L"9");
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
        require_compression_field(ui, field, L"-", false, false);
    }
    choose_compression_option(ui, form.format, public_format_index(L".tar"));
    require_compression_field(ui, form.format, L"TAR (.tar)");
    require_compression_field(ui, form.archive_name, L"FlitzZip-output.tar", false);
    for (const auto field : {form.compression_level, form.method, form.block_size}) {
        click_native_control(ui.window, field);
        ui.key(VK_DOWN);
        ui.key(VK_ESCAPE);
        require_compression_field(ui, field, L"-", false, false);
    }
    ui.finish();
}

// Purpose: Enforce native-mode GPU policy in the user-facing level selector without launching a device workload.
// Inputs: Native format with explicitly preferred rather than required HIP. Outputs: Native label, CPU-eligible policy
// and numeric maximum nine.
TEST_CASE(main_window_compression_native_preferred_policy_has_no_neutron) {
    VisualSession ui;
    const auto form = open_compression(ui);
    choose_compression_option(ui, form.format, 0);
    require_compression_field(ui, form.format, L"FlitzZip (.flzip)");
    require_compression_field(ui, form.archive_name, L"FlitzZip-output.flzip", false);
    choose_compression_option(ui, form.method, 1);
    require_compression_field(ui, form.method, L"AMD HIP preferred");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_END);
    ui.key(VK_RETURN);
    require_compression_field(ui, form.compression_level, L"9");
    click_native_control(ui.window, form.compression_level);
    ui.key(VK_DOWN);
    ui.key(VK_RETURN);
    require_compression_field(ui, form.compression_level, L"1");
    ui.finish();
}

// Purpose: Preserve native block-size selection across a compatibility-format visit and explicit Settings Apply.
// Inputs: Native minimum/maximum block choices and ZIP/native transitions. Outputs: Literal byte-unit boundaries and
// persisted index.
TEST_CASE(main_window_compression_block_choices_survive_format_switch_and_apply) {
    VisualSession ui;
    const auto form = open_compression(ui);
    choose_compression_option(ui, form.format, 0);
    choose_compression_option(ui, form.block_size, 0);
    require_compression_field(ui, form.block_size, L"256 KiB");
    click_native_control(ui.window, form.block_size);
    ui.key(VK_END);
    ui.key(VK_RETURN);
    require_compression_field(ui, form.block_size, L"16 MiB");
    choose_compression_option(ui, form.format, 1);
    require_compression_field(ui, form.block_size, L"-", false, false);
    choose_compression_option(ui, form.format, 0);
    require_compression_field(ui, form.block_size, L"16 MiB");
    click_native_navigation(ui.window, 6);
    const auto settings =
        flitzzip::app::make_settings_layout(native_content_rect(ui.window), GetDpiForWindow(ui.window));
    click_native_control(ui.window, settings.apply);
    REQUIRE_TRUE(read_native_fixture(flitzzip::app::settings_file_path()).find("\"compressionBlockSizeIndex\": 6") !=
                 std::string::npos);
    click_native_navigation(ui.window, 1);
    require_compression_field(ui, form.block_size, L"16 MiB");
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
    require_compression_field(ui, form.format, L"FlitzZip (.flzip)");
    require_compression_field(ui, form.archive_name, L"FlitzZip-output.flzip", false);
    choose_compression_option(ui, form.format, public_format_index(L".tar.zst"));
    require_compression_field(ui, form.format, L"TAR.ZST (.tar.zst)");
    require_compression_field(ui, form.archive_name, L"FlitzZip-output.tar.zst", false);
    choose_compression_option(ui, form.format, 1);
    require_compression_field(ui, form.format, L"ZIP (.zip)");
    require_compression_field(ui, form.archive_name, L"FlitzZip-output.zip", false);
    ui.finish();
}
