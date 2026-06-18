# Design Notes

SuperZip's interface is intentionally dense, calm, and operational. The app is
not a landing page: the first screen is the queue and job surface.

## Iterations

Generated design boards are stored under `resources/design/`:

- `superzip-ui-iteration-1.png`: first main-screen concept.
- `superzip-ui-iteration-2.png`: expanded multi-page shell.
- `superzip-ui-iteration-3.png`: security and integrity options added.
- `superzip-ui-iteration-4.png`: final reference direction with cleaner labels
  and a simpler enterprise layout.
- `superzip-ui-imagegen-polish-20260615.png`: imagegen-assisted polish pass for
  shared page-grid alignment, Queue-local actions, and symmetric rail icons.

The current Win32 implementation follows iteration 4 at the structural level:
left navigation, queue/work surface, persistent GPU status, Security,
History, System, and Settings pages. It keeps rendering native rather than
using bitmap UI assets, so the app remains crisp at 4K+ and high DPI.

## Brand Source Of Truth

`resources/brand/superzip-logo.svg` is the single canonical SuperZip logo
source. The app icon and in-app stacked mark must be generated from the SVG
geometry instead of being redrawn independently:

- `tools/generate_app_icon.ps1` renders `resources/app/superzip.ico` from the
  canonical SVG.
- CMake runs `tools/generate_brand_logo_header.ps1` to generate the Win32
  vector-geometry header used by the app renderer.
- `tools/verify_brand_assets.ps1` checks that the SVG has the canonical mark,
  the icon is current, and the app renderer depends on generated geometry.

Do not add new logo bitmaps or alternate hand-drawn marks. Any visual change to
the mark starts in the SVG and is regenerated through the tooling.

Iteration 4 plus the 2026-06-15 polish reference are the active acceptance
references for GUI work. Future changes must preserve the compact enterprise
shell, brand-only top bar, icon rail, page-specific forms/tables, Queue-local
Add files/Add folder/Clear actions, bottom GPU status strip, and explicit opt-in
security controls. Visual changes are not complete until `tools/gui_smoke.ps1
-Configuration Release` captures every page and those screenshots are reviewed
against the reference direction.

## Interaction Rules

- Settings that can change security behavior are explicit toggles.
- Microsoft Defender scanning and SHA-256 hashing remain off until the user opts
  in.
- Overwrite remains off by default.
- Work starts on a background thread and updates progress through coalesced
  repaint requests.
- Text is ellipsized or wrapped instead of overflowing.

## Performance Monitor Contract

References checked on 2026-06-18:

- Windows Task Manager overview:
  <https://learn.microsoft.com/en-us/shows/inside/task-manager>
- Windows Performance Counters:
  <https://learn.microsoft.com/en-us/windows/win32/perfctrs/performance-counters-portal>
- PDH sampling guidance:
  <https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data>
- DWM drawing best practices:
  <https://learn.microsoft.com/en-us/windows/win32/dwm/bestpractices-ovw>
- Double-buffering rationale:
  <https://learn.microsoft.com/en-us/dotnet/desktop/winforms/advanced/how-to-reduce-graphics-flicker-with-double-buffering-for-forms-and-controls>

The System page uses Task Manager-style history cards: a current value, short
explanatory detail, a subtle grid, scale/time labels, and a bounded
ring-buffer graph with a filled trend plus a crisp line. The visible cards are
CPU usage, total used system RAM, process read/write I/O, and used GPU VRAM.
Current values must remain visible alongside graphs; a graph without the live
number is a regression.

Sampling is deliberately bounded. The update-speed dropdown offers exactly
1-10 seconds, maps directly to the Win32 timer interval, and re-arms the timer
when changed. CPU and process I/O rates are interval-based and need at least two
samples before they show meaningful nonzero rates. GPU engine utilization uses
Windows PDH when available; VRAM uses throttled HIP device queries so the
monitor does not create device chatter or interfere with compression.

Rendering must stay native, crisp, and low-overhead: no blurred backgrounds, no
bitmap graph assets, no unbounded animations, no full-window transparency, and
no polling faster than the selected interval. `tools\gui_smoke.ps1` must keep
opening the update-speed dropdown, capture the expanded menu, and assert that
the performance monitor region contains the expected graph color families.
