// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/shell_path.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>

// Link production objects with scripts/test_integration_fixture.ps1.
// Listing completions are injected; no search/index worker or Explorer starts.
namespace {
int failures = 0;
constexpr int kTargetRow = 199;
constexpr wchar_t kTargetName[] = L"z-target.txt";

void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}

pulse::fs::SnapshotPtr Listing(const std::wstring& folder) {
    auto rows = std::make_shared<std::vector<pulse::fs::DirEntry>>(kTargetRow + 1);
    for (int i = 0; i <= kTargetRow; ++i) {
        auto& entry = (*rows)[static_cast<size_t>(i)];
        entry.name = i == kTargetRow ? kTargetName : L"other-" + std::to_wstring(i) + L".txt";
        entry.full_path = folder + L"\\" + entry.name;
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
    }
    return rows;
}

void OpenSearchHit(pulse::AppState& s, const std::wstring& path) {
    auto* tab = pulse::ActiveTab(s);
    pulse::StartLoadingPath(s, *tab, pulse::app::MakeHomePath());
    tab->current_path = pulse::app::MakeSearchPath(L"open-location-fixture");
    tab->loading = false;
    tab->pending_generation = 0;
    tab->ClearBanner();
    auto hit = std::make_shared<std::vector<pulse::fs::DirEntry>>(1);
    (*hit)[0].name = kTargetName;
    (*hit)[0].full_path = path;
    (*hit)[0].attrs = FILE_ATTRIBUTE_NORMAL;
    tab->SetSnapshot(hit);
    tab->SelectOnly(0);
    pulse::DispatchMenuCommand(s, pulse::app::CmdOpenPath);
}
}

int main() {
    using namespace pulse;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(OleInitialize(nullptr))) return 2;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!s.hwnd) { owned.reset(); OleUninitialize(); return 2; }
    s.compositor.Resize(1100, 700);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();

    const auto directory = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        L"bench_data" / (L"open-location-fixture-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error)) {
        std::printf("[FAIL] fixture directory creation: %d\n", error.value());
        DestroyWindow(s.hwnd); s.hwnd = nullptr;
        owned.reset(); OleUninitialize(); return 2;
    }
    const auto file = directory / kTargetName;
    HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(created != INVALID_HANDLE_VALUE, "isolated target file created");
    if (created != INVALID_HANDLE_VALUE) CloseHandle(created);

    for (bool extended : {false, true}) {
        std::printf("[DIAG] %s search-hit path\n", extended ? "extended" : "ordinary");
        const std::wstring selected = extended ? L"\\\\?\\" + file.wstring() : file.wstring();
        OpenSearchHit(s, selected);
        auto* tab = ActiveTab(s);
        const std::wstring folder = tab->current_path;
        Check(app::SameShellPath(folder, directory.wstring()),
            "CmdOpenPath navigates from search results to the containing folder");
        Check(tab->pending_selected_names == std::vector<std::wstring>{kTargetName} &&
            tab->pending_selected_name == kTargetName && tab->pending_ensure_selection_visible,
            "CmdOpenPath queues the selected file and ensure-visible intent");

        // Workers stay stopped. Inject the same production completion boundary
        // deterministically, including an older result arriving first.
        const uint64_t generation = extended ? 200 : 100;
        tab->pending_generation = generation;
        app::WorkResult loaded{};
        loaded.path = folder;
        loaded.generation = generation - 1;
        loaded.snapshot = Listing(directory.wstring());
        ApplyWorkerResult(s, loaded);
        Check(tab->pending_generation == generation && !tab->snapshot &&
            tab->pending_ensure_selection_visible && !tab->pending_selected_names.empty(),
            "stale parent listing cannot consume the requested selection");
        loaded.generation = generation;
        ApplyWorkerResult(s, loaded);
        Check(tab->IsSelected(kTargetRow) && tab->selected_index == kTargetRow &&
            tab->SelectedCount() == 1 && !tab->pending_generation &&
            tab->pending_selected_names.empty() && !tab->pending_ensure_selection_visible,
            "matching delayed listing selects only the requested file");
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const auto bounds = FocusedPaneRect(s);
        const auto viewport = s.renderer.PaneListRect(pane, bounds);
        const auto item = s.renderer.ItemRectInPane(pane, bounds, kTargetRow);
        Check(tab->scroll_y > 0 && viewport.bottom > viewport.top &&
            item.top >= viewport.top - 0.5f && item.bottom <= viewport.bottom + 0.5f &&
            s.scrollTargetY == tab->scroll_y && !s.scrollAnimating,
            "selected file is scrolled fully into the visible list");

        OpenSearchHit(s, selected);
        tab = ActiveTab(s);
        tab->pending_generation = generation + 1;
        loaded.path = tab->current_path;
        loaded.generation = generation + 1;
        StartLoadingPath(s, *tab, app::MakeHomePath());
        const auto current_snapshot = tab->snapshot;
        const auto current_banner = tab->banner_message;
        loaded.error = loaded.cancelled = true;
        loaded.error_code = ERROR_CANCELLED;
        ApplyWorkerResult(s, loaded);
        Check(tab->current_path == app::MakeHomePath() && tab->snapshot == current_snapshot &&
            tab->banner_message == current_banner && tab->pending_selected_names.empty(),
            "cancelled old listing cannot change the replacement view");
        loaded.error = loaded.cancelled = false;
        loaded.error_code = ERROR_SUCCESS;
        ApplyWorkerResult(s, loaded);
        Check(tab->current_path == app::MakeHomePath() && tab->pending_selected_names.empty() &&
            !tab->pending_ensure_selection_visible && !tab->IsSelected(kTargetRow),
            "navigation cancels reveal intent and rejects the late parent listing");
    }

    s.watches.Stop();
    s.window_tabs.items.clear(); s.pane = nullptr;
    Check(std::filesystem::remove(file, error) && !error, "fixture file cleanup");
    error.clear();
    Check(std::filesystem::remove(directory, error) && !error, "fixture directory cleanup");
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    return failures ? 1 : 0;
}
