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

The current Win32 implementation follows iteration 4 at the structural level:
left navigation, queue/work surface, persistent GPU status, Security Review,
History, AMD GPU, and Preferences pages. It keeps rendering native rather than
using bitmap UI assets, so the app remains crisp at 4K+ and high DPI.

Iteration 4 is the active acceptance reference for GUI work. Future changes must
preserve the compact enterprise shell, command bar, icon rail, page-specific
forms/tables, bottom GPU status strip, and explicit opt-in security controls.
Visual changes are not complete until `tools/gui_smoke.ps1 -Configuration
Release` captures every page and those screenshots are reviewed against the
reference direction.

## Interaction Rules

- Settings that can change security behavior are explicit toggles.
- Microsoft Defender scanning and SHA-256 hashing remain off until the user opts
  in.
- Overwrite remains off by default.
- Work starts on a background thread and updates progress through coalesced
  repaint requests.
- Text is ellipsized or wrapped instead of overflowing.
