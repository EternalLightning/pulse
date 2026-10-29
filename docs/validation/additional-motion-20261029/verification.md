# Additional UI motion — 2026-10-29

## Behavior

- Details/preview panel: 220 ms movement and opacity. The panel keeps its full preferred layout width as it slides through the right window edge; the file list reserves the current animated width. Closing replays the last panel drawing, disables its interactions immediately, and restores the file area gradually. Rapid reversal samples the current position. Resizing, DPI changes, narrow layouts and reduced motion settle directly.
- Native Shell preview overlays are enabled only after panel movement settles; closing requests their hiding. This prevents an independently hosted preview window from being laid out against a moving/offscreen panel.
- Details/list/icon mode changes at the same path use the shared 200 ms content fade-through. The mode is tracked independently of the path, so changing views does not disturb parent-navigation direction. Existing selection/data models are reused.
- Same-page InfoBars expand/collapse over 180 ms. Outgoing text is retained until collapse finishes, while navigation initializes the destination banner immediately under the existing whole-page transition. Content centers within the expanding region and reveals smoothly after there is room for a complete text line. Completely transparent/collapsed actions reject input. List origin, scrollbar hover and button hit testing use the same current geometry. The changes page retains its multiline explanation layout.
- Tooltips fade in over 100 ms and out over 120 ms. Exit retains the text and anchor, both reversal directions remain continuous, different targets replace the retained label, and dragging/popovers suppress the visual. Background, border and glyphs share opacity. Pending work uses the existing animation scheduler.

## Actual verification

- Built Release `pulse` and `pulse_pane_animation_application_test`; production selftests remain disabled. Final log: `build-release/additional-motion-final-build.log`, no compiler warnings/errors.
- `--additional-motion-only`: **229 PASS, 0 FAIL**, exit 0. Includes banner and tooltip state tests, panel intermediate/full widths, reversal and termination, actual panel boundary hit testing, closing interaction suppression, reduced motion/narrow layouts, view-mode reversal, banner geometry/visibility hit tests, application `ListRect` during entrance/removal/collapse, and tooltip scheduling/placement.
- Captured **40** production-renderer PNGs across light/dark themes at 100%/150% scale. Reviewed selected opening/closing, view-switching, revealing-banner and tooltip intermediate frames in `contact.png`.
- Shared navigation dependency: reran only `--navigation-motion-only`: **244 PASS, 0 FAIL**, exit 0, with 44 snapshots in the existing navigation-motion validation folder. No unrelated historical regression suite was run.
- First pass identified one tooltip reversal failure (also counted by its aggregate assertion), preserved in `tests-first-pass.log`. The active curve was being resampled with the new direction's duration. The implementation now samples using the previous target duration; the original assertion is retained and the opposite reversal is also checked. First-pass banner snapshots also exposed harsh midline clipping; final drawing and hit testing share a reveal gate, with added checks for transparent and visible phases.

## Limits and reproduction

The fixtures use isolated models and synthetic inputs. Live third-party Shell/Office providers, physical pointer/keyboard interaction and other DPI combinations were not exercised. The panel's native-preview deferral is implemented but requires interactive provider verification. Renderer hit testing and the application's list-rectangle projection were exercised directly.

Build the fixture with `PULSE_BUILD_ARCHITECTURE_FIXTURES=ON`, then run `python docs/validation/additional-motion-20261029/run-fixture.py`. Evidence: `tests.log`, `snapshots/`, `contact.png` and `tests-first-pass.log`.

Close an older preview instance and open `docs/validation/navigation-motion-20261029/preview.cmd` for the updated `build-release/Pulse.exe`. No installation/package/publishing operation is part of this preview work.
