# Cloud sidebar ordering and provider icons

Date: 2026-10-29

The final cloud catalog is sorted after namespace roots, SyncRootManager roots,
and OneDrive account seeds have been merged. OneDrive accounts stay together at
the front; registered sort order, case-insensitive label, and path determine the
remaining order. Account seeds preserve provider identity during de-duplication.

Third-party roots use their registered Shell namespace icon, or the folder icon
for roots without a namespace. Navigation still uses the original folder path.
Both sidebar layouts fall back to a neutral folder glyph when icon loading fails.

## Actual validation

- CMake configuration completed successfully.
- Direct MSVC compilation and linking of the affected cloud test succeeded,
  using the pending commands obtained from the configured build.
- Direct MSVC `/Zs /W4 /permissive- /utf-8` checks passed for `app_model.cpp`,
  `ui_sidebar.cpp`, and `feature_sync_application_test.cpp`, including the new
  `--cloud-sidebar-only` regression entry point. No compiler diagnostics.
- Changed tracked files passed `git diff --check`; newly added cloud files were
  also checked for trailing whitespace.
- Normal Ninja builds could not complete. Ninja stopped making progress and an
  application error dialog was reported. The related build processes were stopped.
- Running `pulse_cloud_features_test.exe --roots-only` returned `0xC0000142`
  before printing assertions. These tests are not recorded as passed.
- Full Pulse linking, interface screenshots, and Xiaomi provider icon validation
  remain unverified. No installed application was replaced.

Build diagnostics: `build-release/cloud-sidebar-build.log` and
`build-release/cloud-sidebar-syntax.log`. The temporary CMake regeneration
override used during diagnosis was removed.

## Focused checks to rerun when execution works

```powershell
cmake --build build-release --target pulse pulse_cloud_features_test pulse_feature_sync_application_test
.\build-release\pulse_cloud_features_test.exe --roots-only
.\build-release\pulse_feature_sync_application_test.exe --cloud-sidebar-only
```

The first test covers mixed namespace/sync-root ordering, account identity after
merging, unrelated names and paths, and deterministic ties. The second checks
production sidebar conversion and captures wide/rail layouts in light/dark mode
at 100% and 150% scaling, including missing-provider icon fallback.
