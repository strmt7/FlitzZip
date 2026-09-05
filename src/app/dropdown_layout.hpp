#pragma once

#include <algorithm>
#include <windows.h>

namespace superzip::app {

// Purpose: Share bounded popup geometry; inputs: physical client pixels; outputs: rows plus optional scroll arrows.
struct DropdownLayout {
    RECT menu{};
    RECT up{};
    RECT down{};
    int row_top = 0;
    int row_height = 0;
    int visible_rows = 0;
    int first_row = 0;
    int max_first_row = 0;
};

// Purpose: Fit a native-size dropdown into the content viewport, scrolling when all options cannot fit.
// Inputs: anchor/content are valid client rectangles; dpi is positive; count is bounded menu size; first is requested
// row. Outputs: Returns contained whole rows and scroll-arrow bands, or empty geometry when no row can fit.
inline DropdownLayout make_dropdown_layout(const RECT& anchor, const RECT& content, UINT dpi, int count, int first) {
    DropdownLayout layout{};
    if (count <= 0 || anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        return layout;
    }
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int border = scale(1);
    const int available = content.bottom - content.top - scale(16);
    layout.row_height = std::max(1, scale(count > 10 ? 28 : 32));
    const bool scrolling = count > (available - scale(2)) / layout.row_height;
    const int band = scrolling ? scale(20) : 0;
    layout.visible_rows = std::min(count, std::max(0, (available - scale(2) - 2 * band) / layout.row_height));
    if (layout.visible_rows == 0) {
        return DropdownLayout{};
    }
    const int height = layout.visible_rows * layout.row_height + scale(2) + 2 * band;
    int top = anchor.bottom + scale(4);
    if (top + height > content.bottom - scale(8)) {
        top = anchor.top - scale(4) - height;
    }
    top =
        std::clamp(top, static_cast<int>(content.top) + scale(8), static_cast<int>(content.bottom) - scale(8) - height);
    layout.menu = {anchor.left, top, anchor.right, top + height};
    layout.row_top = top + border + band;
    layout.max_first_row = count - layout.visible_rows;
    layout.first_row = std::clamp(first, 0, layout.max_first_row);
    if (scrolling) {
        layout.up = {anchor.left + border, top + border, anchor.right - border, layout.row_top};
        layout.down = {layout.up.left, layout.row_top + layout.visible_rows * layout.row_height, layout.up.right,
                       layout.menu.bottom - (scale(2) - border)};
    }
    return layout;
}

// Purpose: Map a pointer to a visible option; inputs: shared layout and physical coordinates; outputs: index or -1.
inline int dropdown_option_at(const DropdownLayout& layout, int x, int y) {
    if (x <= layout.menu.left || x >= layout.menu.right || y < layout.row_top || layout.row_height <= 0) {
        return -1;
    }
    const int row = (y - layout.row_top) / layout.row_height;
    return row < layout.visible_rows ? layout.first_row + row : -1;
}

// Purpose: Keep keyboard selection visible; inputs: layout and selected option; outputs: bounded first row.
inline int dropdown_first_row_for_selection(const DropdownLayout& layout, int selected) {
    if (layout.visible_rows == 0) {
        return 0;
    }
    const int first =
        selected < layout.first_row ? selected : std::max(layout.first_row, selected - layout.visible_rows + 1);
    return std::clamp(first, 0, layout.max_first_row);
}

}  // namespace superzip::app
