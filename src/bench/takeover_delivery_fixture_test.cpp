// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/explorer_window_takeover.h"
#include "../common/user_storage.h"
#include "../common/localization.h"
#include <shlobj.h>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <utility>

namespace {
int failures = 0;
void Check(bool condition, const char* label) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}
class MemoryRegistry final : public pulse::app::DefaultManagerRegistryStore {
public:
    LSTATUS Read(const std::wstring&, const std::wstring&, pulse::app::DefaultManagerRegistryValue& value) override {
        value = {}; return ERROR_SUCCESS;
    }
    LSTATUS Write(const std::wstring&, const std::wstring&, const pulse::app::DefaultManagerRegistryValue&) override { return ERROR_SUCCESS; }
    LSTATUS Flush(const std::wstring&) override { return ERROR_SUCCESS; }
    LSTATUS RemoveTree(const std::wstring&) override { return ERROR_SUCCESS; }
    void RemoveEmpty(const std::wstring&) override {}
};
}

int main() {
    using namespace pulse;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    OleInitialize(nullptr);
    wchar_t root[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", root, ARRAYSIZE(root));
    if (!length || length >= ARRAYSIZE(root)) return 2;
    const auto output = std::filesystem::path(root) / (L"delivery-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(output);
    storage::OverrideDefaultRootForTesting((output / L"profile").wstring());
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = false;
    s.appPrefs.experimental_explorer_takeover = true;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"isolated handoff fixture",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(s.hwnd && s.compositor.Init(s.hwnd), "create isolated offscreen destination");
    if (!s.hwnd || !s.compositor.Dc()) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.core.windows.Add(s.hwnd, &s);
    s.core.global_ui_owner = s.hwnd;
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    const auto folder = output.wstring();
    s.pane->NewTab(folder);
    auto* tab = ActiveTab(s);
    auto listing = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*listing)[0].name = L"other.txt"; (*listing)[1].name = L"launched.txt";
    for (auto& entry : *listing) { entry.attrs = FILE_ATTRIBUTE_NORMAL; entry.full_path = folder + L"\\" + entry.name; }
    tab->SetSnapshot(listing); tab->snapshot_path = folder;
    tab->loading = false; tab->pending_generation = 0; tab->applied_generation = 50;
    const auto launched = folder + L"\\launched.txt", other = folder + L"\\other.txt";
    std::set<uint64_t> active;
    std::map<uint64_t, bool> answers;
    std::vector<app::ExplorerTakeoverRequest> requests;
    size_t routes = 0;
    std::wstring routed_folder;
    std::vector<std::wstring> routed_paths;
    app::SystemTakeoverTestCallbacks callbacks;
    callbacks.active = [&](uint64_t token) { return active.contains(token); };
    callbacks.acknowledge = [&](uint64_t token, bool delivered) { answers.insert_or_assign(token, delivered); };
    callbacks.drain = [&] { return std::exchange(requests, {}); };
    callbacks.route = [&](const app::ExplorerTakeoverRequest& request) {
        ++routes; routed_folder = request.folder; routed_paths = request.selected_paths; return true;
    };
    MemoryRegistry registry;
    Check(s.systemIntegration.StartForTesting(s, {nullptr, false, &registry}, std::move(callbacks)),
        "use isolated delivery callbacks without Explorer or real registry");
    const auto deadline = GetTickCount64() + 5000;
    while (s.systemIntegration.pending() && GetTickCount64() < deadline) { s.systemIntegration.Tick(s); Sleep(1); }
    const auto original_tabs = s.window_tabs.items.size();
    auto submit = [&](uint64_t token, uint64_t replaces, std::vector<std::wstring> paths) {
        active.insert(token); requests.push_back({token, folder, std::move(paths), replaces});
        s.systemIntegration.Tick(s);
    };
    auto queue_completion = [&](uint64_t token, std::vector<std::wstring> paths = {}) {
        app::ExternalOpenResult result;
        result.takeover_token = token; result.target.folder = folder;
        result.selected_paths = std::move(paths);
        result.selection_flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE;
        s.systemIntegration.CompleteExternalOpen(s, &s, result, true);
    };
    auto complete = [&](uint64_t token) {
        queue_completion(token);
        s.systemIntegration.Tick(s);
    };
    submit(1, 0, {});
    complete(1);
    Check(routes == 1 && answers.contains(1) && answers[1], "initial destination is acknowledged only after its view is ready");
    active.erase(1);
    submit(2, 1, {launched});
    Check(routes == 1 && answers.contains(2) && answers[2] && tab->IsSelected(1) &&
        s.window_tabs.items.size() == original_tabs && s.core.windows.Count() == 1,
        "late selection after the first ACK reuses the destination without another open");
    active.erase(2); ++tab->view_generation;
    submit(3, 2, {other});
    Check(routes == 1 && answers.contains(3) && !answers[3] && tab->IsSelected(1),
        "changed destination view rejects replacement instead of selecting or opening elsewhere");
    submit(4, 99999, {other});
    Check(routes == 1 && answers.contains(4) && !answers[4], "unknown replacement identity never opens a new destination");
    submit(5, 0, {});
    active.erase(5);
    submit(6, 5, {});
    complete(5);
    Check(answers.contains(5) && !answers[5] && !s.systemIntegration.IsExternalOpenActive(5),
        "replaced unresolved completion cannot deliver or acknowledge success");
    complete(6);
    Check(routes == 3 && answers.contains(6) && answers[6], "replacement before delivery can route and complete once for its current token");
    active.erase(6); tab->filter_text = L"launched";
    submit(7, 6, {other});
    Check(routes == 3 && answers.contains(7) && !answers[7], "filtered-out late target rejects ACK without reopening a pane");
    tab->filter_text.clear();
    tab->ClearSelection();
    submit(8, 0, {launched});
    tab->pending_generation = 51;
    queue_completion(8, {launched});
    tab->pending_generation = 52;
    s.systemIntegration.Tick(s);
    Check(!answers.contains(8) && !tab->IsSelected(1) && s.systemIntegration.IsExternalOpenActive(8),
        "same-path refresh during delivery waits without selecting a cached row or rejecting the token");
    tab->pending_generation = 0; tab->applied_generation = 52; tab->loading = true;
    s.systemIntegration.Tick(s);
    Check(!answers.contains(8), "delivery still waits while the refreshed snapshot is loading");
    auto refreshed_listing = std::make_shared<std::vector<fs::DirEntry>>(*listing);
    std::swap((*refreshed_listing)[0], (*refreshed_listing)[1]);
    tab->SetSnapshot(refreshed_listing);
    tab->loading = false;
    s.systemIntegration.Tick(s);
    Check(answers.contains(8) && answers[8] && tab->IsSelected(0),
        "same-path refresh completes delivery against the final visible snapshot");
    active.erase(8);
    submit(9, 8, {other});
    Check(answers.contains(9) && answers[9] && tab->IsSelected(1),
        "refreshed delivery retains its final generation for a late replacement");
    submit(10, 0, {launched});
    tab->pending_generation = 53;
    queue_completion(10, {launched});
    ++tab->view_generation;
    s.systemIntegration.Tick(s);
    Check(answers.contains(10) && !answers[10] && tab->IsSelected(1),
        "view changes during a pending refresh still reject delivery");
    tab->pending_generation = 0;
    submit(11, 0, {});
    complete(11);
    Check(answers.contains(11) && answers[11], "prepare a completed binding for source navigation");
    active.erase(11);
    const auto latest_folder = folder + L"\\latest";
    const auto latest_path = latest_folder + L"\\launched.txt";
    const auto routes_before_navigation = routes;
    active.insert(12); requests.push_back({12, latest_folder, {latest_path}, 11});
    s.systemIntegration.Tick(s);
    Check(routes == routes_before_navigation + 1 && routed_folder == latest_folder &&
        routed_paths == std::vector<std::wstring>{latest_path} && !answers.contains(12) &&
        s.systemIntegration.IsExternalOpenActive(12),
        "changed-folder replacement routes the latest source folder and selection instead of rejecting it");
    auto latest_listing = std::make_shared<std::vector<fs::DirEntry>>(1);
    (*latest_listing)[0].name = L"launched.txt";
    (*latest_listing)[0].attrs = FILE_ATTRIBUTE_NORMAL;
    (*latest_listing)[0].full_path = latest_path;
    tab->ClearSelection(); tab->current_path = tab->snapshot_path = latest_folder;
    tab->SetSnapshot(latest_listing); tab->applied_generation = 53;
    app::ExternalOpenResult latest_result;
    latest_result.takeover_token = 12; latest_result.target.folder = latest_folder;
    latest_result.selected_paths = {latest_path};
    latest_result.selection_flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE;
    s.systemIntegration.CompleteExternalOpen(s, &s, latest_result, true);
    s.systemIntegration.Tick(s);
    Check(answers.contains(12) && answers[12] && tab->IsSelected(0),
        "changed-folder replacement acknowledges only after the latest destination is ready");
    s.systemIntegration.Stop();
    s.core.windows.Remove(s.hwnd); s.core.global_ui_owner = nullptr;
    s.window_tabs.items.clear(); s.pane = nullptr;
    const HWND window = s.hwnd; s.hwnd = nullptr;
    state.reset(); DestroyWindow(window);
    OleUninitialize();
    return failures ? 1 : 0;
}
