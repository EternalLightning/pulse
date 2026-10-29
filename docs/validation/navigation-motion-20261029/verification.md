# Navigation and interaction motion — 2026-10-29

- Entire content pane: 200 ms fade-through and 16 px (at 100% DPI) directional travel, including home, banners, pane title, filters and list. Parent navigation reverses direction. Short enumeration retains the previous visual for up to 120 ms; late results receive a new reveal.
- Settings entry/exit: 240 ms fade-through and 18 px travel; return reverses direction. Settings category content uses 170 ms and 6 px travel. Navigation selected background and accent marker share a single moving rectangle; icon colors follow the selection weights. The marker has no independent trailing/stretch animation.
- Visible file rows use independent 110 ms hover entrance and 150 ms exit, including rapid pointer movement. Tracks are discarded when rows leave the painted view.
- System-disabled motion and high contrast use immediate states. The existing animation scheduler includes pending surface/hover transitions and stops requesting frames after settling.
- Drawing-command snapshots retain visible surfaces rather than directory models. Layout size changes and compositor replacement discard old surfaces. New and old text use separate fade phases to avoid double-exposed labels.

## Actual verification

- Built `pulse` and `pulse_pane_animation_application_test` in `build-release`, Release, selftests disabled. Final build log has no compiler warnings/errors.
- Ran only `--navigation-motion-only`: **244 PASS, 0 FAIL**, exit 0. The fixture covers initial/settled states, row hover changes, folder loading/arrival, rapid parent return, warning-banner page entry/exit, home entry/exit, settings entry/exit, category switching/reversal, and disabling motion mid-transition. Banner/home regression checks inspect the pane transition directly so unrelated hover frames cannot mask missing navigation motion.
- Captured 44 production-renderer PNGs across light/dark themes and 100%/150% scale. Visually inspected the contact sheet, including banner, home and category intermediate frames. No title double exposure remains in the reviewed frames.
- `check-marker.py`: 8 PASS. Pixel checks verify the accent marker remains inside the moving selected rectangle, with padding, during category changes and reversals. The first checker assumed the raw input accent RGB; rendered themes adjust that color, so detection now identifies the blue marker in its narrow left-edge region while retaining the geometric containment assertions.
- Isolated fixture data and preferences; no complete historical regression suite was run. Checks exercise the renderer with fixture models, rather than physical mouse input or real slow network enumeration. Live Shell preview child-window transitions and other DPI/theme combinations were not validated.

Reproduce with `python docs/validation/navigation-motion-20261029/run-fixture.py` after building the fixture target with `PULSE_BUILD_ARCHITECTURE_FIXTURES=ON`.

Root cause of the missing transitions: list-only transition bounds changed whenever a banner changed the list origin, clearing the previous surface; home explicitly reset that surface and returned through a separate draw branch. The transition now wraps the whole stable pane viewport in `DrawPane`.

Evidence: `tests.log`, `snapshots/`, `contact.png`, `marker-checks.log`; final build log: `build-release/navigation-surfaces-build.log`.

Interactive preview: `preview.cmd` launches the build with an isolated data directory. Automatic launch from the tool environment created a process but no visible window was observed; that process was stopped. Interactive startup remains unverified.

For this follow-up, the user's running old preview process was preserved. Its in-use executable was renamed to `build-release/Pulse.before-navigation-surfaces-20261029-211835.exe` before linking the new `Pulse.exe`. Close the old preview and launch `preview.cmd` to inspect the updated behavior.
