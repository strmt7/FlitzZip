#pragma once

#include "app/main_window_layout.hpp"
#include "app/main_window_state.hpp"
#include "core/progress.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

namespace superzip::app {

class QueueDropTarget;

class MainWindow {
    friend class QueueDropTarget;

  public:
    // Purpose: Construct a main window controller with default UI state.
    // Inputs: None.
    // Outputs: Initializes fields only; the native HWND is created by `run`.
    MainWindow();

    // Purpose: Join worker threads and release GDI font handles.
    // Inputs: None.
    // Outputs: Blocks until any active worker finishes, then frees owned resources.
    ~MainWindow();

    // Purpose: Register and show the native Win32 SuperZip window.
    // Inputs: `instance` is the process HINSTANCE and `show_command` is the Windows show mode from `wWinMain`.
    // Outputs: Runs the message loop and returns the process exit code.
    int run(HINSTANCE instance, int show_command);

  private:
    using QueueLayout = superzip::app::QueueLayout;
    using QueueColumnLayout = superzip::app::QueueColumnLayout;
    using HistoryColumnLayout = superzip::app::HistoryColumnLayout;
    using ExtractJobRequest = superzip::app::ExtractJobRequest;
    using ProcessIoRates = superzip::app::ProcessIoRates;
    using CompressLayout = superzip::app::CompressLayout;
    using ExtractLayout = superzip::app::ExtractLayout;
    using SettingsLayout = superzip::app::SettingsLayout;

    // Purpose: Route Win32 messages from the static window procedure to the C++ instance.
    // Inputs: Standard Win32 `hwnd`, `message`, `wparam`, and `lparam` arguments.
    // Outputs: Returns the message result expected by Win32.
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    // Purpose: Handle window, input, DPI, drag/drop, repaint, and lifecycle messages.
    // Inputs: Standard Win32 message payload.
    // Outputs: Returns zero for handled messages or delegates to `DefWindowProcW`.
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    // Purpose: Handle keyboard traversal and activation with native Windows conventions.
    // Inputs: `wparam` is a virtual key and `lparam` is the raw key message payload.
    // Outputs: Moves focus, activates controls, updates dropdowns, or returns default processing.
    LRESULT handle_key_down(WPARAM wparam, LPARAM lparam);

    // Purpose: Track pointer hover state and arm native leave notifications.
    // Inputs: `lparam` contains client-coordinate mouse position from `WM_MOUSEMOVE`.
    // Outputs: Updates hover state and returns the handled Win32 result.
    LRESULT handle_mouse_move(LPARAM lparam);

    // Purpose: Clear pointer hover state after the cursor leaves the client area.
    // Inputs: None; uses current capture state.
    // Outputs: Updates mouse state and returns the handled Win32 result.
    LRESULT handle_mouse_leave();

    // Purpose: Update delayed text-tooltip tracking from the current mouse position.
    // Inputs: None; reads current page, eligible text cells, and mouse coordinates.
    // Outputs: Arms, hides, or preserves the tooltip timer based on ellipsized hover state.
    void update_text_tooltip_tracking();

    // Purpose: Return the ellipsized text under the current mouse, if any.
    // Inputs: None; uses current page layout, queue/form state, and selected fonts.
    // Outputs: Returns true with cell/text set only for eligible truncated text targets.
    bool text_tooltip_candidate_at_mouse(RECT& cell, std::wstring& text);

    // Purpose: Return the ellipsized Queue text under the current mouse, if any.
    // Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
    // Outputs: Returns true only for truncated Queue Name or Path cells.
    bool queue_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text);

    // Purpose: Return the ellipsized History text under the current mouse, if any.
    // Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
    // Outputs: Returns true only for truncated Archive name or Archive path cells.
    bool history_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text);

    // Purpose: Return the ellipsized Compress/Extract field text under the current mouse, if any.
    // Inputs: `state` is a stable UI snapshot and `cell`/`text` receive tooltip data.
    // Outputs: Returns true only for truncated Archive/Destination fields explicitly allowed by the UI contract.
    bool field_text_tooltip_candidate_at_mouse(const UiState& state, RECT& cell, std::wstring& text);

    // Purpose: Check one form field value for delayed tooltip eligibility.
    // Inputs: `field` is the full labeled field rectangle, `value` is the visible text, and `cell`/`text` receive data.
    // Outputs: Returns true only when the mouse is over a truncated value cell.
    bool tooltip_candidate_for_field(const RECT& field, const std::wstring& value, RECT& cell,
                                     std::wstring& text) const;

    // Purpose: Resolve the drawn value area inside a labeled field.
    // Inputs: `field` is the full labeled field rectangle and `select` reserves space for a dropdown arrow when true.
    // Outputs: Returns the value text rectangle used by both drawing and tooltip hit testing.
    [[nodiscard]] RECT field_value_cell(const RECT& field, bool select) const;

    // Purpose: Decide whether a text value overflows the visible cell.
    // Inputs: `text` is rendered in `cell` with `font`.
    // Outputs: Returns true when the text would be ellipsized.
    [[nodiscard]] bool text_overflows_cell(std::wstring_view text, const RECT& cell, HFONT font) const;

    // Purpose: Measure text with an existing GDI font.
    // Inputs: `text` is UTF-16 content and `font` is one of the window-owned fonts.
    // Outputs: Returns the text width in physical pixels, or zero when measurement is unavailable.
    [[nodiscard]] int text_width(std::wstring_view text, HFONT font) const;

    // Purpose: Handle primary-button press using the same geometry as rendering.
    // Inputs: `lparam` contains client-coordinate click position from `WM_LBUTTONDOWN`.
    // Outputs: Updates capture/pressed state, dispatches page or content clicks, and returns the handled Win32 result.
    LRESULT handle_primary_mouse_down(LPARAM lparam);

    // Purpose: Handle primary-button release and trigger the shared command release pulse.
    // Inputs: `lparam` contains client-coordinate release position from `WM_LBUTTONUP`.
    // Outputs: Releases capture, updates mouse state, queues animation, and returns the handled Win32 result.
    LRESULT handle_primary_mouse_up(LPARAM lparam);

    // Purpose: Scroll the Queue table with native mouse-wheel semantics.
    // Inputs: `wparam` contains wheel delta and `lparam` contains screen coordinates.
    // Outputs: Updates the first visible Queue row when the pointer is over an overflowing Queue table.
    LRESULT handle_mouse_wheel(WPARAM wparam, LPARAM lparam);

    // Purpose: Normalize mouse state after Windows changes capture ownership.
    // Inputs: None.
    // Outputs: Clears pressed/capture flags and returns the handled Win32 result.
    LRESULT handle_capture_changed();

    // Purpose: Append native shell-dropped paths to the queue.
    // Inputs: `wparam` contains the HDROP handle from `WM_DROPFILES`.
    // Outputs: Updates queue selection, releases the HDROP handle, and returns the handled Win32 result.
    LRESULT handle_drop_files(WPARAM wparam);

    // Purpose: Append dropped paths to the Queue when the drop target is inside the Queue table.
    // Inputs: `paths` are filesystem paths from shell drag/drop and `point` is the client drop coordinate.
    // Outputs: Returns true and mutates queue state when the drop is accepted; otherwise reports rejection.
    bool accept_dropped_paths(std::vector<std::filesystem::path> paths, POINT point);

    // Purpose: Update live drag/drop highlighting for the Queue table.
    // Inputs: `active` describes whether a drag is over the allowed table drop target.
    // Outputs: Updates visual drag state and queues a repaint when changed.
    void set_queue_drop_highlight(bool active);

    // Purpose: Return whether a client point is inside the active Queue table drop target.
    // Inputs: `point` is a client-coordinate point.
    // Outputs: Returns true only on the Queue page, with no SuperZip modal active, and inside the Queue table.
    [[nodiscard]] bool queue_drop_target_contains(POINT point);

    // Purpose: Allow shell file-drop messages through UIPI for elevated windows.
    // Inputs: None; applies only to the main HWND and only when the process is elevated.
    // Outputs: Returns true when no extra filter is needed or the narrow filter was applied.
    [[nodiscard]] bool enable_elevated_drag_drop_messages() const;

    // Purpose: Initialize drag/drop, performance sampling, and smoke timers during window creation.
    // Inputs: None.
    // Outputs: Arms timers and returns the handled Win32 result.
    LRESULT handle_create();

    // Purpose: Load persisted per-user settings and publish the current applied snapshot.
    // Inputs: None; reads the current user's Local AppData settings file when present.
    // Outputs: Applies validated settings, creates defaults when needed, and logs nonfatal parse/write failures.
    void initialize_settings();

    // Purpose: Save the current Settings draft as the applied snapshot.
    // Inputs: None; reads Settings fields from synchronized UI state.
    // Outputs: Writes the per-user config atomically and updates the applied snapshot.
    void apply_settings();

    // Purpose: Revert Settings page controls to the last applied snapshot.
    // Inputs: None.
    // Outputs: Mutates Settings-owned UI fields and re-arms dependent timers.
    void revert_settings_draft();

    // Purpose: Reset Settings draft and applied snapshot to safe defaults.
    // Inputs: None.
    // Outputs: Updates UI, applied snapshot, and the persisted config file.
    void reset_settings_to_defaults();

    // Purpose: Dispatch timers owned by the main window.
    // Inputs: `wparam` identifies the timer from `WM_TIMER`.
    // Outputs: Runs animation, monitoring, or smoke shutdown work and returns the handled Win32 result.
    LRESULT handle_timer(WPARAM wparam);

    // Purpose: Stop timers and release monitor state before window destruction completes.
    // Inputs: None.
    // Outputs: Posts quit and returns the handled Win32 result.
    LRESULT handle_destroy();

    // Purpose: Paint the full client area using an off-screen buffer.
    // Inputs: None; obtains paint state from Win32.
    // Outputs: Renders one crisp frame and validates the paint region.
    void paint();

    // Purpose: Draw the current frame using DPI-scaled layout regions.
    // Inputs: `dc` is the off-screen device context and `rect` is the client rectangle in physical pixels.
    // Outputs: Writes the shell, navigation, active page, and status strip into `dc`.
    void layout_and_draw(HDC dc, const RECT& rect);

    // Purpose: Return whether the mouse is currently over a live clickable target.
    // Inputs: `rect` is the clickable target and `enabled` must be false for static or disabled UI.
    // Outputs: Returns true only for enabled controls under the mouse pointer.
    [[nodiscard]] bool interactive_hovered(const RECT& rect, bool enabled = true) const;

    // Purpose: Compute the subtle clickable hover fill used by all interactive boxes.
    // Inputs: `base` is the non-hover fill, `rect` is the clickable target, `enabled` gates interaction, and `accent`
    // selects command-button treatment.
    // Outputs: Returns a slightly lifted background color without changing borders or behavior.
    [[nodiscard]] COLORREF interactive_fill(COLORREF base, const RECT& rect, bool enabled = true,
                                            bool accent = false) const;

    // Purpose: Draw the shared hover background for row-like controls.
    // Inputs: `dc` is the target, `rect` is the clickable row, and `enabled` gates interaction.
    // Outputs: Adds only a subtle background lift when the row is hovered.
    void draw_interactive_hover_surface(HDC dc, const RECT& rect, bool enabled = true);

    // Purpose: Draw the keyboard focus affordance for the current target.
    // Inputs: `dc` is the target, `content` is the content area, and `state` is the copied UI state.
    // Outputs: Renders a minimal non-hover focus indicator without mutating state.
    void draw_keyboard_focus(HDC dc, const RECT& content, const UiState& state);

    // Purpose: Draw the delayed Queue text tooltip when an ellipsized Name or Path cell is hovered.
    // Inputs: `dc` is the target for the current frame.
    // Outputs: Renders a compact tooltip above the Queue table without affecting layout.
    void draw_text_tooltip(HDC dc);

    // Purpose: Draw the SuperZip-owned extraction overwrite confirmation modal.
    // Inputs: `dc` is the target, `rect` is the full client area, and `state` contains modal text.
    // Outputs: Renders no pixels unless the overwrite confirmation is active.
    void draw_extract_overwrite_prompt(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Return the centered extraction overwrite modal panel.
    // Inputs: `rect` is the full client area.
    // Outputs: Returns a DPI-scaled panel rectangle using the main-window visual system.
    [[nodiscard]] RECT extract_overwrite_prompt_rect(const RECT& rect) const;

    // Purpose: Return modal action button rectangles in Continue/Cancel order.
    // Inputs: `modal` is the overwrite prompt panel rectangle.
    // Outputs: Returns two right-aligned button rectangles.
    [[nodiscard]] std::array<RECT, 2> extract_overwrite_prompt_buttons(const RECT& modal) const;

    // Purpose: Draw the SuperZip-owned generated license-notice modal.
    // Inputs: `dc` is the target, `rect` is the full client area, and `state` contains modal visibility.
    // Outputs: Renders no pixels unless the license-notice dialog is active.
    void draw_license_notices_dialog(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Return the centered license-notice modal panel.
    // Inputs: `rect` is the full client area.
    // Outputs: Returns a DPI-scaled panel rectangle using the main-window visual system.
    [[nodiscard]] RECT license_notices_dialog_rect(const RECT& rect) const;

    // Purpose: Return the scrollable text viewport inside the license-notice modal.
    // Inputs: `modal` is the license-notice panel rectangle.
    // Outputs: Returns the clipped text area used for drawing and scroll math.
    [[nodiscard]] RECT license_notices_viewport_rect(const RECT& modal) const;

    // Purpose: Return the Close button rectangle for the license-notice modal.
    // Inputs: `modal` is the license-notice panel rectangle.
    // Outputs: Returns a right-aligned command rectangle.
    [[nodiscard]] RECT license_notices_close_button_rect(const RECT& modal) const;

    // Purpose: Return SuperZip/Other tab rectangles for the license-notice modal.
    // Inputs: `modal` is the license-notice panel rectangle.
    // Outputs: Returns tab rectangles in SuperZip, Other order.
    [[nodiscard]] std::array<RECT, 2> license_notices_tab_rects(const RECT& modal) const;

    // Purpose: Build the generated license-notice text for the active modal tab.
    // Inputs: None; reads the generated notice table and current tab index.
    // Outputs: Returns cached UTF-16 notice body generated from source-controlled licenses.
    [[nodiscard]] const std::wstring& license_notices_text();

    // Purpose: Change the active license-notice tab.
    // Inputs: `tab_index` is zero for SuperZip and one for Other.
    // Outputs: Updates tab and scroll state, then queues repaint when the tab changes.
    void select_license_notices_tab(int tab_index);

    // Purpose: Measure the rendered license-notice body height.
    // Inputs: `dc` is the current target and `viewport` is the text viewport.
    // Outputs: Returns the DPI-scaled text height needed for scroll bounds.
    [[nodiscard]] int license_notices_text_height(HDC dc, const RECT& viewport) const;

    // Purpose: Apply a bounded scroll delta to the license-notice modal.
    // Inputs: `delta_pixels` is positive for down and negative for up.
    // Outputs: Updates scroll offset and queues a repaint when the dialog is visible.
    bool scroll_license_notices_dialog(int delta_pixels);

    // Purpose: Return the product-styled license-notice scrollbar track.
    // Inputs: `viewport` is the clipped license text viewport.
    // Outputs: Returns the slim vertical track inside the modal text frame.
    [[nodiscard]] RECT license_notices_scrollbar_track_rect(const RECT& viewport) const;

    // Purpose: Return the license-notice scrollbar thumb for the current scroll offset.
    // Inputs: `viewport` is the clipped license text viewport and `content_height` is the measured notice height.
    // Outputs: Returns an empty rectangle when no scrolling is needed.
    [[nodiscard]] RECT license_notices_scrollbar_thumb_rect(const RECT& viewport, int content_height) const;

    // Purpose: Begin dragging the license-notice scrollbar thumb.
    // Inputs: `y` is the mouse position; `viewport` and `content_height` define the active scroll geometry.
    // Outputs: Captures the scroll baseline when the notice body overflows.
    void begin_license_notices_scroll_drag(int y, const RECT& viewport, int content_height);

    // Purpose: Update license-notice scrolling from an active thumb drag.
    // Inputs: `y` is the current client mouse y-coordinate.
    // Outputs: Moves the bounded scroll offset and queues repaint when changed.
    void update_license_notices_scroll_drag(int y);

    // Purpose: End any active license-notice scrollbar drag.
    // Inputs: None.
    // Outputs: Clears the modal scrollbar drag state.
    void end_license_notices_scroll_drag();

    // Purpose: Close the license-notice modal.
    // Inputs: None.
    // Outputs: Clears modal state, resets scroll state, and queues repaint.
    void close_license_notices_dialog();

    // Purpose: Open the generated license-notice modal from the About page.
    // Inputs: None.
    // Outputs: Shows the modal, resets scroll state, and queues repaint.
    void show_license_notices_dialog();

    // Purpose: Draw the persistent product shell strip.
    // Inputs: `dc` is the target and `rect` is the full client rectangle.
    // Outputs: Renders the brand chrome; page-specific actions stay inside their pages.
    void draw_top_bar(HDC dc, const RECT& rect);

    // Purpose: Draw the compact navigation rail.
    // Inputs: `dc` is the target, `rect` is the rail rectangle, and `state` is the copied UI state.
    // Outputs: Renders all primary pages with active-page highlighting.
    void draw_navigation(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the persistent AMD GPU and operation status strip.
    // Inputs: `dc` is the target, `rect` is the status-strip rectangle, and `state` is the copied UI state.
    // Outputs: Renders backend status, throughput, VRAM placeholder, and details affordance.
    void draw_status_bar(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the active content page.
    // Inputs: `dc` is the target device context and `rect` is the content rectangle in physical pixels.
    // Outputs: Dispatches to the active page renderer.
    void draw_content(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Compute Queue page rectangles shared by rendering and hit testing.
    // Inputs: `rect` is the content area in physical pixels.
    // Outputs: Returns DPI-scaled Queue page control rectangles.
    [[nodiscard]] QueueLayout queue_layout(const RECT& rect) const;

    // Purpose: Return the shared bottom-right primary action button rectangle for operation pages.
    // Inputs: `area` is the DPI-scaled page content area.
    // Outputs: Returns a 110x36 design-pixel command rectangle aligned with Settings Apply.
    [[nodiscard]] RECT primary_action_rect(const RECT& area) const;

    // Purpose: Return the History Clear History button rectangle with Restore Defaults-equivalent visual margins.
    // Inputs: `area` is the DPI-scaled page content area.
    // Outputs: Returns a right-aligned command rectangle sized from the active button font.
    [[nodiscard]] RECT history_clear_button_rect(const RECT& area) const;

    // Purpose: Return the About page Licenses button rectangle.
    // Inputs: `area` is the DPI-scaled page content area.
    // Outputs: Returns a compact command rectangle inside the About card.
    [[nodiscard]] RECT about_licenses_button_rect(const RECT& area) const;

    // Purpose: Compute Compress page rectangles shared by rendering and hit testing.
    // Inputs: `rect` is the content area in physical pixels.
    // Outputs: Returns DPI-scaled Compress page control rectangles.
    [[nodiscard]] CompressLayout compress_layout(const RECT& rect) const;

    // Purpose: Compute Extract page rectangles shared by rendering and hit testing.
    // Inputs: `rect` is the content area in physical pixels.
    // Outputs: Returns DPI-scaled Extract page control rectangles.
    [[nodiscard]] ExtractLayout extract_layout(const RECT& rect) const;

    // Purpose: Compute Settings page rectangles shared by rendering and hit testing.
    // Inputs: `rect` is the content area in physical pixels.
    // Outputs: Returns DPI-scaled Settings page control rectangles.
    [[nodiscard]] SettingsLayout settings_layout(const RECT& rect) const;

    // Purpose: Draw the queue page with the file/folder selection table only.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` is copied UI state.
    // Outputs: Renders queue selection controls; operation configuration remains on later pages.
    void draw_queue_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the compression settings page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
    // Outputs: Renders archive destination, format, compression level, advanced options, integrity toggles, and start
    // control.
    void draw_compress_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the extraction settings page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains opt-in settings.
    // Outputs: Renders archive/destination fields, overwrite policy, integrity toggles, and start control.
    void draw_extract_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the security review page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains security choices.
    // Outputs: Renders path, CRC, integrity, Defender, and overwrite checks with explicit status.
    void draw_security_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the operation history page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains session history.
    // Outputs: Renders filters, history rows, and selected-operation details.
    void draw_history_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw History page filter controls and clear action.
    // Inputs: `dc` is the target, `area` is the page work area, and `state` contains current filter rows.
    // Outputs: Renders the clear button plus operation/status filter fields.
    void draw_history_filters(HDC dc, const RECT& area, const UiState& state);

    // Purpose: Draw the History table header, rows, empty states, and scrollbar.
    // Inputs: `dc` is the target, `table` is the table rectangle, and `state` supplies visible history data.
    // Outputs: Renders the bounded fixed-header history table.
    void draw_history_table(HDC dc, const RECT& table, const UiState& state);

    // Purpose: Draw the History table header row.
    // Inputs: `dc` is the target, `table` and `columns_table` describe the table geometry.
    // Outputs: Renders column titles, resize grips, and the header/body separator.
    void draw_history_table_header(HDC dc, const RECT& table, const RECT& columns_table);

    // Purpose: Draw visible History rows inside the clipped table body.
    // Inputs: `dc` is the target, `table` and `columns_table` describe body geometry, `state` contains rows, and
    // `visible_indices` maps visible rows to history entries. Outputs: Renders visible rows and preserves clipping.
    void draw_history_rows(HDC dc, const RECT& table, const RECT& columns_table, const UiState& state,
                           const std::vector<std::size_t>& visible_indices, int first_visible_row);

    // Purpose: Draw the selected History row details panel.
    // Inputs: `dc` is the target, `details` is the panel rectangle, and `state` contains selection and history data.
    // Outputs: Renders placeholder or selected operation detail text.
    void draw_history_details(HDC dc, const RECT& details, const UiState& state);

    // Purpose: Draw the System page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains backend status.
    // Outputs: Renders HIP status, device metadata, acceleration mode, and monitoring panels.
    void draw_gpu_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the live performance monitor section on the System page.
    // Inputs: `dc` is the target, `monitor` is the panel rectangle, and `sample` is the latest live counter snapshot.
    // Outputs: Renders CPU, RAM, selected-drive total I/O, and total GPU cards with bounded graphs.
    void draw_performance_monitor(HDC dc, const RECT& monitor, const UiState& state);

    // Purpose: Draw one metric card inside the live performance monitor.
    // Inputs: `dc` is the target; text/value fields are preformatted; graph labels describe the graph scale;
    // `history` contains normalized samples. Outputs: Renders a bordered Task Manager-style history graph without
    // overflowing text.
    void draw_performance_monitor_card(HDC dc, const RECT& graph, const wchar_t* label, const std::wstring& value,
                                       const std::wstring& detail, std::span<const double> history, COLORREF color,
                                       const std::wstring& graph_top_label, const std::wstring& graph_bottom_label);

    // Purpose: Draw one dual-line metric card inside the live performance monitor.
    // Inputs: `dc` is the target; graph labels describe scale; `primary_history` and `secondary_history` are normalized
    // samples. Outputs: Renders read/write or similar paired histories with a shared graph scale.
    void draw_dual_performance_monitor_card(HDC dc, const RECT& graph, const wchar_t* label, const std::wstring& value,
                                            const std::wstring& detail, std::span<const double> primary_history,
                                            std::span<const double> secondary_history, COLORREF primary,
                                            COLORREF secondary, const std::wstring& graph_top_label,
                                            const std::wstring& graph_bottom_label);

    // Purpose: Draw a slim operation progress bar under an action button.
    // Inputs: `dc`, `rect`, `state`, and `operation` describe the matching active job.
    // Outputs: Renders only while the requested operation is active.
    void draw_operation_progress_bar(HDC dc, const RECT& rect, const UiState& state, OperationKind operation);

    // Purpose: Draw the Settings page.
    // Inputs: `dc` is the target, `rect` is the content area, and `state` contains toggled defaults.
    // Outputs: Renders general, security, performance, and logging settings.
    void draw_settings_page(HDC dc, const RECT& rect, const UiState& state);

    // Purpose: Draw the about page.
    // Inputs: `dc` is the target and `rect` is the content area.
    // Outputs: Renders product identity, version, and project scope.
    void draw_about_page(HDC dc, const RECT& rect);

    // Purpose: Draw a simple DPI-scaled command or navigation button.
    // Inputs: `dc` is the target, `rect` is the button rectangle, `text` is display text, and `active` selects accent
    // styling. Outputs: Renders the button into `dc`; text is ellipsized rather than overflowing.
    void draw_button(HDC dc, const RECT& rect, const wchar_t* text, bool active);

    // Purpose: Draw a DPI-scaled opt-in settings toggle row.
    // Inputs: `dc` is the target, `rect` is the row rectangle, `text` is display text, and `enabled` selects checked
    // styling. Outputs: Renders the toggle and label into `dc`.
    void draw_toggle(HDC dc, const RECT& rect, const wchar_t* text, bool enabled, ToggleId id);

    // Purpose: Draw a DPI-scaled checkbox row.
    // Inputs: `dc`, `rect`, `text`, `checked`, and `interactive` describe the visual state.
    // Outputs: Renders a checkbox and label into `dc`.
    void draw_checkbox(HDC dc, const RECT& rect, const wchar_t* text, bool checked, bool interactive = true);

    // Purpose: Draw a form field or select-style value box.
    // Inputs: `dc` is the target, `rect` is the box, `label` names the field, `value` is the current display value,
    // `select` adds a menu affordance, `enabled` controls disabled styling, `clickable` enables hover without a menu
    // arrow, and `value_color_override` optionally supplies an exact value color. Outputs: Renders an ellipsized field.
    void draw_field(HDC dc, const RECT& rect, const wchar_t* label, const std::wstring& value, bool select,
                    bool enabled = true, bool clickable = false, COLORREF value_color_override = CLR_INVALID);

    // Purpose: Draw the currently expanded select/dropdown menu.
    // Inputs: `dc` is the target, `content` is the active content area, and `state` is copied UI state.
    // Outputs: Renders an overlay menu above page content when a dropdown is active.
    void draw_active_dropdown(HDC dc, const RECT& content, const UiState& state);

    // Purpose: Return the System monitor card grid used by rendering and hit testing.
    // Inputs: `monitor` is the performance monitor panel rectangle.
    // Outputs: Returns the four equal-width graph card rectangles.
    [[nodiscard]] std::array<RECT, 4> performance_monitor_card_rects(const RECT& monitor) const;

    // Purpose: Return the System refresh-interval field rectangle.
    // Inputs: `monitor` is the performance monitor panel rectangle.
    // Outputs: Returns a narrow rectangle aligned to the GPU card's right edge and monitor header row.
    [[nodiscard]] RECT performance_update_speed_rect(const RECT& monitor) const;

    // Purpose: Return the fixed-drive selector rectangle inside the I/O monitor card.
    // Inputs: `monitor` is the performance monitor panel rectangle.
    // Outputs: Returns a compact dropdown rectangle aligned inside the I/O card header.
    [[nodiscard]] RECT performance_io_drive_rect(const RECT& monitor) const;

    // Purpose: Resolve the owning field rectangle for a dropdown.
    // Inputs: `id` identifies the dropdown and `content` is the current content rectangle.
    // Outputs: Returns the DPI-scaled field rectangle, or an empty rectangle when inactive.
    [[nodiscard]] RECT dropdown_anchor_rect(DropdownId id, const RECT& content) const;

    // Purpose: Resolve the overlay menu rectangle for a dropdown.
    // Inputs: `id` identifies the dropdown and `content` is the current content rectangle.
    // Outputs: Returns a DPI-scaled menu rectangle positioned inside the content area.
    [[nodiscard]] RECT dropdown_menu_rect(DropdownId id, const RECT& content) const;

    // Purpose: Draw the permanent content accent rule.
    // Inputs: `dc` is the target and `rect` is the content area.
    // Outputs: Renders a stable accent line without page-change animation.
    void draw_tab_transition(HDC dc, const RECT& rect);

    // Purpose: Return the current content rectangle used by renderers and hit tests.
    // Inputs: None; reads the HWND client rectangle and DPI-scaled chrome sizes.
    // Outputs: Returns the content area in client physical pixels.
    [[nodiscard]] RECT content_rect() const;

    // Purpose: Handle clicks inside the content area.
    // Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
    // Outputs: Returns true when a setting was changed and a repaint was queued.
    bool handle_content_click(int x, int y);

    // Purpose: Handle mouse activation while the overwrite confirmation modal is active.
    // Inputs: `x` and `y` are client coordinates.
    // Outputs: Consumes every click, continuing or cancelling only when an action button is hit.
    bool handle_extract_overwrite_prompt_click(int x, int y);

    // Purpose: Handle keyboard activation while the overwrite confirmation modal is active.
    // Inputs: `key` is the pressed virtual key.
    // Outputs: Consumes modal navigation, Continue, Cancel, and Escape actions.
    bool handle_extract_overwrite_prompt_key(WPARAM key);

    // Purpose: Handle mouse activation while the license-notice modal is active.
    // Inputs: `x` and `y` are client coordinates.
    // Outputs: Consumes every click and closes only when the Close button is hit.
    bool handle_license_notices_dialog_click(int x, int y);

    // Purpose: Handle keyboard activation and scrolling while the license-notice modal is active.
    // Inputs: `key` is the pressed virtual key.
    // Outputs: Consumes close and scroll keys without leaking focus to the underlying page.
    bool handle_license_notices_dialog_key(WPARAM key);

    // Purpose: Return keyboard-focusable controls for the current page.
    // Inputs: `content` is the current content rectangle and `state` is a copied UI snapshot.
    // Outputs: Returns controls in Tab order using the same geometry as mouse hit testing.
    [[nodiscard]] std::vector<FocusTarget> focus_targets_for(const RECT& content, const UiState& state) const;

    // Purpose: Append all Queue page keyboard-focus targets.
    // Inputs: `targets` receives controls, `content` is the content rectangle, and `state` is a copied UI snapshot.
    // Outputs: Adds Queue action buttons, header tick, and visible row targets.
    void add_queue_focus_targets(std::vector<FocusTarget>& targets, const RECT& content, const UiState& state) const;

    // Purpose: Append all Compress page keyboard-focus targets.
    // Inputs: `targets` receives controls, `content` is the content rectangle, and `state` is a copied UI snapshot.
    // Outputs: Adds command, destination, format, tuning, and toggle targets that are currently enabled.
    void add_compress_focus_targets(std::vector<FocusTarget>& targets, const RECT& content, const UiState& state) const;

    // Purpose: Append all Extract page keyboard-focus targets.
    // Inputs: `targets` receives controls and `content` is the content rectangle.
    // Outputs: Adds command, destination, overwrite, and integrity/security toggles.
    void add_extract_focus_targets(std::vector<FocusTarget>& targets, const RECT& content) const;

    // Purpose: Append all Settings page keyboard-focus targets.
    // Inputs: `targets` receives controls and `content` is the content rectangle.
    // Outputs: Adds Settings buttons, toggles, and dropdowns in tab order.
    void add_settings_focus_targets(std::vector<FocusTarget>& targets, const RECT& content) const;

    // Purpose: Normalize the current keyboard focus to a valid target index.
    // Inputs: `targets` is the current page's focus target list.
    // Outputs: Updates focus index and returns false if no target exists.
    bool normalize_focus_index(const std::vector<FocusTarget>& targets);

    // Purpose: Move keyboard focus by a signed delta through the current page's focus list.
    // Inputs: `delta` is normally +1 or -1.
    // Outputs: Mutates focus index and queues a repaint.
    bool move_keyboard_focus(int delta);

    // Purpose: Activate a focused control with Enter or Space.
    // Inputs: `target` is the current focus target and `key` is the activating virtual key.
    // Outputs: Executes the same command/toggle/dropdown path as mouse activation.
    bool activate_focus_target(const FocusTarget& target, WPARAM key);

    // Purpose: Move within open dropdown options or Queue rows using arrow keys.
    // Inputs: `key` is one of the supported arrow/home/end virtual keys.
    // Outputs: Updates selection or focus and returns true when consumed.
    bool handle_navigation_key(WPARAM key);

    // Purpose: Move keyboard selection inside an expanded dropdown.
    // Inputs: `key` is an arrow/Home/End key, `active` identifies the dropdown, and `state` is a stable UI snapshot.
    // Outputs: Updates dropdown keyboard index and repaints when the dropdown consumes the key.
    bool handle_dropdown_navigation_key(WPARAM key, DropdownId active, const UiState& state);

    // Purpose: Move keyboard focus through visible Queue rows.
    // Inputs: `key` is Up or Down and `state` is a stable UI snapshot.
    // Outputs: Updates selected Queue row, scroll offset, keyboard focus, and repaint state when consumed.
    bool handle_queue_row_navigation_key(WPARAM key, const UiState& state);

    // Purpose: Move keyboard focus through visible History rows.
    // Inputs: `key` is Up or Down and `state` is a stable UI snapshot.
    // Outputs: Updates selected History row, scroll offset, keyboard focus, and repaint state when consumed.
    bool handle_history_row_navigation_key(WPARAM key, const UiState& state);

    // Purpose: Move keyboard focus to the first or last focusable control.
    // Inputs: `key` is Home or End and `state` is a stable UI snapshot.
    // Outputs: Updates keyboard focus and repaints when at least one target exists.
    bool handle_home_end_navigation_key(WPARAM key, const UiState& state);

    // Purpose: Toggle a boolean UI-state member with the standard animated toggle feedback.
    // Inputs: `member` selects the state field and `id` identifies the visual toggle to animate.
    // Outputs: Mutates the state, starts the toggle animation, queues repaint, and returns true.
    bool toggle_bool_setting(bool UiState::* member, ToggleId id);

    // Purpose: Toggle a checkbox-style boolean UI-state member without knob animation.
    // Inputs: `member` selects the state field and `status` is the visible status-line text.
    // Outputs: Mutates the state, writes status text, queues repaint, and returns true.
    bool checkbox_bool_setting(bool UiState::* member, const char* status);

    // Purpose: Compute Queue table column rectangles from the current resizable widths.
    // Inputs: `table` is the visible Queue table rectangle and `row` is the row or header span.
    // Outputs: Returns fixed checkbox and resizable data-column rectangles.
    [[nodiscard]] QueueColumnLayout queue_column_layout(const RECT& table, const RECT& row) const;

    // Purpose: Compute History table column rectangles from the current resizable widths.
    // Inputs: `table` is the visible History table rectangle and `row` is the row or header span.
    // Outputs: Returns resizable Time, Operation, Archive name, Archive path, and Status column rectangles.
    [[nodiscard]] HistoryColumnLayout history_column_layout(const RECT& table, const RECT& row) const;

    // Purpose: Draw Queue page title and queue-management commands.
    // Inputs: `dc` is the paint target, `layout` holds DPI-scaled rectangles, and `state` is the copied UI state.
    // Outputs: Renders the title, optional Remove selected, Add files, Add folder, Clear, and item-count controls.
    void draw_queue_toolbar(HDC dc, const QueueLayout& layout, const UiState& state);

    // Purpose: Draw the fixed Queue table header row.
    // Inputs: `dc` is the paint target, `table`/`columns_table` describe table geometry, and `state` is copied UI
    // state. Outputs: Renders select-all checkbox, column titles, resize separators, and header/body divider.
    void draw_queue_table_header(HDC dc, const RECT& table, const RECT& columns_table, const UiState& state);

    // Purpose: Draw Queue empty-state drag/drop guidance.
    // Inputs: `dc` is the paint target and `table` is the Queue table rectangle.
    // Outputs: Renders centered muted or highlighted drop text without changing queue state.
    void draw_queue_empty_state(HDC dc, const RECT& table);

    // Purpose: Draw the visible Queue body rows below the fixed header.
    // Inputs: `dc` is the paint target, `table`/`columns_table` describe geometry, `state` is copied UI state, and
    // `first_visible_row` is the first queued item to render.
    // Outputs: Renders clipped row backgrounds, row checkboxes, and text columns.
    void draw_queue_table_rows(HDC dc, const RECT& table, const RECT& columns_table, const UiState& state,
                               int first_visible_row);

    // Purpose: Draw the Queue overflow scrollbar when rows exceed visible capacity.
    // Inputs: `dc` is the paint target, `table` is the full table rectangle, `row_count` is the queue size, and
    // `max_scroll` is the largest valid first visible row.
    // Outputs: Renders the body-only scrollbar track and thumb when overflow exists.
    void draw_queue_scrollbar(HDC dc, const RECT& table, std::size_t row_count, int max_scroll);

    // Purpose: Draw the History overflow scrollbar with the Queue scrollbar visual system.
    // Inputs: `dc` is the paint target, `table` is the full table rectangle, `row_count` is the filtered row count, and
    // `max_scroll` is the largest valid first visible row.
    // Outputs: Renders the body-only scrollbar track and thumb when overflow exists.
    void draw_history_scrollbar(HDC dc, const RECT& table, std::size_t row_count, int max_scroll);

    // Purpose: Reserve Queue table body space for an overflow scrollbar when needed.
    // Inputs: `table` is the full table rectangle and `row_count` is the number of queued entries.
    // Outputs: Returns the column layout table with the scrollbar gutter removed only when rows overflow.
    [[nodiscard]] RECT queue_columns_table(const RECT& table, std::size_t row_count) const;

    // Purpose: Reserve History table body space for an overflow scrollbar when needed.
    // Inputs: `table` is the full table rectangle and `row_count` is the number of filtered history entries.
    // Outputs: Returns the column layout table with the scrollbar gutter removed only when rows overflow.
    [[nodiscard]] RECT history_columns_table(const RECT& table, std::size_t row_count) const;

    // Purpose: Count the number of complete Queue rows visible below the fixed header.
    // Inputs: `table` is the full Queue table rectangle.
    // Outputs: Returns a non-negative visible row count.
    [[nodiscard]] int queue_visible_row_count(const RECT& table) const;

    // Purpose: Count the number of complete History rows visible below the fixed header.
    // Inputs: `table` is the full History table rectangle.
    // Outputs: Returns a non-negative visible row count.
    [[nodiscard]] int history_visible_row_count(const RECT& table) const;

    // Purpose: Compute the largest valid first visible Queue row.
    // Inputs: `table` is the full Queue table rectangle and `row_count` is the number of queued entries.
    // Outputs: Returns zero when no scrolling is needed, otherwise the maximum scroll offset.
    [[nodiscard]] int queue_max_scroll_offset(const RECT& table, std::size_t row_count) const;

    // Purpose: Compute the largest valid first visible History row.
    // Inputs: `table` is the full History table rectangle and `row_count` is the filtered row count.
    // Outputs: Returns zero when no scrolling is needed, otherwise the maximum scroll offset.
    [[nodiscard]] int history_max_scroll_offset(const RECT& table, std::size_t row_count) const;

    // Purpose: Clamp Queue scroll state after queue or viewport changes.
    // Inputs: `table` is the visible table and `row_count` is the current queue size.
    // Outputs: Keeps `queue_scroll_first_row_` inside the valid range.
    void clamp_queue_scroll_offset(const RECT& table, std::size_t row_count);

    // Purpose: Clamp History scroll state after filters, history, or viewport changes.
    // Inputs: `table` is the visible table and `row_count` is the filtered history size.
    // Outputs: Keeps `history_scroll_first_row_` inside the valid range.
    void clamp_history_scroll_offset(const RECT& table, std::size_t row_count);

    // Purpose: Return the Queue scrollbar track inside the table body.
    // Inputs: `table` is the full Queue table rectangle.
    // Outputs: Returns a narrow body-only track rectangle.
    [[nodiscard]] RECT queue_scrollbar_track_rect(const RECT& table) const;

    // Purpose: Return the History scrollbar track inside the table body.
    // Inputs: `table` is the full History table rectangle.
    // Outputs: Returns a narrow body-only track rectangle matching Queue.
    [[nodiscard]] RECT history_scrollbar_track_rect(const RECT& table) const;

    // Purpose: Return the Queue scrollbar thumb for the current scroll position.
    // Inputs: `table` is the full table and `row_count` is the number of queued entries.
    // Outputs: Returns an empty rectangle when no scrollbar is required.
    [[nodiscard]] RECT queue_scrollbar_thumb_rect(const RECT& table, std::size_t row_count) const;

    // Purpose: Return the History scrollbar thumb for the current scroll position.
    // Inputs: `table` is the full table and `row_count` is the filtered history entry count.
    // Outputs: Returns an empty rectangle when no scrollbar is required.
    [[nodiscard]] RECT history_scrollbar_thumb_rect(const RECT& table, std::size_t row_count) const;

    // Purpose: Move the Queue table by a row delta.
    // Inputs: `delta_rows` is positive for down and negative for up.
    // Outputs: Mutates the first visible row and queues repaint when the visible range changes.
    bool scroll_queue_rows(int delta_rows);

    // Purpose: Move the History table by a row delta.
    // Inputs: `delta_rows` is positive for down and negative for up.
    // Outputs: Mutates the first visible row and queues repaint when the visible range changes.
    bool scroll_history_rows(int delta_rows);

    // Purpose: Start dragging the Queue scrollbar thumb.
    // Inputs: `y` is the current client mouse y-coordinate and `table`/`row_count` describe the visible table.
    // Outputs: Captures the drag baseline when scrolling is possible.
    void begin_queue_scroll_drag(int y, const RECT& table, std::size_t row_count);

    // Purpose: Start dragging the History scrollbar thumb.
    // Inputs: `y` is the current client mouse y-coordinate and `table`/`row_count` describe the visible table.
    // Outputs: Captures the drag baseline when scrolling is possible.
    void begin_history_scroll_drag(int y, const RECT& table, std::size_t row_count);

    // Purpose: Update Queue scroll position from a scrollbar-thumb drag.
    // Inputs: `y` is the current client mouse y-coordinate.
    // Outputs: Mutates the first visible row and queues repaint when the thumb moves to another row.
    void update_queue_scroll_drag(int y);

    // Purpose: Update History scroll position from a scrollbar-thumb drag.
    // Inputs: `y` is the current client mouse y-coordinate.
    // Outputs: Mutates the first visible row and queues repaint when the thumb moves to another row.
    void update_history_scroll_drag(int y);

    // Purpose: End any active Queue scrollbar drag.
    // Inputs: None.
    // Outputs: Clears scrollbar drag state.
    void end_queue_scroll_drag();

    // Purpose: End any active History scrollbar drag.
    // Inputs: None.
    // Outputs: Clears scrollbar drag state.
    void end_history_scroll_drag();

    // Purpose: Test whether a History row passes the active operation and status filters.
    // Inputs: `state` supplies the filter values and `entry` is one history row.
    // Outputs: Returns true when the row should be visible.
    [[nodiscard]] bool history_entry_matches_filters(const UiState& state, const HistoryEntry& entry) const;

    // Purpose: Build the visible History row index list for the active filters.
    // Inputs: `state` is a stable UI snapshot.
    // Outputs: Returns indexes into `state.history` in display order.
    [[nodiscard]] std::vector<std::size_t> filtered_history_indices(const UiState& state) const;

    // Purpose: Keep queue selection flags aligned with queued paths while mutex is held.
    // Inputs: None; reads and mutates `state_`.
    // Outputs: Adds missing enabled flags, removes stale flags, and normalizes selected index.
    void normalize_queue_selection_locked();

    // Purpose: Append filesystem paths to the Queue through one shared mutation path.
    // Inputs: `paths` are selected or dropped filesystem paths and `status` is the visible status after success.
    // Outputs: Returns the number of nonempty paths appended, updates selection/scroll state, and repaints on success.
    std::size_t append_queued_paths(std::vector<std::filesystem::path> paths, std::string status);

    // Purpose: Test whether a copied Queue state has at least one checked row.
    // Inputs: `state` is a stable UI snapshot with queue paths and checkbox flags.
    // Outputs: Returns true only when at least one queued item is selected for operations.
    [[nodiscard]] bool has_selected_queue_items(const UiState& state) const;

    // Purpose: Toggle every Queue row selection from the header checkbox.
    // Inputs: None; requires at least one queued item to have an effect.
    // Outputs: Enables or disables every queued item and repaints.
    bool toggle_all_queue_items();

    // Purpose: Toggle a single Queue row checkbox.
    // Inputs: `index` is the queue row to mutate.
    // Outputs: Updates row enabled state, focus selection, and repaint status.
    bool toggle_queue_item(std::size_t index);

    // Purpose: Remove every checked Queue row.
    // Inputs: None; reads the current queue and checkbox state.
    // Outputs: Deletes selected queue entries, preserves unchecked entries, resets scroll bounds, and queues repaint.
    bool remove_selected_queue_items();

    // Purpose: Start a Queue column resize drag.
    // Inputs: `separator` identifies the boundary between adjacent resizable columns and `x` is the mouse coordinate.
    // Outputs: Captures baseline column widths for subsequent mouse-move updates.
    void begin_queue_column_resize(int separator, int x);

    // Purpose: Update active Queue column resize drag.
    // Inputs: `x` is the current client mouse coordinate.
    // Outputs: Mutates adjacent data column widths within readable header minimums.
    void update_queue_column_resize(int x);

    // Purpose: End any active Queue column resize drag.
    // Inputs: None.
    // Outputs: Clears resize state and queues a repaint if a drag was active.
    void end_queue_column_resize();

    // Purpose: Start a History column resize drag using the same adjacent-column model as Queue.
    // Inputs: `separator` identifies the boundary between adjacent resizable columns and `x` is the mouse coordinate.
    // Outputs: Captures baseline column widths for subsequent mouse-move updates.
    void begin_history_column_resize(int separator, int x);

    // Purpose: Update active History column resize drag.
    // Inputs: `x` is the current client mouse coordinate.
    // Outputs: Mutates adjacent data column widths within readable header minimums.
    void update_history_column_resize(int x);

    // Purpose: End any active History column resize drag.
    // Inputs: None.
    // Outputs: Clears resize state and queues a repaint if a drag was active.
    void end_history_column_resize();

    // Purpose: Handle Queue page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when a queue control or row selection consumed the click.
    bool handle_queue_click(const RECT& content, int x, int y);

    // Purpose: Handle Compress page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when a compression control consumed the click.
    bool handle_compress_click(const RECT& content, int x, int y);

    // Purpose: Handle Extract page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when an extraction control consumed the click.
    bool handle_extract_click(const RECT& content, int x, int y);

    // Purpose: Handle History page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when a history filter or command consumed the click.
    bool handle_history_click(const RECT& content, int x, int y);

    // Purpose: Handle Security page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when the security review command consumed the click.
    bool handle_security_click(const RECT& content, int x, int y);

    // Purpose: Start a background security verification pass for visible archive settings.
    // Inputs: None; reads queue and opt-in settings from UI state.
    // Outputs: Launches a Verify worker or reports missing queue input.
    void start_security_verify();

    // Purpose: Handle GPU page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when the GPU refresh command consumed the click.
    bool handle_gpu_click(const RECT& content, int x, int y);

    // Purpose: Handle Settings page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when a Settings control consumed the click.
    bool handle_settings_click(const RECT& content, int x, int y);

    // Purpose: Handle About page hit-testing and commands.
    // Inputs: `content` is the active page rectangle and `x`/`y` are client mouse coordinates.
    // Outputs: Returns true when the Licenses command consumed the click.
    bool handle_about_click(const RECT& content, int x, int y);

    // Purpose: Handle a click while a dropdown menu is expanded.
    // Inputs: `x` and `y` are physical-pixel mouse coordinates relative to the client area.
    // Outputs: Returns true when the click was consumed by dropdown close or selection.
    bool handle_active_dropdown_click(int x, int y);

    // Purpose: Open one dropdown menu.
    // Inputs: `id` identifies the dropdown to open.
    // Outputs: Updates UI state and queues a repaint.
    void open_dropdown(DropdownId id);

    // Purpose: Close any expanded dropdown menu.
    // Inputs: None.
    // Outputs: Clears dropdown state and queues a repaint when needed.
    void close_active_dropdown();

    // Purpose: Apply a dropdown option selection.
    // Inputs: `id` identifies the dropdown and `option_index` is the zero-based selected row.
    // Outputs: Mutates the corresponding UI value, closes the menu, and queues a repaint.
    void select_dropdown_option(DropdownId id, int option_index);

    // Purpose: Re-arm the live performance sampling timer after a user setting change.
    // Inputs: `seconds` is clamped by callers to the supported 1-10 second range.
    // Outputs: Sets the Win32 timer interval without creating additional timers.
    void reset_performance_timer(int seconds);

    // Purpose: Change the active application page.
    // Inputs: `page` is the destination page enum value.
    // Outputs: Mutates UI state and queues a repaint.
    void set_page(Page page);

    // Purpose: Open the Windows file picker and append selected files to the queue.
    // Inputs: None; user selection comes from the shell picker or smoke-test environment.
    // Outputs: Mutates queued paths and queues a repaint when files are selected.
    void add_files();

    // Purpose: Open the Windows folder picker and append a selected folder to the queue.
    // Inputs: None; user selection comes from the shell picker or smoke-test environment.
    // Outputs: Mutates queued paths and queues a repaint when a folder is selected.
    void add_folder();

    // Purpose: Clear queued paths.
    // Inputs: None.
    // Outputs: Empties the queue and queues a repaint.
    void clear_queue();

    // Purpose: Clear in-memory operation history.
    // Inputs: None.
    // Outputs: Empties the session history and queues a repaint.
    void clear_history();

    // Purpose: Open the Windows folder picker for destination selection.
    // Inputs: None; user selection comes from the shell dialog or smoke-test environment override.
    // Outputs: Updates the destination directory and queues a repaint when selected.
    void choose_destination();

    // Purpose: Open Windows Explorer with the current user's SuperZip log file selected.
    // Inputs: None; resolves and creates the per-user log file path.
    // Outputs: Launches Explorer or records a warning status when the shell action fails.
    void open_log_file_location();

    // Purpose: Advance the visible compression level selection.
    // Inputs: None; reads and mutates UI compression-level state.
    // Outputs: Queues a repaint with the next compression level.
    void cycle_compression_level();

    // Purpose: Start a background SuperZip compression job from queued paths.
    // Inputs: None; reads queued paths from UI state.
    // Outputs: Launches a worker thread or no-ops when the queue is empty or another worker is running.
    void start_compress();

    // Purpose: Start a background SuperZip extraction job from selected queued archives.
    // Inputs: None; reads queued paths from UI state.
    // Outputs: Launches a worker thread or no-ops when the queue is empty or another worker is running.
    void start_extract();

    // Purpose: Detect whether Ask-before-overwriting needs user confirmation.
    // Inputs: `destination` is the extraction root.
    // Outputs: Returns true when the destination exists and is non-empty or cannot be proven empty.
    [[nodiscard]] bool extract_overwrite_prompt_needed(const std::filesystem::path& destination) const;

    // Purpose: Open the SuperZip-owned overwrite confirmation modal for a pending extract job.
    // Inputs: `request` is the fully captured job request to run if the user continues.
    // Outputs: Stores the pending job, updates modal UI state, clears progress, and queues repaint.
    void show_extract_overwrite_prompt(ExtractJobRequest request);

    // Purpose: Cancel the pending overwrite confirmation.
    // Inputs: None.
    // Outputs: Clears modal and pending job state without starting extraction.
    void cancel_extract_overwrite_prompt();

    // Purpose: Continue the pending extract job with overwrite enabled for this job only.
    // Inputs: None.
    // Outputs: Clears modal state and starts the captured extraction job.
    void continue_extract_overwrite_prompt();

    // Purpose: Launch a captured extraction job on the background worker.
    // Inputs: `request` contains archive, destination, GPU, security, and overwrite choices.
    // Outputs: Starts the worker, updates progress/history/status, and performs pre/post security scans.
    void launch_extract_job(ExtractJobRequest request);

    // Purpose: Run an archive operation on a single background worker.
    // Inputs: `job` is the work closure and `label` is the status text shown while it runs.
    // Outputs: Updates status/history/progress and queues repaints; catches worker exceptions into UI state.
    void run_job(std::function<void()> job, std::string label);

    // Purpose: Publish one active operation progress snapshot to the UI.
    // Inputs: `snapshot` is an immutable worker progress sample.
    // Outputs: Replaces visible progress, cancels any completed-progress hold timer, and queues repaint.
    void publish_progress_snapshot(const ProgressSnapshot& snapshot);

    // Purpose: Keep the last progress sample visible after an operation stops.
    // Inputs: `mark_complete` fills known totals for successful work; caller must hold `mutex_`.
    // Outputs: Arms the 15-second clear timer or clears idle progress.
    void retain_progress_after_stop_locked(bool mark_complete);

    // Purpose: Clear retained progress after its hold interval expires.
    // Inputs: None; reads the synchronized progress hold timestamp.
    // Outputs: Clears visible progress and queues repaint when the deadline has elapsed.
    void clear_expired_progress();

    // Purpose: Append one filtered current-session log entry.
    // Inputs: `severity` is the visible category and `message` is safe session text.
    // Outputs: Adds a bounded in-memory row and queues repaint.
    void append_log_entry(LogSeverity severity, std::string message);

    // Purpose: Add an operation result line to the in-memory session history.
    // Inputs: `line` is a display string and should not include secrets.
    // Outputs: Mutates history state.
    void append_history(const std::string& line);

    // Purpose: Add a structured operation result line to the in-memory session history.
    // Inputs: `operation`, `archive_name`, `archive_path`, `detail`, and `success` describe one completed operation.
    // Outputs: Mutates history state and selects the appended row for the detail panel.
    void append_history_entry(std::string operation, std::string archive_name, std::string archive_path,
                              std::string detail, bool success);

    // Purpose: Refresh visible AMD GPU status text.
    // Inputs: None.
    // Outputs: Mutates GPU status state using `query_gpu_info`.
    void refresh_gpu_status();

    // Purpose: Initialize optional Windows performance counters for live monitoring.
    // Inputs: None; uses the current process and Windows PDH provider.
    // Outputs: Opens best-effort GPU engine counters without failing app startup.
    void initialize_performance_monitor();

    // Purpose: Release optional Windows performance counters.
    // Inputs: None.
    // Outputs: Closes PDH handles and clears monitor state.
    void shutdown_performance_monitor();

    // Purpose: Collect one live performance sample for the System page.
    // Inputs: None; reads process, memory, I/O, optional PDH, and throttled HIP memory state.
    // Outputs: Updates `state_.performance` with CPU, RAM, I/O, GPU utilization, and VRAM values.
    void update_performance_sample();

    // Purpose: Sample total system CPU use since the previous monitor tick.
    // Inputs: `elapsed_seconds` is the interval from the previous sample.
    // Outputs: Returns logical-processor-normalized CPU percentage and updates previous system FILETIME state.
    [[nodiscard]] double sample_system_cpu_percent(double elapsed_seconds);

    // Purpose: Sample SuperZip process CPU use since the previous monitor tick.
    // Inputs: `elapsed_seconds` is the interval from the previous sample.
    // Outputs: Returns total-system-capacity CPU percentage and updates previous process FILETIME state.
    [[nodiscard]] double sample_process_cpu_percent(double elapsed_seconds);

    // Purpose: Sample total selected fixed-drive activity through Windows performance counters.
    // Inputs: `selected_drive_index` identifies the user-selected fixed-drive row.
    // Outputs: Returns drive busy percent plus total read/write byte rates, or unavailable values on counter failure.
    [[nodiscard]] ProcessIoRates sample_selected_drive_io(int selected_drive_index);

    // Purpose: Release selected-drive I/O performance counters after a drive-selection change.
    // Inputs: None.
    // Outputs: Closes disk PDH handles so the next sample binds to the selected drive.
    void reset_disk_performance_monitor();

    // Purpose: Refresh cached HIP VRAM and visible GPU identity at a bounded cadence.
    // Inputs: `now` is the current steady-clock timestamp.
    // Outputs: Updates cached VRAM fields and GPU status text when the throttle permits a HIP query.
    void refresh_gpu_memory_cache(std::chrono::steady_clock::time_point now);

    // Purpose: Publish a completed performance sample into current UI state and graph history.
    // Inputs: `sample` is the counter set and `now` is the sample timestamp.
    // Outputs: Updates visible performance state, ring-buffer graph history, and last-sample time.
    void publish_performance_sample(const PerformanceMonitorSample& sample, std::chrono::steady_clock::time_point now);

    // Purpose: Sample total Windows GPU engine utilization when PDH exposes it.
    // Inputs: None; uses initialized PDH wildcard counters.
    // Outputs: Returns total system GPU percentage or a negative value when unavailable.
    [[nodiscard]] double sample_gpu_utilization();

    // Purpose: Sample Windows GPU dedicated memory assigned to the SuperZip process.
    // Inputs: None; uses the current process id and Windows PDH GPU Process Memory counters.
    // Outputs: Returns dedicated GPU memory bytes or zero when the counter is unavailable.
    [[nodiscard]] std::uint64_t sample_process_dedicated_vram_bytes() const;

    // Purpose: Sample total Windows dedicated GPU memory currently used by all processes/adapters.
    // Inputs: None; uses Windows PDH GPU Adapter Memory counters without mutating app state.
    // Outputs: Returns dedicated VRAM usage bytes, or zero when Windows does not expose the counter.
    [[nodiscard]] std::uint64_t sample_total_dedicated_vram_used_bytes() const;

    // Purpose: Start a bounded non-blocking page transition animation.
    // Inputs: `from` and `to` identify the tab change.
    // Outputs: Arms the animation timer and queues repaint frames.
    void start_page_transition(Page from, Page to);

    // Purpose: Start a bounded non-blocking toggle animation.
    // Inputs: `id` identifies the toggle and `from`/`to` are logical states.
    // Outputs: Arms the animation timer and queues repaint frames.
    void start_toggle_animation(ToggleId id, bool from, bool to);

    // Purpose: Start a bounded non-blocking command-button release animation.
    // Inputs: `point` is the client-coordinate release position after a primary mouse click.
    // Outputs: Records the release pulse origin and arms the animation timer.
    void start_button_release_animation(POINT point);

    // Purpose: Return normalized active page-transition progress.
    // Inputs: None; reads the steady animation clock.
    // Outputs: Returns 1.0 when no page transition is active.
    [[nodiscard]] double page_transition_progress() const;

    // Purpose: Return a toggle's visual knob position.
    // Inputs: `id` identifies the toggle and `enabled` is the final logical state.
    // Outputs: Returns a normalized knob position from 0.0 to 1.0.
    [[nodiscard]] double toggle_visual_position(ToggleId id, bool enabled) const;

    // Purpose: Return normalized command-button release animation progress.
    // Inputs: `rect` is the button rectangle being rendered.
    // Outputs: Returns 1.0 when no release pulse applies to this button.
    [[nodiscard]] double button_release_progress(const RECT& rect) const;

    // Purpose: Advance active UI animations.
    // Inputs: None.
    // Outputs: Queues another repaint while animation is active or stops the timer.
    void tick_animation();

    // Purpose: Recreate native fonts at the current monitor DPI.
    // Inputs: None; reads `dpi_`.
    // Outputs: Replaces owned GDI font handles.
    void rebuild_fonts();

    // Purpose: Coalesce paint requests from worker and UI threads.
    // Inputs: None.
    // Outputs: Posts at most one pending repaint message until the UI thread processes it.
    void request_repaint();

    // Purpose: Scale design-space pixels to current physical pixels.
    // Inputs: `value` is a 96-DPI design coordinate or size.
    // Outputs: Returns the nearest Win32 `MulDiv` scaled integer for crisp layout.
    [[nodiscard]] int scale(int value) const;

    HWND hwnd_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT body_font_ = nullptr;
    HFONT small_font_ = nullptr;
    HFONT tiny_font_ = nullptr;
    HFONT mono_font_ = nullptr;
    UINT dpi_ = 96;
    std::mutex mutex_;
    UiState state_;
    std::thread worker_;
    std::atomic_bool worker_running_ = false;
    std::atomic_bool repaint_queued_ = false;
    bool window_was_minimized_ = false;
    ULONG_PTR gdiplus_token_ = 0;
    Page transition_from_page_ = Page::Queue;
    Page transition_to_page_ = Page::Queue;
    std::chrono::steady_clock::time_point page_transition_start_{};
    std::chrono::steady_clock::time_point toggle_transition_start_{};
    std::chrono::steady_clock::time_point button_release_start_{};
    std::chrono::steady_clock::time_point last_performance_sample_time_{};
    std::chrono::steady_clock::time_point last_gpu_memory_sample_time_{};
    ToggleId transition_toggle_ = ToggleId::None;
    bool transition_toggle_from_ = false;
    bool transition_toggle_to_ = false;
    std::array<int, 4> queue_column_widths_{276, 100, 96, 468};
    std::array<int, 4> queue_column_resize_start_{276, 100, 96, 468};
    int queue_column_resize_separator_ = -1;
    int queue_column_resize_start_x_ = 0;
    std::array<int, 5> history_column_widths_{112, 100, 300, 410, 102};
    std::array<int, 5> history_column_resize_start_{112, 100, 300, 410, 102};
    int history_column_resize_separator_ = -1;
    int history_column_resize_start_x_ = 0;
    int queue_scroll_first_row_ = 0;
    int queue_scroll_drag_start_y_ = 0;
    int queue_scroll_drag_start_offset_ = 0;
    int queue_wheel_delta_remainder_ = 0;
    bool queue_scroll_dragging_ = false;
    int history_scroll_first_row_ = 0;
    int history_scroll_drag_start_y_ = 0;
    int history_scroll_drag_start_offset_ = 0;
    int history_wheel_delta_remainder_ = 0;
    bool history_scroll_dragging_ = false;
    int license_notices_scroll_drag_start_y_ = 0;
    int license_notices_scroll_drag_start_offset_ = 0;
    int license_notices_scroll_drag_content_height_ = 0;
    bool license_notices_scroll_dragging_ = false;
    std::array<PerformanceMonitorSample, 96> performance_history_{};
    std::size_t performance_history_count_ = 0;
    std::size_t performance_history_next_ = 0;
    POINT mouse_position_{-1, -1};
    POINT button_release_point_{-1, -1};
    bool mouse_inside_client_ = false;
    bool primary_mouse_down_ = false;
    bool mouse_tracking_ = false;
    bool mouse_capture_active_ = false;
    bool queue_drop_highlight_ = false;
    RECT text_tooltip_cell_{};
    POINT text_tooltip_anchor_point_{-1, -1};
    bool text_tooltip_cell_active_ = false;
    bool text_tooltip_visible_ = false;
    std::wstring text_tooltip_text_;
    std::array<std::wstring, 2> cached_license_notices_text_;
    int keyboard_focus_index_ = 0;
    int dropdown_keyboard_index_ = -1;
    int modal_focus_index_ = 1;
    int license_notices_tab_index_ = 0;
    int license_notices_scroll_pixels_ = 0;
    ExtractJobRequest pending_extract_job_{};
    bool pending_extract_job_active_ = false;
    bool ole_initialized_ = false;
    QueueDropTarget* drop_target_ = nullptr;
    FILETIME last_process_kernel_time_{};
    FILETIME last_process_user_time_{};
    FILETIME last_system_idle_time_{};
    FILETIME last_system_kernel_time_{};
    FILETIME last_system_user_time_{};
    std::uint64_t cached_vram_total_bytes_ = 0;
    std::uint64_t cached_vram_free_bytes_ = 0;
    std::wstring last_clock_text_;
    PDH_HQUERY gpu_query_ = nullptr;
    PDH_HCOUNTER gpu_counter_ = nullptr;
    PDH_HQUERY disk_query_ = nullptr;
    PDH_HCOUNTER disk_busy_counter_ = nullptr;
    PDH_HCOUNTER disk_read_counter_ = nullptr;
    PDH_HCOUNTER disk_write_counter_ = nullptr;
    std::wstring disk_counter_instance_;
    AppSettings applied_settings_{};
};

}  // namespace superzip::app
