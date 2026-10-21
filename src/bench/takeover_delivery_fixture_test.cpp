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
    app::SystemTakeoverTestCallbacks callbacks;
    callbacks.active = [&](uint64_t token) { return active.contains(token); };
    callbacks.acknowledge = [&](uint64_t token, bool delivered) { answers.insert_or_assign(token, delivered); };
    callbacks.drain = [&] { return std::exchange(requests, {}); };
    callbacks.route = [&](const app::ExplorerTakeoverRequest&) { ++routes; return true; };
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
    auto complete = [&](uint64_t token) {
        app::ExternalOpenResult result;
        result.takeover_token = token; result.target.folder = folder;
        result.selection_flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE;
        s.systemIntegration.CompleteExternalOpen(s, &s, result, true);
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
    s.systemIntegration.Stop();
    s.core.windows.Remove(s.hwnd); s.core.global_ui_owner = nullptr;
    s.window_tabs.items.clear(); s.pane = nullptr;
    const HWND window = s.hwnd; s.hwnd = nullptr;
    state.reset(); DestroyWindow(window);
    OleUninitialize();
    return failures ? 1 : 0;
}
