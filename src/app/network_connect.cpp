// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "network_connect.h"
#include "../common/localization.h"
#include <algorithm>
#include <cwctype>
#include <set>
#include <utility>

namespace pulse::app {
namespace {
std::mutex shares_mutex;
std::set<std::wstring> pending_shares;
std::wstring Key(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
}
bool IsNetworkAuthenticationError(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_LOGON_FAILURE ||
        error == ERROR_INVALID_PASSWORD || error == ERROR_ACCOUNT_RESTRICTION ||
        error == ERROR_SESSION_CREDENTIAL_CONFLICT;
}
std::wstring UncShareRoot(const std::wstring& path) {
    std::wstring rest;
    if (path.size() > 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0) rest = path.substr(8);
    else if (path.starts_with(L"\\\\") && path.size() > 2 && path[2] != L'?' && path[2] != L'.') rest = path.substr(2);
    else return {};
    const auto slash = rest.find(L'\\');
    if (slash == std::wstring::npos || slash == 0 || slash + 1 >= rest.size() || rest[slash + 1] == L'\\') return {};
    return L"\\\\" + rest.substr(0, rest.find(L'\\', slash + 1));
}
std::wstring NetworkConnectErrorText(DWORD error) {
    if (error == ERROR_CANCELLED) return l10n::Get(l10n::StringId::NetworkSignInCanceled);
    if (error == ERROR_SESSION_CREDENTIAL_CONFLICT)
        return l10n::Get(l10n::StringId::NetworkCredentialConflict);
    if (IsNetworkAuthenticationError(error))
        return l10n::Get(l10n::StringId::NetworkSignInFailed);
    return l10n::Get(l10n::StringId::NetworkConnectFailedPrefix) + std::to_wstring(error);
}
void NetworkConnectMailbox::Close() { std::lock_guard lock(mutex); closed = true; results.clear(); }
void NetworkConnectMailbox::Publish(NetworkConnectResult result) {
    std::lock_guard lock(mutex);
    if (!closed) results.push_back(std::move(result));
}
std::vector<NetworkConnectResult> NetworkConnectMailbox::Take() {
    std::lock_guard lock(mutex);
    std::vector<NetworkConnectResult> taken;
    taken.swap(results);
    return taken;
}
bool BeginNetworkShareRequest(const std::wstring& share) {
    std::lock_guard lock(shares_mutex);
    return !share.empty() && pending_shares.insert(Key(share)).second;
}
void EndNetworkShareRequest(const std::wstring& share) {
    std::lock_guard lock(shares_mutex);
    pending_shares.erase(Key(share));
}
}
