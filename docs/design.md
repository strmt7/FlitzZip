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
left navigation, queue/work surface, persistent GPU status, Security Review,
History, AMD GPU, and Preferences pages. It keeps rendering native rather than
using bitmap UI assets, so the app remains crisp at 4K+ and high DPI.

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
