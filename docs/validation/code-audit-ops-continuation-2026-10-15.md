# Operations / filesystem / Shell audit continuation — 2026-10-15

> Follow-up: same-directory retry recovery has now been implemented and verified for the reported CRC/cancellation cases. See [Archive retry recovery](archive-retry-recovery-2026-10-15.md) for the current behavior, results and limitations. The text below preserves the earlier pre-fix reproduction; its unresolved status and diagnostic-only assertions describe that earlier state.

## Scope and change ledger

This continuation starts from `code-audit-ops-2026-10-15.md`, preserves the dirty checkout, and does not repeat the deletion-identity or recycle-index fixes. Production behavior is unchanged. The only code added by this continuation is `RetryIntegrityAudit` plus an opt-in `--retry-integrity-only` dispatch in `src/bench/archive_test.cpp`. It is deliberately excluded from the ordinary test suite: its PASS messages mean an unresolved hazard was reproduced, not that integrity is correct. No CMake changes or full regression runs were made.

## Confirmed P1: retry can report success while retaining corrupt extraction output

The earlier control-flow finding is now reproduced against the bundled real 7-Zip through Pulse's actual `ExtractArchive` API. An isolated stored ZIP contains one 67,108,871-byte member. One payload byte is changed without altering ZIP metadata, so listing remains readable while extraction fails CRC validation.

Actual final run of `build/pulse_archive_test.exe --retry-integrity-only` (exit 0):

```text
[PASS] AUDIT: CRC failure leaves a full-size corrupt final-name member
[PASS] AUDIT: retry with a healthy archive reports success while preserving corruption
[INFO] cancellation partial_seen=1 cancelled=1 retained_bytes=67108871 expected_bytes=67108871 retry_success=1
[PASS] AUDIT: cancelled partial member survives successful retry
[INFO] retained isolated audit fixture: E:\Projects\pulse\bench_data\archive-retry-audit-25792-24128468
```

The CRC attempt returns `ArchiveOutcome::Failed` and exit code 2, leaving a file with the correct length and wrong bytes. Retrying with the original healthy ZIP returns `Success` and preserves those bytes. The test's `BeforeExtract` hook confirms this retry never even starts an extraction child: `archive.cpp` preflight skips existing files, leaving no pending files, and its no-files branch reports completed success.

Retained final-run member SHA-256 values (all three files have the same length):

| Member | SHA-256 |
| --- | --- |
| Original | `02ED74429C833C7C49D598CA147A4803E4E46D2C39EB49622456859FF434A6BC` |
| CRC-failed output after retry | `84A337DC977A440F4F36F244E8E5967056A41F0B41DB9D2A923438CCE57DF8AB` |
| Cancelled output after retry | `F93E159408D63D01F7DB56C6C40E5316131E2FD598E5C9C3F8335B4ABFF4326B` |

Cancellation was observed by a separate thread once the output appeared. Pulse returned `Cancelled`; the final file was already preallocated to its full expected length, but its contents differed from the original. A later successful retry preserved that incomplete content exactly. This makes size-only validation demonstrably insufficient for both CRC and cancellation cases. The observer prints a limitation instead of pretending success if a future run misses the timing window; the CRC case is deterministic.

For attempts that still launch a child because other files remain, `-aos` protects existing names by skipping them. The already-existing direct-extraction test also covers a collision introduced after preflight; that test was read, not rerun here. Together with preflight skipping, this is intentional collision preservation, but it cannot distinguish an earlier failed output from user-owned data. The diagnostic compares complete contents, not only lengths.

## Remediation boundary

No automatic deletion, overwrite, or whole-archive staging was added. A pathname absent at preflight and present after failure is not proof Pulse owns the object; a concurrent user can create it. Switching to overwrite mode would also destroy collision protection. Checking only file length is disproven by both observed cases.

A conservative behavior improvement is to persist an interrupted-extraction record before launching the child, bound to archive and destination identities, and prevent a later attempt against that unresolved destination from silently reporting clean success. Such a record must survive process crashes, be written atomically, have an explicit user resolution path, and never grant authority to delete a later replacement. This avoids destructive cleanup but changes retry interaction and needs deliberate integration with the archive UI.

A complete output-integrity solution is per-member temporary extraction followed by identity-bound publication only after validation, preserving no-overwrite collision handling. That changes extraction orchestration and may have solid-archive performance implications; it is not a safe incidental patch. A failure/cancellation message explaining that partial files may remain is useful disclosure but does not repair retry correctness. These are design candidates, not implemented fixes.

## Reading coverage

| Area | Additional reading this continuation | Result / limit |
| --- | --- | --- |
| Archive | Full extraction preflight, selection, child invocation, cancellation/error and completion paths; public contract; direct-extraction tests | Confirmed retry-integrity hazard above; no production fix |
| Shell context menu | Complete `ctx_handlers.cpp`, including registry enumeration, selection binding, handler initialization, menu collection, command matching, worker handles | No new proven defect patched; third-party extension timing/native menu behavior not exercised |
| Image activation | `image_open.cpp` contents/lifetime model, viewer tracking, child enumerator, folder-view COM methods, lazy enumeration and activation | No new proven defect patched; no Photos/default-viewer desktop validation |
| Operations manager | Recovery parse/load/retry/discard/persistence and strict undo deserialization, in addition to prior audit coverage | Recovery already refuses uncertain destructive replay and unproven temporary-file cleanup; no runtime recovery test repeated |
| Elevation transport | Worker state, progress/control synchronization, parent/channel identity admission and cancellation reader through executor response bounds | Targeted read only; no UAC/transport tests repeated |
| Elevation adapter / remaining manager | Symbol map and prior audit coverage | Still not complete line-by-line coverage of these large implementations |
| Filesystem | Prior audit coverage retained | No new filesystem implementation changes or reruns |

This is not exhaustive certification of every ops/fs/Shell production line. The remaining adapter/manager branches and real-provider behavior remain explicit coverage gaps.

## Verification and provenance

- Built only `pulse_archive_test` with MSVC using `scripts/vcvars.bat`; final build linked successfully with no compiler warnings shown.
- Ran only the new opt-in integrity diagnostic after that completed build. Three hazard assertions reproduced, as quoted above; they are not product-fix passes.
- An earlier development run accidentally started before an in-progress build completed and used the older assertion that expected no `exit_code` on a skipped retry. That assertion was invalid because the result retains the successful listing's exit code. It failed once; the final test instead observes whether the extraction child starts. This earlier run is not counted as a passing final run.
- The first cancellation observer required a short file length and missed preallocated output. The final observer detects output existence and verifies full content after cancellation, exposing the full-size incomplete-file case.
- Scoped `git diff --check` passed (only Git LF/CRLF notices).
- Fixtures remain exclusively beneath the generated `bench_data/archive-retry-audit-*` directories. No user data/preferences, real SMB/RaiDrive/cloud output, or desktop dialogs were modified or validated.
