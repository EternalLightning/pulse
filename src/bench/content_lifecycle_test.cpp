// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/search_query.h"
#include "../common/localization.h"
#include <filesystem>
#include <cstdio>

int wmain() {
    using namespace pulse;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto fixture = std::filesystem::path(module).parent_path().parent_path() / L"bench_data" /
        (L"content-close-ui-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(fixture);
    SetEnvironmentVariableW(L"LOCALAPPDATA", fixture.c_str());
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    {
        auto state = std::make_unique<AppState>(); auto& s = *state;
        s.appPrefs.persist = false; s.isolatedTest = true;
        s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
        s.contentSearch.Start(nullptr, 0, index::ContentAgentMode::Instant);
        auto& tab = s.pane->view;
        tab.current_path = app::MakeSearchPath(L"content:instant_marker");
        tab.search_input_content = true; tab.search_content_active = true;
        tab.pending_generation = 42; tab.search_live_generation = 42; tab.search_session_id = 8;
        auto rows = std::make_shared<std::vector<fs::DirEntry>>();
        fs::DirEntry row; row.name = L"kept.txt"; rows->push_back(row); tab.SetSnapshot(rows);
        tab.selected_index = 0; tab.scroll_y = 25;
        s.addressLiveDue = GetTickCount64() + 1;
        s.appPrefs.keep_running_on_close = true;
        SuspendContentSearches(s);
        check(!tab.search_content_active && !tab.search_live_generation && !tab.pending_generation && !s.addressLiveDue,
            "close-to-tray clears active query, subscription and pending input timer");
        check(tab.snapshot == rows && tab.EntryCount() == 1 && tab.selected_index == 0 && tab.scroll_y == 25,
            "closing preserves result rows, selection and scroll");
        check(tab.search_content_stopped && tab.banner_title == L"搜索已停止",
            "closed query visibly reports search stopped");
        check(!l10n::Get(l10n::StringId::ContentInstantReady).empty(), "instant mode description has a translated resource");
        l10n::SetLanguage(L"en-US");
        check(l10n::Get(l10n::StringId::ContentSearchStopped) == L"Search stopped" &&
            !l10n::Get(l10n::StringId::ContentInstantReady).empty(), "English stopped and instant status resources are present");
        TickAddressSearch(s, GetTickCount64() + 2000);
        check(!tab.pending_generation && tab.search_content_stopped && tab.snapshot == rows,
            "timer after reopening cannot restart the stopped query");
        check(s.contentSearch.InstantMode(), "production content client is explicitly in instant mode");
        s.contentSearch.Stop();
    }
    std::error_code error; std::filesystem::remove_all(fixture, error);
    CoUninitialize(); return failures ? 1 : 0;
}
