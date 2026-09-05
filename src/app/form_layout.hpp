#pragma once

#include "app/main_window_layout.hpp"

namespace superzip::app {

// Purpose: Lay out compression controls without window or device queries.
// Inputs: rect excludes shell chrome; dpi is positive; client bounds are at least 960x600 DIPs.
// Outputs: Returns default or compact form rectangles with unchanged font and command sizes.
[[nodiscard]] CompressLayout make_compress_layout(const RECT& rect, UINT dpi);

// Purpose: Lay out extraction controls with readable narrower-column proportions.
// Inputs: rect excludes shell chrome; dpi is positive; client bounds are at least 960x600 DIPs.
// Outputs: Returns rectangles shared by painting, focus, and pointer input.
[[nodiscard]] ExtractLayout make_extract_layout(const RECT& rect, UINT dpi);

// Purpose: Lay out settings controls without overlapping selectors and commands.
// Inputs: rect excludes shell chrome; dpi is positive; client bounds are at least 960x600 DIPs.
// Outputs: Returns default or balanced narrow-column rectangles without changing settings behavior.
[[nodiscard]] SettingsLayout make_settings_layout(const RECT& rect, UINT dpi);

}  // namespace superzip::app
