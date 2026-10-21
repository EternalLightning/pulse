# Window size lock — 2026-10-21

Added a persistent, default-off “锁定窗口大小 / Lock window size” switch immediately below the background window-size matching action. Enabling it captures each main window's current normal size, disables matching, rejects border/corner resizing and the system Size command, and constrains normal window-size requests. Moving, minimizing and maximizing remain available; restoring returns to the locked size. Enabling while maximized captures the normal restore size. Disabling immediately restores resizing and matching.

Implementation uses the existing settings effects to capture/reset each window's lock immediately. Existing preference and settings-control infrastructure handles persistence, localization, hit testing and layout.

## Verification

Built the affected production and fixture targets:

```powershell
cmake --build build --target pulse_file_selection_ui_test pulse
cmake --build build --target pulse_file_selection_ui_test
.\build\pulse_file_selection_ui_test.exe --window-size-lock
```

The final fixture ran outside the restricted sandbox with isolated state and preference persistence disabled: **53 PASS, 0 FAIL**. Log: `bench_data/window-size-lock-ui/result.log`.

Checks call the production main-window handlers and settings-click path, covering:

- Border/corner hit tests, system Size and direct size requests; position changes remain allowed.
- Maximize/restore, minimizing from normal/maximized states, enabling while maximized and fixed pixel size across a DPI-change message.
- Preferences JSON roundtrip, old-file default and reset behavior.
- Actual switch clicks, disabled match hit target and direct-dispatch guard with a valid image, then match re-enabling after unlock.
- Chinese/English settings layout at 100% light/wide and 150% dark/narrow, with four compositor screenshots visually reviewed for clipping, overlap and switch visibility.

An initial fixture run exposed the maximized-enable boundary before the immediate settings-effect capture was linked into the test. The final rebuilt fixture passes that case and the subsequent DPI check. The fixture's missing accent initialization was also corrected before final screenshot review.

Screenshots: `bench_data/window-size-lock-ui/{zh-CN,en-US}-{100,150}.png`.

`git diff --check` found no whitespace errors. No full regression suite, installed application replacement, manual Windows snap-shortcut test or physical multiple-monitor DPI test was performed. Existing unrelated working-tree changes were preserved.
