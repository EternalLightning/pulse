# Text input motion — 2026-10-29

Follow-up caret-blink fixes and the expanded 48-frame fixture are recorded in `../interaction-followup-20261029/verification.md`; the results below describe the original implementation.

## Behavior

- Custom child EDIT surfaces share an 80 ms caret-position transition. Retargeting samples the current position before changing direction. Text, native selection, undo and IME locations update immediately; the animation changes only the painted caret.
- Typed/inserted glyphs reveal over 100 ms. The unchanged prefix stays in place. Existing suffix text moves as insertion opens a gap or deletion closes it. Inserted glyphs are clipped to the available gap to prevent overlap with the moving suffix.
- Text ranges retain DirectWrite shaping and expand the changed span to cluster boundaries. UTF-16 surrogate pairs are not divided. Wrapped rename text moves within a line; segments crossing lines reveal at their destination rather than moving through other lines.
- IME composition, selection presentation, complex bidirectional text, disabled system motion and high contrast retain immediate positions as appropriate. Programmatic full text replacement, size/font changes and focus loss reset transient motion. IME candidate placement follows the logical caret.
- A child-window timer requests frames only while movement/reveal is pending. It stops on settling, focus loss, hiding, destruction or native backend policy/fallback. Native caret blinking remains in place; the custom caret stays visible during movement.

## Verification

- Release targets: `pulse` and `pulse_child_edit_test`, production selftests disabled. Build log: `build-release/edit-motion-final-build.log`.
- `pulse_child_edit_test --motion`: 130 PASS, 0 FAIL, exit 0; 24 production input-renderer PNGs across light/dark backgrounds at 100% and 150% scale. Includes deterministic caret reversal/settling, Unicode diff boundaries, actual native EDIT mutations, insertion/deletion, immediate selection/IME handling, disabled motion, wrapped fields, backend policy cancellation and automatic timer completion after rapid typing.
- Existing `pulse_child_edit_test` module: 153 PASS, 0 FAIL, exit 0. Covers selection, dragging, viewport stability, Unicode input, undo, focus, backend switching and presentation fallback. No unrelated historical suite was run.
- `check-pixels.py`: 4 PASS; unchanged prefix pixels match the initial frame during insertion. Reviewed enlarged input snapshots for caret movement, insertion, deletion and wrapped text; corrected the initial suffix/glyph overlap before final capture.
- IME verification uses composition messages and committed Unicode text in isolated fields; a real interactive IME/candidate session was not exercised. Other scale factors and full application interaction remain manual preview checks. Text input tests preserve the foreground window and use synthetic text only.

Reproduce with `python docs/validation/edit-motion-20261029/run-fixture.py`, then `python docs/validation/edit-motion-20261029/check-pixels.py`.

Preview: close an older build and use `docs/validation/navigation-motion-20261029/preview.cmd`. It opens `build-release/Pulse.exe` with isolated test data. Try arrow keys, typing in the middle of a filename, backspace and a wrapped rename field.
