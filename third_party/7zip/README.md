# Bundled 7-Zip runtime

Official 7-Zip 26.03 (x64), downloaded 2026-09-30 from
https://www.7-zip.org/a/7z2603-x64.exe and extracted with the official
https://www.7-zip.org/a/7zr.exe. Only the command-line runtime and its license
are redistributed here. No installed 7-Zip application is required.

Installer SHA256: `0859C524B8A63551848F0C246ABDDCB1D0B7B656B0FBFE879F8D85E61A9E6EDD`

| File | SHA256 |
| --- | --- |
| 7z.exe | `6EE3C0ED0B27663C1B948AE85A7C0BB073AED1498983182F3F0DF1F6A8C30B2F` |
| 7z.dll | `65E4C1F855F9EF6E8F0F5DF8E3F27D9EB5F07311408639DA0A1CA0B8F4871B0D` |

See [License.txt](License.txt) for GNU LGPL and unRAR license restrictions.
The upstream source is available at https://www.7-zip.org/download.html.
Pulse starts this executable directly with hidden windows, argument quoting,
UTF-8 output, and EOF on standard input. This dependency is used exclusively
for archive listing and extraction; it must accompany releases in `7zip/`.

## License texts and corresponding source

Licensing material added by EternalLightning for this independent Pulse
distribution; the upstream runtime binaries and License.txt are unchanged.
The full LGPL 2.1 and GPL 2 texts are included as
`LGPL-2.1-or-later.txt` and `GPL-2.0-or-later.txt` (copied from the SPDX
license-list-data text directory). The BSD and unRAR notices remain in
`License.txt`.

The official corresponding source archive for the bundled 26.03 runtime is
included unmodified as `7z2603-src.7z`, obtained from
https://www.7-zip.org/a/7z2603-src.7z on 2026-10-30. Both Pulse packaging
paths include it alongside the runtime and license texts, so recipients
receive the source rather than having to rely on a third-party download link.
Extract this archive with 7-Zip to obtain the upstream source and build files.
Archive SHA-256: `41e2a7c0e9f838351c625e01f0f581a2188bb3dda10c8e0b4a852da26546ffe2`.
Its `C/7zVersion.h` identifies version 26.03, matching the bundled runtime.
When updating the runtime, update this archive, its hash, and both packaging
paths together; do not pair a new runtime with this old source archive.
