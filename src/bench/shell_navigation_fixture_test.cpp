#include "../app/app_internal.h"
#include "../app/shell_selection.h"
#include "../app/archive_navigation.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>
#include <map>

// Link with scripts/test_integration_fixture.ps1 after the parent's pulse build.
// No GUI is shown, no services start, and no real registry/preferences are used.
namespace {
class MemoryRegistry final : public pulse::app::DefaultManagerRegistryStore {
public:
    using Value = pulse::app::DefaultManagerRegistryValue;
    std::map<std::pair<std::wstring, std::wstring>, Value> values;
    LSTATUS Read(const std::wstring& path, const std::wstring& name, Value& value) override {
        const auto found = values.find({path, name});
        value = found == values.end() ? Value() : found->second;
        return ERROR_SUCCESS;
    }
    LSTATUS Write(const std::wstring& path, const std::wstring& name, const Value& value) override {
        if (value.exists) values[{path, name}] = value;
        else values.erase({path, name});
        return ERROR_SUCCESS;
    }
    LSTATUS Flush(const std::wstring&) override { return ERROR_SUCCESS; }
    LSTATUS RemoveTree(const std::wstring& path) override {
        std::erase_if(values, [&](const auto& value) {
            return value.first.first == path || value.first.first.starts_with(path + L"\\");
        });
        return ERROR_SUCCESS;
    }
    void RemoveEmpty(const std::wstring&) override {}
};
bool WaitSystemCompletion(HWND hwnd) {
    const ULONGLONG until = GetTickCount64() + 5000;
    MSG message{};
    while (GetTickCount64() < until) {
        if (PeekMessageW(&message, hwnd, pulse::app::kSystemIntegrationMessage,
            pulse::app::kSystemIntegrationMessage, PM_REMOVE)) return true;
        Sleep(1);
    }
    return false;
}
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
pulse::fs::SnapshotPtr Listing(const std::wstring& folder) {
    auto rows = std::make_shared<std::vector<pulse::fs::DirEntry>>(2);
    (*rows)[0].name = L"other.txt";
    (*rows)[1].name = L"launched.txt";
    for (auto& entry : *rows) {
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entry.full_path = folder + L"\\" + entry.name;
    }
    return rows;
}
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[DIAG] shell fixture entered main\n");
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    std::printf("[DIAG] AppState constructed\n");
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!s.hwnd) { owned.reset(); OleUninitialize(); return 2; }
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);

    const std::wstring this_pc = app::kThisPcParsingName;
    Check(ResolveOpenTarget(this_pc).folder.empty(), "cold This PC argument resolves to empty rather than C");
    s.pane->NewTab(ResolveOpenTarget(this_pc).folder);
    Check(ActiveTab(s)->current_path.empty(), "cold startup pane preserves This PC empty path");
    s.pane->NewTab(app::MakeHomePath());
    OpenFolderInNewTab(s, this_pc);
    const size_t count = s.window_tabs.items.size();
    Check(count == 2 && ActiveTab(s)->current_path.empty(), "hot This PC creates explicit empty-path tab");
    OpenFolderInNewTab(s, this_pc);
    Check(s.window_tabs.items.size() == count && ActiveTab(s)->current_path.empty(), "hot This PC reuses an existing This PC tab");
    NewTab(s, L"pulse:fixture:ordinary");
    Check(ActiveTab(s)->current_path == L"pulse:fixture:ordinary", "ordinary explicit new tab behavior stays separate from This PC opener");

    // Use only a newly-created temp fixture; filesystem watches are stopped
    // before removing it. No actual Explorer/COM registration is started.
    wchar_t temporary[32768]{};
    const DWORD fixture_root = GetEnvironmentVariableW(L"PULSE_TEST_FIXTURE_ROOT", temporary, ARRAYSIZE(temporary));
    if (fixture_root >= ARRAYSIZE(temporary) ||
        (!fixture_root && !GetTempPathW(ARRAYSIZE(temporary), temporary))) { OleUninitialize(); return 2; }
    const auto directory = std::filesystem::path(temporary) /
        (L"pulse-shell-navigation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::printf("[DIAG] fixture directory=%ls\n", directory.c_str());
    std::error_code fixture_error;
    if (!std::filesystem::create_directory(directory, fixture_error)) {
        std::printf("[FAIL] could not create isolated fixture: error=%d %s\n",
            fixture_error.value(), fixture_error.message().c_str());
        OleUninitialize(); return 2;
    }
    const auto file = directory / L"launched.txt";
    HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        std::printf("[FAIL] could not create fixture file: error=%lu\n", GetLastError());
        std::filesystem::remove(directory, fixture_error); OleUninitialize(); return 2;
    }
    CloseHandle(created);
    const auto target = ResolveOpenTarget(file.wstring());
    const std::wstring folder = target.folder;
    Check(target.file_leaf == L"launched.txt" && app::SameShellPath(target.file_path, file.wstring()),
        "open target resolves folder and reusable file metadata together");
    tab = ActiveTab(s);
    StartLoadingPath(s, *tab, folder);
    tab->pending_generation = 40;
    SelectLaunchedFile(s, target);
    Check(tab->pending_selected_names == std::vector<std::wstring>{L"launched.txt"} && tab->pending_ensure_selection_visible,
        "cold file launch queues delayed selection while loading");
    app::WorkResult stale{};
    stale.path = folder; stale.generation = 39; stale.snapshot = Listing(folder);
    ApplyWorkerResult(s, stale);
    Check(tab->pending_generation == 40 && !tab->snapshot && !tab->pending_selected_names.empty(),
        "stale loading result cannot consume launch selection intent");
    app::WorkResult loaded{};
    loaded.path = folder; loaded.generation = 40; loaded.snapshot = Listing(folder);
    ApplyWorkerResult(s, loaded);
    Check(!tab->pending_generation && tab->IsSelected(1) && tab->selected_index == 1,
        "matching cold file result selects requested file");
    tab->SelectOnly(0);
    tab->filter_text = L"other";
    SelectLaunchedFile(s, target);
    Check(tab->IsSelected(0) && !tab->IsSelected(1), "file launch does not select filtered-out cached row");
    tab->filter_text.clear();
    tab->snapshot_path = folder + L"\\stale";
    SelectLaunchedFile(s, target);
    Check(tab->IsSelected(0) && !tab->IsSelected(1), "file launch does not select wrong-identity cached row");
    tab->snapshot_path = folder;
    OpenFolderInNewTab(s, file.wstring());
    Check(app::SameShellPath(ActiveTab(s)->current_path, folder) && ActiveTab(s)->IsSelected(1),
        "hot single-instance file open reuses folder and selects file immediately");
    tab = ActiveTab(s);
    tab->SelectOnly(0);
    tab->pending_generation = 42;
    SelectLaunchedFile(s, target);
    Check(tab->IsSelected(0) && !tab->IsSelected(1), "pending listing retains only file selection intent before matching refresh");
    loaded.generation = 42;
    ApplyWorkerResult(s, loaded);
    Check(tab->IsSelected(1), "cached hot file selection survives its matching refresh");
    tab->pending_generation = 43;
    SelectLaunchedFile(s, target);
    StartLoadingPath(s, *tab, app::MakeHomePath());
    loaded.generation = 43;
    ApplyWorkerResult(s, loaded);
    Check(tab->current_path == app::MakeHomePath() && tab->pending_selected_names.empty(),
        "navigation cancels launch intent and late old-folder completion");
    const auto archive = directory / L"archive.zip";
    created = CreateFileW(archive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created != INVALID_HANDLE_VALUE) CloseHandle(created);
    Check(IsArchiveView(ResolveOpenTarget(archive.wstring()).folder), "archive file keeps special archive-view launch convention");

    // This is the exact selection/guard implementation used before takeover ACK.
    app::Tab destination;
    app::PlacesCatalog places; places.persist = false;
    destination.current_path = L"\\\\?\\C:\\fixture";
    destination.SetSnapshot(Listing(destination.current_path));
    destination.applied_generation = 50;
    std::vector<int> rows;
    const std::vector<std::wstring> selected{L"C:\\fixture\\launched.txt"};
    const unsigned flags = SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE;
    Check(app::ApplyShellSelection(destination, places, selected, flags, rows) && destination.IsSelected(1),
        "takeover delivery matches ordinary Shell path to extended destination and selects visibly");
    Check(app::ApplyShellSelection(destination, places, selected, SVSI_ENSUREVISIBLE, rows) && destination.IsSelected(1),
        "ensure-only Shell request preserves existing selection");
    Check(app::ApplyShellSelection(destination, places, selected, SVSI_DESELECT, rows) && !destination.IsSelected(1),
        "Shell deselect never selects target row");
    Check(app::ApplyShellSelection(destination, places, selected, SVSI_FOCUSED, rows) && !destination.IsSelected(1),
        "focus-only Shell request does not implicitly select row");
    destination.filter_text = L"other";
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows), "filtered-out row refuses takeover ACK");
    destination.filter_text.clear();
    auto hidden = std::make_shared<std::vector<fs::DirEntry>>(*destination.snapshot);
    (*hidden)[1].attrs = FILE_ATTRIBUTE_HIDDEN;
    destination.SetSnapshot(hidden);
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows) && !destination.IsSelected(1),
        "hidden unselectable row refuses takeover ACK");
    destination.SetSnapshot(Listing(destination.current_path));
    destination.loading = true;
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows), "loading view refuses takeover ACK");
    destination.loading = false;
    destination.pending_generation = 51;
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows) &&
        app::ShellSelectionIsStale(destination, L"C:\\fixture", 50, destination.view_generation),
        "new pending generation cancels stale takeover ACK");
    destination.pending_generation = 0;
    Check(app::ShellSelectionIsStale(destination, L"C:\\fixture", 50, destination.view_generation + 1),
        "changed view generation cancels takeover ACK");
    destination.banner_message = L"unavailable";
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows), "error view refuses takeover ACK");
    destination.banner_message.clear();
    destination.net_readonly = true;
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows), "offline cached view refuses takeover ACK");
    destination.net_readonly = false;
    Check(!app::ApplyShellSelection(destination, places, {L"C:\\fixture\\missing.txt"}, flags, rows),
        "missing target refuses takeover ACK");
    destination.snapshot_path = L"C:\\different";
    Check(!app::ApplyShellSelection(destination, places, selected, flags, rows), "wrong snapshot identity refuses takeover ACK");
    destination.current_path.clear(); destination.snapshot_path.clear();
    destination.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    Check(app::ApplyShellSelection(destination, places, {}, flags, rows), "ready empty This PC without selection can ACK");

    // Exercise the actual background-to-UI completion gap with no real registry or COM.
    std::printf("[DIAG] starting isolated system mailbox regression\n");
    MemoryRegistry registry;
    Check(s.systemIntegration.StartForTesting(s, {nullptr, false, &registry}) &&
        WaitSystemCompletion(s.hwnd), "isolated system task posts initial completion");
    Check(s.systemIntegration.pending(), "system controls remain pending until completion is consumed by UI");
    std::printf("[DIAG] consuming initial completion and submitting enable\n");
    s.systemIntegration.Tick(s);
    s.systemIntegration.Toggle(s, 21);
    Check(WaitSystemCompletion(s.hwnd) && s.systemIntegration.pending() &&
        !s.appPrefs.open_folders_in_pulse, "worker apply completion retains pending while UI still has old preference state");
    s.systemIntegration.Toggle(s, 21);
    s.systemIntegration.Tick(s);
    Check(s.appPrefs.open_folders_in_pulse && s.appPrefs.take_over_win_e &&
        s.appPrefs.take_over_this_pc && !s.systemIntegration.pending(),
        "unconsumed completion rejects duplicate toggle and applies all actual values together");
    std::printf("[DIAG] submitting disable\n");
    s.systemIntegration.Toggle(s, 21);
    Check(WaitSystemCompletion(s.hwnd), "next accepted system toggle posts completion");
    s.systemIntegration.Tick(s);
    Check(!s.appPrefs.open_folders_in_pulse && !s.appPrefs.take_over_win_e &&
        !s.appPrefs.take_over_this_pc && registry.values.empty(),
        "next system toggle uses consumed actual state and restores isolated associations");
    s.systemIntegration.Stop();
    std::printf("[DIAG] isolated system worker stopped\n");

    s.watches.Stop();
    s.window_tabs.items.clear(); s.pane = nullptr;
    Check(std::filesystem::remove(file, fixture_error) && !fixture_error, "fixture file cleanup");
    fixture_error.clear();
    Check(std::filesystem::remove(archive, fixture_error) && !fixture_error, "fixture archive cleanup");
    fixture_error.clear();
    Check(std::filesystem::remove(directory, fixture_error) && !fixture_error, "fixture directory cleanup");
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset();
    std::printf("[DIAG] AppState destroyed\n");
    OleUninitialize();
    return failures ? 1 : 0;
}
