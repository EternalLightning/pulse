# Archive retry recovery — 2026-10-15

## Implemented behavior

This supersedes the unresolved retry hazard in `code-audit-ops-continuation-2026-10-15.md`. The original reproduction remains historical evidence, not a current passing integrity result.

Retry uses the same destination. Only confirmed residual files from an incomplete extraction are removed and recreated; unrelated files and preexisting same-name archive members retain the existing skip behavior. No change-directory prompt or whole-destination overwrite was introduced.

`src/ops/archive_retry.h` owns a hidden `.pulse-extract-state` journal. It binds snapshots to the destination directory identity and normalized absolute source archive path. The archive may be repaired or replaced at the same source path. Another archive path cannot consume the unresolved record. Pending members are flushed before the extractor starts. Complete newline-delimited snapshots survive an incomplete trailing append; concurrent journal writers are rejected.

7-Zip verbose output identifies reported members. Handles prevent deletion while held, and failure/cancellation records file identity, size, last-write time and change time. Retry validates all selected residuals before removal by handle. Changed, replaced, unreported, hardlinked or reparse outputs do not acquire overwrite authority. Matching readonly residuals are supported. Existing destination directory guards remain active. Empty valid journals are removed; foreign empty markers are rejected. The reserved journal name is rejected in archive contents.

`src/ops/archive.cpp` also drains the remaining progress pipe after child exit before sealing residual metadata. Extraction still uses `-aos`, preserving collisions unrelated to recovery. `src/ops/archive.h` documents the contract; focused regression cases live in `src/bench/archive_test.cpp`.

## Actual verification

Completed build and focused recovery command (exit 0, no compiler warnings shown):

```bat
cmd.exe /d /s /c "call scripts\vcvars.bat && cmake --build build --target pulse_archive_test pulse && build\pulse_archive_test.exe --retry-recovery-only"
```

| Check | Result | Evidence scope |
| --- | --- | --- |
| `--retry-recovery-only` | 20 PASS, exit 0 | Real bundled 7-Zip CRC failure and cancellation, exact repaired content, unrelated and preexisting same-name files preserved, readonly residual, ownership changes, journal exclusivity, foreign empty marker, torn tail, different source path |
| `--direct-only` | 19 PASS, exit 0 with normal permissions | Existing extraction, progress, destination replacement/pinning, readonly/hardlink/late collisions, reparse rejection, cancellation, CRC failure, selection isolation/tampering/concurrency |
| `--password-only` | 22 PASS, exit 0 | Encrypted and unencrypted extraction, diagnostic classification, successful content, cancelled listing; rerun because process output handling changed |
| `--outcome-only` | 9 PASS, exit 0 | Exit-code classification, machine encryption evidence, incomplete listing, adapter rejection and deletion-record rejection |

The direct suite initially failed sandbox fixture prerequisites for directory replacement/hardlink/junction cases and stopped early. The same executable passed with normal permissions; the initial failures are not counted as passing tests.

An earlier 64 MiB cancellation fixture missed the observation window and printed `[LIMIT]`. The final 128 MiB + 7 byte fixture and yielding observer reproduced full-size incorrect output and verified complete repair:

```text
partial_seen=1 cancelled=1 retained_bytes=134217735 expected_bytes=134217735 retry_success=1
[PASS] retry repairs cancelled residual completely
```

Retained fixture: `E:\Projects\pulse\bench_data\archive-retry-audit-16060-3489000`. Password follow-up fixture: `E:\Projects\pulse\bench_data\archive-password-9220-3703218`. Fixtures are isolated beneath `bench_data`; no user preferences or user archives were changed.

## Limits and remaining work

- An abnormal process exit may leave outputs without confirmed final ownership metadata. These files are preserved and automatic overwrite stops with an error. This is conservative refusal, not automatic crash recovery for every output.
- Ownership evidence combines extractor reports and subsequently opened file handles; this is not an atomic filesystem-creation provenance guarantee.
- Journal reads are limited to 32 MiB and individual snapshot writes to 16 MiB. Very large or repeatedly failing operations have not been exhaustively exercised against these bounds or JSON parser bounds.
- Cancellation reproduction is timing-sensitive; the deterministic CRC case independently verifies the original full-size corrupt-file hazard.
- No full archive suite, full selftest, real SMB/RaiDrive/cloud-provider test, or GUI interaction/visual acceptance was run. Main-program compilation is not GUI functional acceptance.
- This closes the tested retry-corruption path, not the entire repository audit or every remaining audit finding.
