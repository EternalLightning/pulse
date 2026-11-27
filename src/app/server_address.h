// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// server_address.h - What the user typed to reach a server: \\host\share,
// //host/share, smb://user@host/share, a bare IP, or (in the connect flow) a
// bare host name. Pure string work so the address bar and tests share it; no
// name resolution, no network access.
#pragma once
#include <string>
#include <string_view>

namespace pulse::app {

enum class ServerAddressKind {
    Invalid,      // empty, a local path, or a malformed host
    Server,       // \\host (the share list)
    Share,        // \\host\share[\more]
    WebDav,       // Windows WebClient UNC form of an HTTP(S) WebDAV address
    Unsupported,  // ftp://, afp://, nfs://, sftp://, ...
};

struct ServerAddress {
    ServerAddressKind kind = ServerAddressKind::Invalid;
    std::wstring unc;     // normalized \\host[\share...] (no trailing separator)
    std::wstring host;    // host as it appears in unc
    std::wstring scheme;  // lower-case scheme for URL input ("smb", "ftp", ...)
    std::wstring user;    // smb://user@host: dropped from unc, kept for sign-in
};

// `allow_bare_host`: "nas" or "nas\share" count as a server (the connect
// flow). The address bar passes false so folder names and aliases keep
// working there; bare IPv4 addresses ("192.168.1.20\share") are always
// accepted.
ServerAddress ParseServerAddress(std::wstring_view text, bool allow_bare_host = false);

constexpr bool IsBrowsableServerAddress(ServerAddressKind kind) {
    return kind == ServerAddressKind::Server || kind == ServerAddressKind::Share ||
        kind == ServerAddressKind::WebDav;
}

// "\\host\share\a\b" -> "\\host\share"; "\\host\" -> "\\host"; else empty.
// Accepts the \\?\UNC\ prefix.
std::wstring NetworkRecentRoot(std::wstring_view path);

} // namespace pulse::app
