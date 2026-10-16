# Operations / filesystem / Shell audit — 2026-10-15

This audit preserves the existing dirty checkout. Changes from this audit are limited to `src/ops/delete_operation.cpp`, `src/ops/clipboard.cpp`, `src/fs/fs_recycle.cpp`, and focused cases in `src/bench/recycle_backend_test.cpp`. No CMake changes were made by this work package.

## Fixed findings

### P1: deletion confirmation did not retain the confirmed object's identity

`RunDelete` formerly published the permanent-delete confirmation before opening its `FileObjectReference`s. Ordinary first attempts also sent an empty `expected_identities` vector. Renaming the selected object away and creating another object at the same path while confirmation was pending therefore left the replacement eligible for deletion.

The operation now retains references before publishing confirmation, checks those references again before dispatch, and sends their identities to the existing Shell identity checks. Retries retain their original references, and undo identities are checked against history before admission. An object that cannot be identified is refused. This protects the confirmation window; it is not a claim that third-party Shell providers provide an atomic identity-based delete primitive.

The regression creates only isolated `bench_data/delete-confirmation-*` files, changes identity during the pending confirmation, checks that both objects survive, then submits a new request and verifies that an unchanged confirmed object can still be deleted.

### P2: truncated recycle metadata was accepted as a shortened restore path

`ParseIndexBytes` formerly reduced a version-2 index's declared character count when the record was shorter than declared. The resulting shortened path was exposed as `original_path` to enumeration and restoration. It now rejects truncated records and embedded NULs rather than manufacturing a different destination. A valid index remains accepted.

### P3: clipboard format registration failure leaked allocated memory

`WriteClipboard` and `CompleteCutClipboard` allocated an HGLOBAL before testing whether `RegisterClipboardFormatW` succeeded. A zero format ID skipped both the ownership transfer and cleanup. Allocation now occurs only after registration succeeds. This is a small failure-path correction; no clipboard-failure injection was performed.

## Actual verification

- Built `pulse_recycle_backend_test` and its required `pulse_shell` dependency with MSVC via `scripts/vcvars.bat`; no compiler warnings/errors were reported for these changes.
- `pulse_recycle_backend_test --index-validation`: 3/3 PASS (valid index, truncated index, embedded NUL).
- `pulse_recycle_backend_test --confirmation-identity`: 5/5 PASS outside the sandbox using the same executable. The first sandbox run passed the identity-rejection checks but failed the positive backend deletion with `shell_client pipe CreateFile err=5`; the outside-sandbox rerun passed the positive backend check too. This distinction is retained rather than describing the failed run as passing.
- Scoped `git diff --check` passed, with only Git's existing LF/CRLF notices.
- `clipboard.cpp` is not linked into the recycle backend test; the integrating agent's main `pulse` build is needed to cover its compilation.

Real SMB, RaiDrive, cloud-provider identities, UAC desktop interaction, and manually observed native dialogs were not validated by these fixtures. Providers that cannot supply identity are now refused before delete confirmation; compatibility on those providers is unverified.

## Unresolved archive integrity / interaction risk

`archive.h` explicitly documents direct extraction and partial outputs on cancellation/failure. `archive.cpp` passes the final destination to 7-Zip, terminates the child on cancellation, and returns without removing unfinished final-name files. Its next preflight skips existing non-directory outputs, and `-aos` also skips them. Consequently, a retry can complete successfully while an earlier partial or corrupt output remains. “Success” currently means the selected extraction attempt completed under skip-existing semantics, not that every destination matches archive contents.

This is confirmed from control flow and the documented contract, not from a large-archive interruption experiment in this audit. Reproduction to add: extract a large stored member to an isolated directory, cancel after the member exists but before completion, restart extraction into the same directory, then compare hash/length of the surviving output against the original. A CRC-failing archive is another relevant failure case; file size alone cannot establish completeness.

Safe remediation needs a deliberate commit strategy: stage each output and publish only complete files, or persist identity-bound incomplete-output records and make retry handle them explicitly. Blindly deleting every path that appeared since preflight can delete concurrently created user data. Restoring whole-archive staging also changes the recent direct-extraction design and its disk-space/performance behavior. No such redesign was silently applied here.

## Review coverage and limits

Detailed reads covered: `archive.cpp/.h`, `archive_progress.h`, `copy_progress_report.h`, `transfer_rate.h`, `clipboard.cpp`, `delete_operation.cpp/.h`, `delete_service.cpp/.h`, `file_object_reference.h`, `produced_undo.h`, `recycle_undo_validation.cpp/.h`, `drop_staging.cpp`, `fs_enum.cpp`, `fs_watch.cpp`, `fs_net_cache.cpp`, `fs_recycle.cpp`, `path_error.h`, `shell_launch_process.cpp`, `ctx_session_process.cpp`, `shell_interaction_worker.cpp`, `recycle_fileop.cpp`, `deletion_identity.cpp`, `shell_query_worker.cpp`, `shell_query_metadata.cpp`, and `shell_query_icons.cpp`.

Targeted reads covered: `destination_guard.h` capture/lease/rename paths; `ops_manager.cpp` current diff, startup/shutdown, operation dispatch symbol map, transfer commit/copy/cancellation; `operation_permission.cpp`; `operation_elevation_transport.cpp` diff, process authentication and cancellation transport; `operation_elevation_adapter.cpp` diff/helper selection; `operation_presentation.h`; `file_lock_owner.cpp` lock probing and process termination; `fs_snapshot.cpp`; `shell_host/main.cpp` changed progress and worker entry plus authorized deletion setup/dispatch; `shell_launch_worker.cpp` owner validation and Shell/native-window launch entry points. `image_open.cpp`, `ctx_handlers.cpp`, and larger remaining manager/elevation implementations received symbol/entry-point scans only, not complete line-by-line review.

Related tests read: `recycle_backend_test.cpp` deletion identity/confirmation and focused dispatch, `archive_test.cpp` progress/direct-extraction cancellation cases, and target definitions in CMake. This work package does not claim that every production line or all historical tests have been reviewed or run. The unreviewed portions require follow-up if exhaustive coverage is a release gate.

## Simplification candidates

Recycle index decoding exists both in `fs_recycle.cpp` and the stricter `ExactOriginal` implementation in `shell_host/recycle_fileop.cpp`; sharing a bounded pure decoder would prevent validation drift, but coupling those modules was not necessary for this fix. Small local process/handle wrappers are repeated across Shell worker files; consolidation is optional and was not mixed into the safety changes.
