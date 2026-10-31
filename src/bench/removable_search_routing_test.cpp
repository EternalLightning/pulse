// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app/app_internal.h"
#include "app/search_query.h"
#include "index/persistent_index_policy.h"
#include "index/live_filename_search.h"
#include <filesystem>
#include <iostream>
#include <string_view>

namespace pulse::index {
struct IndexClientTestAccess {
    static void SetVolumes(IndexClient& client, bool mounted_alias = false) {
        ipc::PayloadWriter payload;
        std::vector<std::pair<VolumeKind, const wchar_t*>> mounts{
            {VolumeKind::Removable, L"G:\\"}, {VolumeKind::Other, L"Z:\\"}, {VolumeKind::Fixed, L"C:\\"}};
        if (mounted_alias) mounts.push_back({VolumeKind::Removable, L"C:\\mounted-usb\\"});
        payload.PutU32(1); payload.PutString(L"fixture-index"); payload.PutU32(static_cast<uint32_t>(mounts.size()));
        for (const auto& [kind, mount] : mounts) {
            payload.PutString(kind == VolumeKind::Removable ? L"fixture-usb" : kind == VolumeKind::Other ? L"fixture-unknown" : L"fixture-internal");
            payload.PutString(kind == VolumeKind::Removable ? L"USB storage" : kind == VolumeKind::Other ? L"Unknown device" : L"Internal disk");
            payload.PutString(mount);
            payload.PutString(kind == VolumeKind::Removable ? L"exFAT" : L"NTFS");
            payload.PutString(L"old cached state"); payload.PutString(L"");
            payload.PutU32(7); payload.PutU32(static_cast<uint32_t>(kind));
            payload.PutU32(100); payload.PutU32(kind == VolumeKind::Fixed ? 123 : 987654); payload.PutU32(0);
        }
        payload.PutU32(0);
        client.HandleVolumes(payload.data().data(), payload.data().size());
        client.connected_ = true;
    }
};
struct ContentRefreshTestPeer {
    static void EnableCapture(ContentSearchClient& client) { client.running_ = true; }
    static ContentSearchRequest Last(ContentSearchClient& client) { return client.pending_requests_.back(); }
};
}

using namespace pulse;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}
}
int main(int argc, char** argv) {
    const bool routing_only = argc == 2 && std::string_view(argv[1]) == "--routing-only";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    auto state = std::make_unique<AppState>(); auto& s = *state;
    s.isolatedTest = true;
    s.places.persist = s.appPrefs.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseRemovableRoutingFixture"; RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
        -30000, -30000, 1100, 900, nullptr, nullptr, wc.hInstance, nullptr);
    Check(s.hwnd && s.compositor.Init(s.hwnd), "isolated hidden UI compositor initializes");
    s.compositor.RecreateTextFormats(1.0f); s.renderer.SetCompositor(&s.compositor); s.renderer.SetScale(1.0f);
    s.window_tabs.NewTab(L"G:\\folder"); s.pane = s.window_tabs.Active()->panes.front().get();
    auto& tab = *ActiveTab(s);
    index::IndexClientTestAccess::SetVolumes(s.index);
    Check(index::CachedTransientLocalSearch(L"G:\\folder"), "volume IPC reply publishes removable classification for UI routing");
    index::Query query; query.path_prefix = L"G:\\folder"; query.session_id = 21; query.limit = 40;
    DispatchIndexSearch(s, query, 91);
    Check(s.pendingIndexSearches[91].local_ready && !s.pendingIndexSearches[91].network_ready,
        "removable filename query awaits only existing live scanner provider");
    query.path_prefix = L"C:\\folder"; query.session_id = 22; DispatchIndexSearch(s, query, 92);
    Check(!s.pendingIndexSearches[92].local_ready && s.pendingIndexSearches[92].network_ready,
        "internal filename query retains local index provider");
    query.path_prefix = L"\\\\server\\share\\folder"; query.session_id = 23; DispatchIndexSearch(s, query, 93);
    Check(s.pendingIndexSearches[93].local_ready && !s.pendingIndexSearches[93].network_ready,
        "UNC filename query retains live scanner routing");
    index::ContentRefreshTestPeer::EnableCapture(s.contentSearch);
    StartIndexedContentSearch(s, tab, L"path:G:\\folder content:needle", false);
    const auto request = index::ContentRefreshTestPeer::Last(s.contentSearch);
    Check(tab.search_allow_scan && !request.indexed && !request.subscribe && !request.task_scan && request.root == L"G:\\folder",
        "removable content query selects existing one-shot temporary scan without persistent subscription");
    Check(tab.search_content_active && tab.search_live_generation == 0,
        "temporary content search remains active with no live indexed generation");
    index::IndexClientTestAccess::SetVolumes(s.index, true);
    Check(index::CachedTransientLocalSearch(L"C:\\mounted-usb\\child"), "volume IPC mount alias reaches cached removable routing");
    query.path_prefix = L"C:\\mounted-usb\\child"; query.session_id = 24; DispatchIndexSearch(s, query, 94);
    Check(s.pendingIndexSearches[94].local_ready && !s.pendingIndexSearches[94].network_ready,
        "directory-mounted USB filename query awaits the temporary scanner");
    StartIndexedContentSearch(s, tab, L"path:C:\\mounted-usb\\child content:needle", false);
    const auto mounted_request = index::ContentRefreshTestPeer::Last(s.contentSearch);
    Check(!mounted_request.indexed && !mounted_request.subscribe && mounted_request.root == L"C:\\mounted-usb\\child",
        "directory-mounted USB content query uses the temporary non-subscribing scan");
    index::IndexClientTestAccess::SetVolumes(s.index);
    Check(!index::CachedTransientLocalSearch(L"C:\\mounted-usb\\child"), "volume IPC refresh removes stale directory mount alias");
    tab.current_path = app::MakeSettingsPath(L"index");
    s.settingsExpanded = 0xffffffff;
    const auto output = std::filesystem::absolute(L"bench_data/issue-fixes-removable");
    if (!routing_only) for (bool english : {false, true}) {
        l10n::Initialize(GetModuleHandleW(nullptr), english ? L"en-US" : L"zh-CN");
        auto vm = BuildVm(s);
        vm.settings_scroll = 610.0f;
        Check(vm.settings_index_volumes.size() == 3, "index settings exposes volume reply rows");
        if (vm.settings_index_volumes.size() != 3) continue;
        const auto& usb = vm.settings_index_volumes[0]; const auto& unknown = vm.settings_index_volumes[1];
        Check(!usb.checked && !usb.enabled && usb.progress == 0 && usb.detail.find(L"987") == std::wstring::npos &&
            !usb.state.empty() && usb.state == l10n::Get(l10n::StringId::IndexRemovableExcluded),
            "removable row hides stale counts/progress and disables toggle with localized policy state");
        Check(!unknown.checked && !unknown.enabled && unknown.progress == 0 && unknown.detail.find(L"987") == std::wstring::npos &&
            !unknown.state.empty() && unknown.state == l10n::Get(l10n::StringId::IndexDeviceUnavailable),
            "unknown device row hides stale counts and disables toggle with localized unavailable state");
        Check(vm.settings_index_volumes[2].checked && vm.settings_index_volumes[2].enabled,
            "internal fixed row retains existing index toggle");
        for (bool dark : {false, true}) {
          for (const UINT width : {1100u, 800u}) {
            vm.dark = dark;
            s.compositor.Resize(width, 900);
            s.compositor.Dc()->BeginDraw();
            s.renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(width), 900), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
            const bool drawn = SUCCEEDED(s.compositor.Dc()->EndDraw());
            const auto name = std::wstring(english ? L"settings-en-" : L"settings-zh-") + (dark ? L"dark-" : L"light-") + std::to_wstring(width) + L".png";
            Check(drawn && s.compositor.SaveSnapshot((output / name).c_str()), "localized policy settings render and save preview");
          }
        }
    }
    index::CacheSearchDevices({});
    std::cout << checks - failures << " PASS / " << failures << " FAIL\n";
    s.contentSearch.Stop(); DestroyWindow(s.hwnd); s.hwnd = nullptr; state.reset(); CoUninitialize();
    return failures ? 1 : 0;
}
