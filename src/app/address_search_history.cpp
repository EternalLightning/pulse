// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "app_internal.h"
#include "search_query.h"
#include "../common/localization.h"
#include "../ui/address_search_layout.h"
#include <algorithm>
#include <cwctype>

namespace pulse {
namespace {
void SaveHistory(AppState& s) {
    if (!s.searchHistory.persist) return;
    s.searchHistoryWriter.Submit(s.searchHistory);
}

bool Contains(const std::wstring& text, const std::wstring& query) {
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
        [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }) != text.end();
}

std::wstring ScopeLabel(const app::SearchHistoryEntry& entry) {
    std::wstring raw;
    app::ParsePulsePath(entry.path, nullptr, &raw);
    const auto spec = app::ParseSearchQuery(raw);
    if (spec.location == app::LocationScope::CustomFolder) return spec.custom_folder;
    if (spec.location == app::LocationScope::CurrentFolder) return spec.current_folder;
    return l10n::Get(l10n::StringId::SearchScopeAll);
}
}

void RecordSearchHistory(AppState& s, const std::wstring& path) {
    std::wstring kind, raw;
    if (!app::ParsePulsePath(path, &kind, &raw) || kind != L"search") return;
    auto query = app::SearchDisplayNeedle(raw);
    if (query.empty()) query = raw;
    if (s.searchHistory.Record(query, path)) SaveHistory(s);
}

void ShowAddressSearchHistory(AppState& s) {
    if (!s.addressSearching || s.searchHistoryOpen || !s.hwndAddressEdit || !EnsureMenu(s)) return;
    if (s.menu->IsOpen()) return;
    auto* tab = ActiveTab(s);
    auto* pane = s.pane;
    if (!tab) return;
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring draft(static_cast<size_t>(length) + 1, L'\0');
    draft.resize(GetWindowTextW(s.hwndAddressEdit, draft.data(), length + 1));
    s.searchHistoryOpen = true;
    const bool previous_ignore = s.addressIgnoreKillFocus;
    s.addressIgnoreKillFocus = true;
    do {
        auto entries = s.searchHistory.entries;
        std::wstring visible_filter;
        auto items_for = [&](const std::wstring& filter) {
            std::vector<ui::FluentMenuItem> items;
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto scope = ScopeLabel(entries[i]);
                if (!filter.empty() && !Contains(entries[i].query, filter) && !Contains(scope, filter)) continue;
                ui::FluentMenuItem item;
                item.command = 100 + static_cast<int>(i);
                item.trailing_command = 1000 + static_cast<int>(i);
                item.text = entries[i].query;
                item.glyph = L"\xE81C";
                item.shortcut = scope;
                // A full folder path must not squeeze the search term out of a narrow row.
                if (const auto slash = scope.find_last_of(L"\\/"); slash != std::wstring::npos && slash + 1 < scope.size())
                    item.shortcut = scope.substr(slash + 1);
                if (item.shortcut.size() > 14) item.shortcut = item.shortcut.substr(0, 13) + L"\u2026";
                std::wstring raw;
                app::ParsePulsePath(entries[i].path, nullptr, &raw);
                const auto& mode = l10n::Get(app::SplitSearchQueryText(raw).content.present()
                    ? l10n::StringId::SearchModeContent : l10n::StringId::SearchModeName);
                item.shortcut += L" · " + mode;
                item.tooltip = entries[i].query + L"\n" + scope + L" · " + mode;
                items.push_back(std::move(item));
            }
            if (items.empty()) {
                ui::FluentMenuItem empty;
                empty.text = l10n::Get(l10n::StringId::SearchHistoryEmpty);
                empty.enabled = false;
                items.push_back(std::move(empty));
            }
            if (!entries.empty() && filter.empty()) {
                items.back().separator_after = true;
                ui::FluentMenuItem clear;
                clear.command = 2;
                clear.secondary = true;
                clear.text = l10n::Get(l10n::StringId::SearchHistoryClear);
                clear.glyph = L"\xE74D";
                items.push_back(std::move(clear));
            }
            ui::FluentMenuItem header;
            header.text = l10n::Get(l10n::StringId::SearchHistory);
            header.enabled = false;
            items.insert(items.begin(), std::move(header));
            return items;
        };
        const auto field = ui::LayoutAddressSearch(s.renderer.SearchBarRect(
            static_cast<float>(s.compositor.Width())), s.scale).input;
        RECT anchor{static_cast<LONG>(field.left), static_cast<LONG>(field.top),
                    static_cast<LONG>(field.right), static_cast<LONG>(field.bottom)};
        MapWindowPoints(s.hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
        s.menu->SetAnchorRect(anchor);
        s.menu->SetExternalFilterEdit(s.hwndAddressEdit);
        s.menu->SetFilterMinWidth((field.right - field.left) / s.scale);
        s.menu->SetMaxVisibleRows(9);
        s.menu->SetInitialFilterText(draft);
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::SearchHistory));
        s.menu->SetHoverFirstOnOpen(false);
        s.menu->SetSelectAllOnOpen(false);
        wchar_t debug_path[32768]{};
        if (s.shot.active && s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_HISTORY_SHOT", debug_path, ARRAYSIZE(debug_path))) {
            s.menu->SetTheme(s.darkMode, s.accentColor);
            s.menu->SaveDebugSnapshot(debug_path, items_for(L""));
            break;
        }
        const auto filter_items = [&](const std::wstring& filter) {
            visible_filter = filter;
            return items_for(filter);
        };
        const auto handle_command = [&](int command) {
            if (!s.addressSearching || ActiveTab(s) != tab || s.pane != pane) return false;
            if (command == 2) {
                if (s.searchHistory.Clear()) SaveHistory(s);
            } else if (command >= 1000 && command < 1000 + static_cast<int>(entries.size())) {
                if (s.searchHistory.Remove(entries[static_cast<size_t>(command - 1000)].path)) SaveHistory(s);
            } else {
                return false;
            }
            entries = s.searchHistory.entries;
            s.menu->ReplaceFilterItems(items_for(visible_filter));
            return true;
        };
        const int command = s.menu->TrackPopup({anchor.left, anchor.top},
            items_for(L""), filter_items, false, handle_command);
        if (!s.addressSearching || ActiveTab(s) != tab || s.pane != pane) break;
        if (command >= 100 && command < 100 + static_cast<int>(entries.size())) {
            const auto path = entries[static_cast<size_t>(command - 100)].path;
            s.addressLiveDue = s.addressHistoryDue = 0;
            HideAddressEditor(s, false);
            NavigateTo(s, path);
            ShowAddressSearch(s);
            break;
        } else {
            SaveAddressSearchDraft(s);
            if (s.menu->LastFilterCommitted()) SubmitAddressSearch(s);
        }
    } while (false);
    s.addressIgnoreKillFocus = previous_ignore;
    s.searchHistoryOpen = false;
    if (s.searchScopePending) {
        s.searchScopePending = false;
        if (s.addressSearching && ActiveTab(s) == tab && s.pane == pane)
            ShowAddressSearchScope(s);
    }
}
}
