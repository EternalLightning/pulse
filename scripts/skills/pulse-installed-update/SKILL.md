---
name: pulse-installed-update
description: Replace an installed Pulse payload with hash-verified backup and rollback when the user asks to update or replace their installed Pulse. Does not build installers or publish releases.
---

# Pulse installed update

Use the maintained script at `E:/Projects/pulse/scripts/replace_installed.ps1`. Read its implementation only when changing it or diagnosing failure. It defaults to planning; `-Apply` performs replacement. Requires PowerShell 7.2+.

1. Honor the requested scope. Build only affected production targets and run relevant checks; do not bump versions or run full regression just to replace an installed build.
2. Resolve the actual installation path from current process/service/installation evidence. `D:/Applications/Pulse` is a historical location, not an assumed current fact. Never use a build directory as the installation.
3. Use a clean portable payload staging directory, normally produced by `scripts/package_portable.ps1 -BuildDir <actual-build-dir>`. Reuse a verified current stage when available. The script checks the six executable versions against repository `version.txt`, original filenames, and absence of the embedded selftest dispatcher. Dependencies and relevant functional validation still belong to the [build/package workflow](../../../docs/build-and-package.md).
4. Plan using:

   ```powershell
   pwsh -NoProfile -File E:/Projects/pulse/scripts/replace_installed.ps1 -PayloadDir <stage> -InstallDir <installation>
   ```

5. For an authorized installed replacement, run the same command with `-Apply`. Existing authorization suffices; do not ask again without a concrete reason. If the UI is resident, ask the user to exit; WM_CLOSE may merely hide it. Never force-kill it. The script preflights changed files for locks, backs up the existing files represented in the payload, verifies backup hashes, copies only changed files, verifies all payload hashes and checks service/index-agent state. It leaves configuration, uninstaller files and files outside the payload untouched. It never stops/restarts services or processes. If an index/helper file is locked, stop here and resolve the required lifecycle action within the user's authorization; do not disable this check. Service/helper stop/restart is outside this script's automated scope.
6. Read the compact result. On failure, `result.json` records rollback errors and the backup path; do not retry a partial rollback blindly. On success, preserve the backup. Start the installed UI only if requested or appropriate to restoring the user's prior session; launch at the user's ordinary integrity, not from an elevated shell. Run a focused installed smoke check. Hash and service checks alone do not establish UI correctness.

Report changed/verified counts, backup path, installed smoke outcome and any limitations. Do not push or delete backup/evidence unless requested. Do not load old validation logs or rediscover payload lists for routine runs; use script output, and inspect history only on a concrete failure.
