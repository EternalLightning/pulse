# Interaction and OneDrive follow-up — 2026-10-30

## Changes

- Ctrl-wheel view changes now carry a direct-manipulation presentation policy from the input handler through the tab and pane model. Each step updates immediately and cancels an in-progress mode fade. Menu changes retain their existing transition; folder navigation remains animated even after wheel use. This avoids repeated full-pane exits during continuous wheel input; it is not a continuous icon-size interpolation implementation.
- Selected backgrounds and their blue markers share a 120 ms opacity transition in details/list and icon views. Native selection changes immediately. Outgoing selection fades independently, zebra fill follows selection opacity, and first-painted selections appear at their actual initial state. Tracks cover only visible rows and are discarded with the existing frame pruning. Row keys include the source index to avoid alias collisions.
- Caret blinking uses per-editor activity time instead of the global tick phase. Movement/text animation stays visible and receives a full visible blink interval when it settles. Blink repaint timers are scheduled to that phase's next edge. Normal idle blinking, system-disabled blinking, mouse capture/release, focus, IME and native fallback remain supported.
- Cloud sync status now requests only `System.StorageProviderState` from `GPS_FASTPROPERTIESONLY`; missing or failed fast data stays unknown, with no fallback to ordinary file properties. Cloud placeholder type names use a dummy extension and `SHGFI_USEFILEATTRIBUTES`, never the actual cloud item. The details worker forwards already-read attributes; known-placeholder requests skip another path attribute lookup.

## OneDrive source / build / running-instance evidence

The earlier thumbnail/icon/preview protection is present in the same checkout. Observed before this follow-up build:

- `thumbnail_cache.cpp`: 2026-10-29 20:03:57; `shell_icons.cpp`: 20:02:00; `preview_host/main.cpp`: 20:04:43.
- Existing `Pulse.Preview.exe`: 20:05:10; `Pulse.Shell.exe`: 20:03:06; `Pulse.exe`: 22:25:57.
- At the initial investigation, both `E:\Projects\pulse\build-release\Pulse.exe` (PID 32652, started 23:24:45) and `D:\Applications\Pulse\Pulse.exe` (PID 48952, started 21:32:59) were running. The screenshot does not identify which instance produced it. Later only the installed process remained.

This is not evidence of a missing checkout merge. Existing guards and build times are consistent with the earlier fix being incorporated in the preview, while two further automatic property-query routes remained. The current build includes the additional guards. File hashes/times for current sources, production helpers, preview executable and installed executable are in `artifact-identities.json`; the preview and installed executable hashes differ. `processes-after.json` and `original-guard-object-times.json` retain additional observations. No installed payload was replaced.

Microsoft documents the fast store as avoiding disk/network reads and permits missing values: [GETPROPERTYSTOREFLAGS](https://learn.microsoft.com/en-us/windows/win32/api/propsys/ne-propsys-getpropertystoreflags). Extension/attribute-only type lookup semantics: [SHGetFileInfoW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow).

## Actual verification

- Release build succeeded for `pulse`, `pulse_pane_animation_application_test`, `pulse_child_edit_test`, `pulse_cloud_features_test`, `pulse_shell_query_test` and their required helpers. Final log: `build-release/interaction-followup-final-build.log`, no compiler warnings/errors. The initial build exposed a missing `<shellapi.h>` in the new metadata test; it was added and the final build passed. Initial diagnostics remain in `interaction-followup-build.log`.
- `--interaction-feedback-only`: **180 PASS, 0 FAIL**, exit 0, 32 PNGs. Covers selection entrance/exit/reversal, reduced motion, rapid direct wheel view changes, subsequent folder navigation, menu transitions, and the actual Ctrl-wheel handler plus pane projection.
- `--motion`: **273 PASS, 0 FAIL**, exit 0, 48 PNGs. Includes deterministic per-field blink timing and actual caret-pixel checks during movement, settling, hidden/visible periods, typing, capture/release and refocus, across light/dark backgrounds and 100%/150% scale.
- Existing child-editor module: **153 PASS, 0 FAIL**, exit 0, covering dragging, selection, scrolling, undo and native/backend fallback.
- `--sync-probe-only`: **6 PASS, 0 FAIL**, exit 0. COM fakes assert the fast-only flag and absence of ordinary-property fallback on failures/missing values.
- `--cloud-metadata-only`: **9 PASS, 0 FAIL**, exit 0. Production type-name code resolves file/directory labels for each recall flag, including pinned + recall, without any corresponding real file content.
- Total: **621 passing checks**, 80 captured frames. Selected UI and caret frames were visually reviewed in `ui-contact.png` and `caret-contact.png`. No unrelated historical suite was run.

## Limits and reproduction

No real OneDrive file content was opened and no hydration/download was intentionally triggered. The metadata tests verify call policy and missing-file behavior, not an end-to-end OneDrive provider session. Therefore the observed download symptom is not proven eliminated on the user's machine; close all older Pulse windows and retest with the newly built preview. The default installed executable remains a separate, older payload.

Run `python docs/validation/interaction-followup-20261029/run-checks.py`; optional case names are `ui`, `edit`, `probe`, `metadata`. The runner rejects an existing `C:\Feedback` path before using its synthetic image names. The existing child-editor module is run separately. Evidence logs are in each case subdirectory and `existing-child-edit.log`.

Preview entry: `docs/validation/navigation-motion-20261029/preview.cmd`.
