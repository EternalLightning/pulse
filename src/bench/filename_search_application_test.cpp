#include "../app/app_internal.h"
#include "../app/search_query.h"
#include "../common/localization.h"
#include "../index/live_filename_search.h"
#include <cstdio>
#include <filesystem>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static const FluentMenuItem* PinyinOption(const FluentMenu& menu) {
        for (int i = 0; i < menu.model_.Count(); ++i) {
            const auto* item = menu.model_.At(i);
            if (item && item->command == 3) return item;
        }
        return nullptr;
    }
};
}

int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    check(!s.appPrefs.search_pinyin, "pinyin search is disabled for fresh preferences");
    s.appPrefs.search_pinyin = true;
    s.appPrefs.ResetToDefaults();
    check(!s.appPrefs.search_pinyin, "reset preferences disables pinyin search");
    s.appPrefs.search_pinyin = true;
    check(s.appPrefs.FromJson(L"{}") && !s.appPrefs.search_pinyin,
        "preferences missing the pinyin setting keep literal search");
    app::AppPrefs saved;
    saved.persist = false;
    s.appPrefs.search_pinyin = true;
    check(saved.FromJson(s.appPrefs.ToJson()) && saved.search_pinyin,
        "an explicitly enabled pinyin setting survives serialization");
    s.appPrefs.search_pinyin = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"Filename search fixture", 0,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.hwndAddressEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
        0, 0, 300, 30, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.hwndAddressEdit) {
        check(false, "create isolated search editor");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwndAddressEdit = s.hwnd = nullptr;
        owned.reset();
        OleUninitialize();
        return 2;
    }
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    const std::wstring root = L"C:\\filename-search-fixture";
    const std::vector<index::Hit> indexed{
        {root + L"\\budget.txt", L"budget.txt"},
        {root + L"\\unrelated.txt", L"unrelated.txt"},
        {root + L"\\other.txt", L"other.txt"},
        {L"D:\\filename-search-fixture\\budget-outside.txt", L"budget-outside.txt"},
        {root + L"\\中国.txt", L"中国.txt"},
        {root + L"\\zhongguo.txt", L"zhongguo.txt"}
    };
    // Feed the production matcher through the normal provider delivery path.
    // No index service, user files or preferences are touched by this fixture.
    const auto publish = [&](uint32_t id) {
        const auto found = s.pendingIndexSearches.find(id);
        if (found == s.pendingIndexSearches.end()) return;
        const auto query = found->second.query;
        const auto compiled = index::ParseQuery(query.needle);
        index::SearchResult result;
        for (const auto& hit : indexed) {
            if ((!query.path_prefix.empty() && !index::FilenameSearchCovered(hit.path, query.path_prefix)) ||
                !index::MatchLiveFilename(hit, compiled, query.folders_only)) continue;
            result.hits.push_back(hit);
        }
        result.total = result.hits.size();
        AcceptIndexProviderResult(s, id, std::move(result), false);
    };
    const auto shown = [&] {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        return pane.EntryCount();
    };
    const auto submit = [&](const wchar_t* query, bool current) {
        s.addressSearchCurrent = current;
        SetWindowTextW(s.hwndAddressEdit, query);
        QueueAddressSearch(s);
        TickAddressSearch(s, s.addressLiveDue);
        return static_cast<uint32_t>(tab.pending_generation);
    };
    tab.current_path = root;
    auto folder = std::make_shared<std::vector<fs::DirEntry>>();
    for (size_t i = 0; i < 3; ++i) {
        fs::DirEntry entry;
        entry.name = indexed[i].name;
        entry.full_path = indexed[i].path;
        folder->push_back(std::move(entry));
    }
    tab.SetSnapshot(folder);
    s.addressSearching = s.addressEditing = true;
    s.addressSearchRoot = root;
    const auto scoped = submit(L"budget", true);
    check(tab.loading && shown() == 0, "entering folder search hides the unfiltered directory while pending");
    check(s.pendingIndexSearches.at(scoped).query.path_prefix == root,
        "current-folder search sends its root separately from the name query");
    publish(scoped);
    check(!tab.loading && shown() == 1 && tab.EntryAt(0).name == L"budget.txt",
        "folder search displays only matching filenames inside its scope");
    const auto all = submit(L"budget", false);
    check(tab.loading && shown() == 0 && !tab.search_retaining_results,
        "switching to all disks hides results from the previous scope");
    check(s.pendingIndexSearches.at(all).query.path_prefix.empty(),
        "all-disk search removes the current-folder restriction");
    index::SearchResult stale;
    stale.total = 1;
    stale.hits.push_back(indexed[1]);
    AcceptIndexProviderResult(s, scoped, std::move(stale), false);
    check(tab.pending_generation == all && shown() == 0, "late results cannot overwrite a new search scope");
    publish(all);
    check(shown() == 2 && tab.search_total == 2 && !tab.loading,
        "all-disk search from a folder includes a match on another drive");
    std::wstring rest;
    app::ParsePulsePath(tab.current_path, nullptr, &rest);
    const auto previous = tab.snapshot;
    RequestSearchPage(s, tab, rest, true);
    check(tab.snapshot == previous && shown() == 2,
        "refreshing the same query retains its matching results");
    publish(static_cast<uint32_t>(tab.pending_generation));
    const auto changed = submit(L"other", false);
    check(tab.loading && shown() == 0, "changing the keyword hides previous matches while pending");
    publish(changed);
    check(shown() == 1 && tab.EntryAt(0).name == L"other.txt", "changed query publishes only its own match");
    const auto empty = submit(L"missing-filename", false);
    check(shown() == 0, "no-match query does not retain unrelated rows");
    publish(empty);
    check(!tab.loading && shown() == 0 && tab.search_total == 0,
        "empty completion clears loading and displays no rows");
    const auto failed = submit(L"budget", true);
    index::SearchResult error;
    error.error = ERROR_ACCESS_DENIED;
    AcceptIndexProviderResult(s, failed, std::move(error), false);
    check(!tab.loading && shown() == 0 && tab.banner_type == app::BannerType::SearchIncomplete,
        "provider error displays its banner without old directory rows");
    const auto literal = submit(L"zhongguo", false);
    publish(literal);
    check(shown() == 1 && tab.EntryAt(0).name == L"zhongguo.txt",
        "default search matches literal filenames without pinyin expansion");
    s.appPrefs.search_pinyin = true;
    app::ParsePulsePath(tab.current_path, nullptr, &rest);
    RequestSearchPage(s, tab, rest, true);
    publish(static_cast<uint32_t>(tab.pending_generation));
    check(shown() == 2, "enabling pinyin expands the existing query to Chinese filenames");
    s.appPrefs.search_pinyin = false;
    RequestSearchPage(s, tab, rest, true);
    publish(static_cast<uint32_t>(tab.pending_generation));
    check(shown() == 1, "disabling pinyin removes transliteration matches from the existing query");
    const bool graphics = s.compositor.Init(s.hwnd);
    check(graphics, "initialize search options rendering");
    if (graphics) {
        s.compositor.RecreateTextFormats(s.scale);
        s.accentColor = ui::HexColor(0x0078D4);
        s.renderer.SetCompositor(&s.compositor);
        s.renderer.SetScale(s.scale);
        const auto shots = std::filesystem::absolute(L"../bench_data/review-changes/search-options");
        std::filesystem::create_directories(shots);
        for (bool enabled : {false, true}) {
            s.appPrefs.search_pinyin = enabled;
            s.darkMode = enabled;
            l10n::Initialize(GetModuleHandleW(nullptr), enabled ? L"zh-CN" : L"en-US");
            const auto shot = shots / (enabled ? L"pinyin-on-dark.png" : L"pinyin-off-light.png");
            SetEnvironmentVariableW(L"PULSE_TEST_OPTIONS_SHOT", shot.c_str());
            s.shot.active = true;
            ShowSearchOptions(s);
            s.shot.active = false;
            const auto* option = s.menu ? ui::FluentMenuTestPeer::PinyinOption(*s.menu) : nullptr;
            check(option && option->toggle && option->enabled && option->checked == enabled &&
                option->text == l10n::Get(l10n::StringId::SearchPinyin),
                "search settings exposes the pinyin toggle with its current state");
            check(std::filesystem::exists(shot), "search options screenshot renders");
        }
        SetEnvironmentVariableW(L"PULSE_TEST_OPTIONS_SHOT", nullptr);
    }
    DestroyWindow(s.hwnd);
    s.hwndAddressEdit = s.hwnd = nullptr;
    owned.reset();
    OleUninitialize();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
