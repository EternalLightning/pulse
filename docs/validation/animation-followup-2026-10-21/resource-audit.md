# Animation scheduling audit, 2026-10-21

Scope: app_main WM_PAINT/WM_TIMER, app_ui_schedule, ui_compositor,
notification_toast and file_operation_dialog. Existing uncommitted work retained.

## Independently reproduced findings

1. Continuously invalidating WM_PAINT can starve generated WM_TIMER messages.
   `paint_timer_probe.cs` models the production BeginPaint / paced work /
   EndPaint / InvalidateRect loop, with a 100 ms timer and 800 ms measurement.
   Without explicit timer materialization: 51 paints, 0 timers, 804 ms.
   With PeekMessage(WM_TIMER, PM_NOREMOVE) before InvalidateRect: 52 paints,
   7 timers, first timer at 117 ms, 801 ms total.
   This is an OS scheduling reproduction, not an instrumented full Pulse run.
   The main window originally lacked the fairness step already present in the
   operation window; the root agent owns the main-window correction.

2. GetTickCount64 sampling repeated at high frame rates on this machine.
   `animation_clock_probe.cs`, one-second paced sampling:
   60 Hz: 61 samples, 0 repeated adjacent ticks.
   120 Hz: 120 samples, 55 repeated adjacent ticks.
   165 Hz: 165 samples, 99 repeated adjacent ticks; nonzero increments 15/16 ms.
   First 160 ms at 165 Hz: 25 samples, 14 repeated ticks.
   This establishes animation-clock quantization, not GPU/display frame rate.
   The operation completion reveal now uses the shared steady_clock helper.

Reproduction commands, from the project root in PowerShell:

    Add-Type -Path bench_data/animation_audit_20261021/paint_timer_probe.cs
    [PaintTimerProbe]::Run($false)
    [PaintTimerProbe]::Run($true)
    Add-Type -Path bench_data/animation_audit_20261021/animation_clock_probe.cs
    [AnimationClockProbe]::Run(60)
    [AnimationClockProbe]::Run(120)
    [AnimationClockProbe]::Run(165)

## Operation-window changes

- `src/ui/file_operation_dialog.cpp:34`: compare all fields consumed by the
  operation presentation before invalidating on status Update. The main
  maintenance loop submits unchanged status every 100 ms, so changing only
  the child-window timer would not eliminate settled-window redraws.
- `src/ui/file_operation_dialog.cpp:45`: paused, waiting and terminal states
  do not append idle speed samples or continue indeterminate animation.
- `src/ui/file_operation_dialog.cpp:1668`: timer redraws only for pending
  animation, device recovery, or a new sample in an expanded speed graph.
  Active transfers still collect history while the graph is collapsed.
- Completion reveal uses AnimationTimeMs at hide, settings, render and minimize
  boundaries; test-controlled reveal samples use that same clock.
- The child 100 ms timer remains installed. This removes redundant drawing
  and speed-history mutation, not every timer wakeup.
- The 600 ms completion auto-hide remains in the unchanged main maintenance
  path (`src/app/app_ops_ui.cpp`, completed-status branch). Inspected but not
  exercised end-to-end by the standalone operation-window fixture.

## Actual verification

Built only `pulse_file_operation_failure_test`; no compiler warnings.

- `--resource-only`: 22 PASS, 0 FAIL. Tests real window WM_TIMER/Update
  invalidation and history behavior across completed, failed, cancelled,
  paused and both waiting states, changed content, collapsed/expanded active
  transfer graph and hidden window.
- `--completion-only`: 20 PASS, 0 FAIL. Includes real paint-loop advance,
  settled endpoint, repeated status, reduced-motion behavior and hide/show.
- `--motion-only`: 11 PASS, 0 FAIL. Preparation measurement: 247 frames in
  1.5 seconds, 164.7 FPS, median gap 6.06 ms, p95 gap 6.58 ms, 18 timer messages.
- Visually inspected operation-completion/settled-light-96.png and
  operation-completion/settled-dark-168.png; no clipping or overlaps observed.
- `git diff --check` on owned source changes passed.

No full application build, full historical regression, installed deployment,
whole-process CPU/RSS measurement or user-profile mutation was performed.
