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
- The applied `Show operation summary` preference is captured when an archive
  job starts. After success, failure, or cancellation, it opens History with
  that job's latest primary result selected and visible, clearing filters that
  could hide it. Auxiliary hash/scan rows remain available without replacing
  the primary result. Disabling the preference preserves the current page;
  recording results in History is independent of automatic presentation.

## Compact Layout Foundation

Compress, Extract, and Settings share pure layout helpers with their hit tests.
Compress reflows its lower controls into two columns below 730 client DIPs;
Extract and Settings balance their columns below 1086 client DIPs. Native font
and control sizes are retained. Geometry tests cover 448 size/DPI combinations
per form, including both sides of the reflow boundaries and 100%-300% DPI.

Dropdowns fit complete rows inside the content viewport. Overflow uses bounded
wheel scrolling and arrow bands; keyboard navigation reveals its selected row.
Every create-format option remains selectable. Metric cards measure wrapped
detail text with the selected native font before reserving graph space; sample
counts, timing, and graph-series positioning logic are unchanged.

GUI smoke captures all eight pages at both the normal 1200-by-760-DIP client
size and a programmatically resized 960-by-600-DIP test size at the host's
actual DPI. It verifies popup selection through wheel, arrows, and keyboard,
saves real settings, and restores the pre-test settings snapshot. The fixed-style
window now chooses its size from the monitor work area, within these tested
minimum/preferred dimensions. This does not enable unrestricted user resizing
or establish support for untested monitor configurations; see `docs/portability.md`.

## Queue Metadata

Queue size/type cells and Extract/Verify candidate selection use display-only
metadata snapshots. `QueueMetadataCache` performs filesystem status and size
queries on one lazy background-priority thread, never during painting or
button eligibility checks. Archive jobs still open and validate their sources
independently; a cached file classification is not authority to extract it.

Membership is bounded by the 4,096-row Queue limit. Repeated requests for the
same path share one pending read. Removed rows lose pending work, and an
in-flight result cannot populate a later row with the same path. Existing
snapshots remain visible during refresh; pending rows show `...`, and missing
or inaccessible metadata is explicit. A requested snapshot becomes eligible
for refresh two seconds after completion. This is not a promise of two-second
filesystem latency: a refresh requires another display request, and shutdown
joins any current OS metadata query.

File type changes invalidate the separate bounded folder-size task. Recursive
folder totals retain their existing queue-lifetime caching behavior; changes
deep inside a folder are not a live filesystem-watch feature. Tests cover
blocked reads, deduplication, removal/re-addition, failures and retry, real
file resize/delete/recreation, and candidate ordering/ticks/type filtering.
These are responsiveness and lifecycle checks, not codec speed benchmarks.

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
total system CPU usage, total used system RAM, selected fixed-drive total I/O
activity, and total system GPU engine utilization. Current values must remain
visible alongside graphs; a graph without the live number is a regression. The
I/O card includes a compact fixed-drive selector and excludes optical,
removable, network, unknown, and other non-fixed volumes.

The runtime, acceleration policy, and device architecture occupy a compact,
unframed status band. The monitor follows immediately below, with individual
metric cards but no enclosing decorative panel. Rendering and dropdown anchors
share `src/app/system_layout.hpp`; geometry tests cover 100%-250% DPI and
multiple content bounds. Those rectangle tests are not proof of full-app
small-screen support: the fixed-window limitation in `docs/portability.md`
still applies, and screenshots must be inspected at the actual tested DPI.

Sampling is deliberately bounded. The refresh-interval dropdown offers exactly
1, 3, 5, and 10 seconds, maps directly to the Win32 timer interval, and re-arms
the timer when changed. CPU values use total-system and process-dedicated
counters. I/O uses Windows PDH `LogicalDisk` counters for the selected fixed
drive: the graph and headline value show `% Disk Time`, while the detail rows
show total read and write byte rates. GPU engine utilization uses Windows PDH
when available; VRAM uses throttled HIP device queries so the monitor does not
create device chatter or interfere with compression.

Rendering must stay native, crisp, and low-overhead: no blurred backgrounds, no
bitmap graph assets, no unbounded animations, no full-window transparency, and
no polling faster than the selected interval. `tools\gui_smoke.ps1` must keep
opening the refresh-interval dropdown, capture the expanded menu, and assert that
the performance monitor region contains the expected graph color families.
Smoke also selects every refresh interval, requires a newly appended command
result, and saves the non-default interval through Settings to verify the
persisted value. The fixed-drive selection must report the drive actually
chosen. Screenshot color variation alone does not prove a dropdown was opened
or its selection reached the application.
