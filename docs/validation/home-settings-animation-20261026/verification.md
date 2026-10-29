# Home groups and settings switches — 2026-10-26

## Changes

- Home Libraries, Drives and Network locations use 200 ms ease-out transitions for content height/opacity and chevron rotation. Following headings and scroll extents follow the current expansion. Retracting cards and the network notice stop accepting navigation clicks.
- Settings switches use 160 ms ease-out transitions for knob position and track/border/knob colors. This includes general/search settings, built-in context-menu switches, system-context rows/details and the diagnostic performance switch.
- Transitions sample their previous trajectory before reversing. Initial values appear immediately; disabled system motion settles immediately. The existing frame scheduler stops when the transitions settle. Settings tracks reset on page exit/change and discard controls no longer drawn.

## Actual verification

- Built `pulse` in `build_audit_msbuild_20261025`, Release. The final incremental build has no compiler warnings/errors. Existing working-tree changes were preserved.
- Built the normal CMake target `pulse_pane_animation_application_test`, Release; its final build has no compiler warnings/errors.
- Ran only `--home-settings-only`: **193 PASS, 0 FAIL**, exit 0. No complete historical suite was run.
- Checked transition initialization, zero-elapsed frame scheduling, reversal continuity, settling and disabled motion; all three groups across 300/760/1200 DIP widths at 100/150/200% DPI; intermediate layout extents, non-overlap and retracting-card/notice hit protection; production home click/render scheduling and high-contrast settling; settings renderer scheduling and page-exit cleanup.
- Captured 36 actual production-renderer PNG frames in light/dark themes at 100/150% DPI. Reviewed home intermediate/folded frames and switch off/opening/on/closing crops. The tested switch is scrolled into view. Intermediate home content is intentionally clipped to the animated reveal area; headings remain outside it.

## Reproduction and evidence

```powershell
cmake --build build_audit_msbuild_20261025 --config Release --target pulse -- /m:1 /nodeReuse:false
cmake --build build_audit_msbuild_20261025 --config Release --target pulse_pane_animation_application_test -- /m:1 /nodeReuse:false
python bench_data/home-settings-verification/run-fixture.py
```

- Assertion log: `bench_data/home-settings-verification/tests.log`.
- PNGs: `bench_data/home-settings-verification/snapshots/`.
- Switch comparison: `bench_data/home-settings-verification/switch-review.png`.
- Final build logs: `bench_data/home-settings-build-final.log`, `bench_data/home-settings-standard-test-build-final.log`.

The native UI fixture could not create/write output files in this execution environment, including a temporary output directory. The test therefore uses optional `PULSE_TEST_SNAPSHOT_STDOUT=1`: it reads the actual D2D target, encodes PNGs in memory and sends them to the launcher, which validates and saves them. Normal fixture use still supports `Compositor::SaveSnapshot`. PNG encoding was moved after the intermediate sample so capture time cannot consume the animation before that sample. Initial failed capture/timing logs remain in `bench_data/`.

Frames use synthetic cards and isolated in-memory settings. Visual DPI coverage is 100/150%; 200% was checked for layout/hit geometry. The installed Pulse payload was not replaced; no installer, release publication or push was requested.
