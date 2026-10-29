# Pulse 1.2.1.1 packaging

- Packaged current working-tree sources without changing the version or existing source edits.
- Release build and Inno Setup compilation passed. Production selftest-dispatcher check passed.
- MSVC runtime dependency scan passed: no additional runtime DLLs required.
- Portable verification passed: all 41 payload files match their sources by SHA-256 and size; six executable versions and original filenames match.
- Extracted ZIP startup check: isolated data directory, fixture directory enumerated, process exit code 0.
- The hidden launch produced a black screenshot; this does not verify visible rendering.
- Installer compilation reported the admin/HKCU per-user-area warning. Installation, elevation, service registration and upgrade behavior were not exercised.
- Existing same-version artifacts were backed up to dist/package-backup-20261026-185050.
- New SHA-256 sidecars are beside both artifacts; identities are recorded in checksums.json and artifacts.json.

Artifacts:
- dist/PulseSetup-1.2.1.1.exe
- dist/Pulse-1.2.1.1-portable-win-x64.zip
