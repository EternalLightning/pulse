# Archive listing and starred sidebar validation — 2026-10-21

Scope: archive listing throughput and cancellation, plus starred sidebar typography and visibility. Existing changes from other tasks remain in the working tree. No installed application was replaced.

## Archive findings and changes

- Browsing already calls `7z l -slt -ba`; it does not extract archive members. Opening an individual member uses temporary extraction.
- Read-only source: `E:\Projects\MotionControl\MotionControl.7z`, 4,465,574,660 bytes. Listing contains 63,786 records, approximately 15.6 MB of textual metadata.
- Before the change, production `ListArchive` took 100,844 ms outside the sandbox. Direct 7-Zip listing took 2,689 ms. Pipe diagnostics attributed the large delay to the small output pipe and polling interaction.
- Increased pipe capacity to 256 KiB and read chunks to 64 KiB, preserving the existing cancellation poll interval. A diagnostic run returned the complete record count in 2,000 ms. Final production navigation took 2,171 ms and displayed 20 root entries.
- Leaving an archive clears listing, cancellation token, file, prefix, cached password and encrypted-member state. Replacing a pane view and destroying a pane cancel outstanding listing work.
- In the final original-archive navigation test, leaving during listing delivered completion and allowed a following worker task within 47 ms. Source size and modification time remained unchanged.

## Sidebar changes

- The starred root now uses the same navigation font and secondary text color as Quick Access.
- Right-clicking either its label or disclosure arrow opens the existing section menu, including Hide Section.
- The sidebar blank-area menu restores the section. The existing session visibility mask persists the choice; hidden sections leave no layout gap.

## Focused checks

All final commands below exited successfully. Process and UI fixtures ran outside the restricted sandbox because its handle/IPC restrictions can affect these Windows checks. Fixtures use isolated state and do not persist user settings.

```powershell
cmake --build build --target pulse pulse_archive_navigation_test pulse_archive_test
cmake --build build --target pulse_file_selection_ui_test
.\build\pulse_archive_test.exe --process-only
.\build\pulse_archive_test.exe --progress-only
.\build\pulse_archive_navigation_test.exe --lifecycle-only
.\build\pulse_archive_navigation_test.exe --lifecycle-only E:\Projects\MotionControl\MotionControl.7z
.\build\pulse_file_selection_ui_test.exe --sidebar-starred
```

- Process checks: complete 16 MiB output in 78 ms, output limit, final drain, handle isolation, failed launch and cancellation.
- Progress checks: progress/diagnostic pipe separation, final updates, cancellation, ZIP/7z selected extraction and empty directories.
- Navigation checks: highly compressed isolated 7z, original archive, return-to-folder cleanup, subsequent work, close/replace/remove cancellation, original identity and fixture cleanup.
- Sidebar checks: actual pointer selection in production context menus, hide/restore, session JSON roundtrip, wide/rail layout, light/dark themes at 100% and 150%. Initial 150% fixture failures came from a compositor/window size mismatch; resizing the test window resolved them. Final run: 48 PASS, 0 FAIL.
- Reviewed actual compositor screenshots under `bench_data/sidebar-starred-ui`, including visible 100% light, visible 150% dark and hidden 150% dark; text styles agree and hidden content leaves no gap.
- `git diff --check` found no whitespace errors.

Earlier focused Shell worker checks passed for blocked query/invoke reclamation and subsequent host responsiveness. A read-only original-file menu probe found the exact `SevenZipOpen` verb, invoked it successfully in 125 ms, observed the launched 7-Zip application and received a subsequent host ping. These checks did not reproduce the user's original persistent Shell stall in the installed UI; that particular failure cannot yet be claimed resolved.
