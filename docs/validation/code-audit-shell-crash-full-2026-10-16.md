# Shell client and crash reporter full source audit — 2026-10-16

Full assigned-file source review followed by two narrowly authorized repairs. Existing dirty changes were preserved. The five assigned files had no working-tree diff against HEAD before these repairs, so the findings below describe existing defects rather than regressions introduced by the earlier dirty diff. Targeted original-source red and repaired-source green comparisons completed successfully.

## Findings

### SC-1 — P2: A helper that exits before its pipe is connected prevents recovery for the rest of the client lifetime

Location: `src/ipc/shell_client.cpp:179-185`, with the spawn gate at line 152 and reader retry at lines 418-427.

`SpawnChild` publishes `child_started_ = true` before the first connection. If the child exits before publishing its named pipe, `EnsureConnected` detects the signaled process and returns false without clearing that flag or retiring the process handles. There is no published connection, so `Submit` also skips `HandleDisconnect`. Subsequent reader retries and requests skip `SpawnChild` because `child_started_` remains true, then detect the same dead process again. A transient helper startup crash therefore disables Shell launch, menu queries and Shell-backed creation/deletion until Stop/Start or application restart.

Repair implemented: close/reset failed startup process state before returning, allowing the existing spawn backoff to control later fresh attempts. A regression case in `src/bench/shell_launch_client_test.cpp` starts a test-owned copy of that executable with `--exit-before-pipe`, waits for its exit, seeds the private client state and verifies retirement plus the next attempt's backoff. `--startup-recovery` selects this case. It deliberately does not launch a production helper or touch installed applications; it validates retirement/retry eligibility rather than a complete subsequent successful handshake.

### SC-2 — P2: Failure reporting reads mutable connection error text without synchronization

Location: `src/ipc/shell_client.cpp:258-260`, `413-415`, `592-593`; writers include lines 167, 174, 184, 188 and `SpawnChild`.

`last_error_` is assigned or cleared while `send_mutex_` is held. `Submit` and `HandleDisconnect` release that lock before calling `UnreachableMessage`, which reads the string and returns a reference to it. Meanwhile the reader's retry loop can acquire `send_mutex_` and assign the string in another failed `EnsureConnected`, or another request can reconnect and clear it. The callback then copies a string that can be concurrently modified, producing a C++ data race and possible invalid memory access during failure reporting. The existing dead-child retry loop in SC-1 provides a concrete repeated writer.

Repair implemented: copy the selected failure message to an owned local string while holding `send_mutex_`, and dispatch callbacks after unlocking using that snapshot. Failed replay requests each retain their own message. The private message helper now returns a value and documents its locking requirement. The deterministic `FailureMessageSnapshot` case queues a failed mutation followed by an unavailable read-only retry: the first callback acquires `send_mutex_` and replaces the connection error, then the retry callback must still receive the original error snapshot. This tests error ownership and unlocked callbacks without a flaky thread race or a production-only test hook; the C++ data-race diagnosis itself rests on the lock audit.

## Exact assigned-file coverage

| File | Lines read | Review focus |
| --- | ---: | --- |
| `src/ipc/shell_client.cpp` | 1-596, complete | Child startup and retirement; connection ownership; stop/cancel/abort; frame deadlines; request registration; non-replay policy; callback routing; menu session close; reply parsing and disconnect retry |
| `src/ipc/shell_client.h` | 1-135, complete | Public threading contract; pending/reply maps; lock domains; shared connection lifetime; HWND encoding and private authorized deletion API |
| `src/ipc/ctx_menu_util.h` | 1-373, complete | Built-in filtering; menu text cleanup; flyout flattening and limits; registry flags; catalog normalization; handler keys; grouping/classification |
| `src/common/crash_reporter.cpp` | 1-380, complete | Initialization and shutdown; paths and retention; crash-loop detection; dump/JSON/fallback writing; handler reentrancy; breadcrumb publication; fatal/recoverable/terminate paths |
| `src/common/crash_reporter.h` | 1-36, complete | Role/config declarations and noexcept API contract |

Additional targeted call-site/fixture inspection: `src/ops/ops_manager.cpp` Shell callback setup and forwarding/worker shutdown (approximately 1610-1760); `src/shell_host/main.cpp` cancellation/query/invoke/close dispatch and session startup/message routing (approximately 625-705 and 1338-1403); `src/bench/shell_launch_client_test.cpp` connection preparation and failed-write/disconnect/transport cases (approximately 335-478); `src/bench/crash_reporter_test.cpp` complete. Repository-wide symbol searches checked production breadcrumb call sites and ShellClient ownership references. AGENTS.md was read.

## Validation and limits

- Full source inspection, targeted diff/status inspection, and `git diff --check` completed. Diff check passed with only repository line-ending notices.
- Built only `pulse_shell_launch_client_test` through `cmd.exe /d /s /c "call scripts\vcvars.bat && ... && cmake --build build --target pulse_shell_launch_client_test"` using the approved elevated build environment. `TEMP` and `TMP` were set only for each command to `E:\Projects\pulse\bench_data\shell_audit_temp`. Both original-source and repaired-source actual compilations completed without new compiler warnings.
- Original-production comparison: temporarily reversed only the two owned production-file fixes; `git diff --numstat` for those files was empty. The same new test source and `build\pulse_shell_launch_client_test.exe --startup-recovery` produced **3 PASS / 3 FAIL, exit 1**. Failures were startup retirement, next-attempt backoff state, and error snapshot retention.
- Repaired comparison: restored the exact repaired file bytes, refreshed their timestamps because copy restoration preserved pre-comparison timestamps, and confirmed that Ninja actually recompiled both the client and test rather than reusing baseline objects. The identical selector produced **6 PASS / 0 FAIL, exit 0**.
- No broad suite, installed-app interaction, user file operation or preference change was performed. The only spawned process was the fixture's own executable, which exits with code 7 before creating any pipe.
- Existing Shell client fixtures cover non-replay and transport completion routing. The added targeted case covers a spawned fixture exiting before its initial pipe connection.
- No further reachable correctness defect was confirmed in `ctx_menu_util.h` or the crash reporter during this scope. This is not runtime proof of Shell extension behavior or crash capture reliability.
- Named-pipe primitives, separate interaction/context broker implementations, diagnostics exporter and third-party Shell extensions were not fully audited in this work package.
- Crash-path allocation failure, in-process minidump failure, concurrent diagnostics retention, and possible future multithreaded breadcrumb writers remain validation considerations; they were not promoted to confirmed findings without a concrete current production trigger.
