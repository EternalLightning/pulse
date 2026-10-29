#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {
inline uint64_t NextNetworkIdentity() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1);
}
struct NetworkRequestIdentity {
    uint64_t window = 0, tab = 0, view = 0, listing = 0;
    std::wstring path;
    bool operator==(const NetworkRequestIdentity&) const = default;
};
struct NetworkConnectResult { NetworkRequestIdentity identity; DWORD error = ERROR_CANCELLED; };
inline bool NetworkResultMatches(const NetworkConnectResult& result, const NetworkRequestIdentity& pending,
    const NetworkRequestIdentity& current, const void* pending_tab, const void* current_tab, bool loading) {
    return !loading && pending_tab == current_tab && result.identity == pending && pending == current;
}
bool IsNetworkAuthenticationError(DWORD error);
std::wstring UncShareRoot(const std::wstring& path);
std::wstring NetworkConnectErrorText(DWORD error);
// A task owns no UI objects; the window closes its mailbox before destruction.
struct NetworkConnectMailbox {
    std::mutex mutex;
    bool closed = false;
    std::vector<NetworkConnectResult> results;
    void Close();
    void Publish(NetworkConnectResult result);
    std::vector<NetworkConnectResult> Take();
};
bool BeginNetworkShareRequest(const std::wstring& share);
void EndNetworkShareRequest(const std::wstring& share);
}
