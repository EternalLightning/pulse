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
