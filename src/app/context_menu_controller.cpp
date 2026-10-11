#include "context_menu_controller.h"
#include "../common/localization.h"

#include "../ipc/ctx_menu_util.h"

#include <algorithm>
#include <cwctype>
#include <unordered_set>
#include <windows.h>

namespace pulse::app {

namespace {

ShellMenuEntry ComEntry(const ops::ShellMenuItem& item, int command = 0) {
    ShellMenuEntry entry;
    entry.command = command;
    entry.text = item.text;
    entry.enabled = item.enabled;
    entry.verb = item.verb;
    entry.from_com = true;
    entry.clsid = item.clsid;
    entry.handler = item.handler;
    return entry;
}

} // namespace

void AppendShellSection(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<ShellMenuEntry>& entries) {
    if (entries.empty()) return;
    constexpr size_t kSectionCap = 48;
    const bool has_quick_access = std::any_of(items.begin(), items.end(), [](const auto& item) {
        return item.command == CmdPinQuickAccess || item.command == CmdUnpinQuickAccess;
    });
    const auto native_quick_access = [&](const ShellMenuEntry& entry) {
        const auto verb = ipc::ToLowerVerb(entry.verb);
        return has_quick_access && (verb == L"pintohome" || verb == L"unpinfromhome");
    };
    std::unordered_set<int> seen_commands;
    for (const auto& item : items)
        if (item.command != 0) seen_commands.insert(item.command);

    size_t added = 0;
    for (const auto& e : entries) {
        if (added >= kSectionCap) break;
        const bool header = !e.children.empty();
        if (e.text.empty() || (e.command == 0 && !header)) continue;
        if (native_quick_access(e) || (e.command != 0 && !seen_commands.insert(e.command).second)) continue;
        if (added == 0 && !items.empty()) items.back().separator_after = true;
        ui::FluentMenuItem row;
        row.command = e.command;
        row.text = e.text;
        row.enabled = e.enabled;
        row.live_update = true;
        // Software-owned submenu: keep the hierarchy as a one-level flyout.
        for (const auto& c : e.children) {
            if (c.text.empty() || c.command == 0 || native_quick_access(c) ||
                !seen_commands.insert(c.command).second) continue;
            ui::FluentMenuItem child;
            child.command = c.command;
            child.text = c.text;
            child.enabled = c.enabled;
            row.children.push_back(std::move(child));
        }
        if (header && row.children.empty()) continue;
        items.push_back(std::move(row));
        ++added;
    }
}

std::vector<ShellMenuEntry> ApplyExplorerPrefs(const ContextMenuPrefs& prefs,
                                               const std::vector<ShellMenuEntry>& entries) {
    std::vector<ShellMenuEntry> compress_top;
    struct Ranked {
        ShellMenuEntry entry;
        int rank = 4;
    };
    std::vector<Ranked> kept;
    kept.reserve(entries.size());
    int mru_kept = 0;
    const int mru_cap = (std::max)(0, prefs.open_with_mru);
    int vendor_index = -1;

    const bool compress_on = prefs.ItemEnabled(ipc::CompressCatalogKey(),
        ipc::CtxMenuCategory::Software, true);

    for (const auto& e : entries) {
        if (e.text.empty()) continue;
        if (!e.clsid.empty() && !prefs.HandlerEnabled(e.clsid)) continue;
        const bool flyout = !e.children.empty();
        if (!flyout && ipc::IsCompressTopLevel(e.text)) {
            if (compress_on) compress_top.push_back(e);
            continue;
        }
        const auto cat = ipc::ClassifyExplorerItem(e.verb, e.text, flyout);
        const std::wstring key = !e.clsid.empty()
            ? ipc::HandlerCatalogKey(e.clsid)
            : ipc::CatalogKey(e.text, flyout);
        if (!prefs.ItemEnabled(key, cat, e.from_com)) continue;
        if (cat == ipc::CtxMenuCategory::OpenWith && ipc::IsOpenWithMruText(e.text)) {
            if (mru_kept >= mru_cap) continue;
            ++mru_kept;
        }
        // Explorer's order: vendor flyouts, then software verbs, 打开方式, 打印,
        // and 发送到 / image / system verbs last. Ranking by category instead of
        // "is a flyout" keeps the 发送到 submenu from jumping above the software
        // section now that the share group defaults to on.
        int rank = 4;
        if (cat == ipc::CtxMenuCategory::Software) rank = flyout ? 0 : 1;
        else if (cat == ipc::CtxMenuCategory::OpenWith) rank = 2;
        else if (cat == ipc::CtxMenuCategory::Print) rank = 3;
        if (flyout && ipc::IsCompressVendorFlyout(e.text))
            vendor_index = static_cast<int>(kept.size());
        kept.push_back({ e, rank });
    }

    auto append_compress_children = [&](ShellMenuEntry& dest) {
        for (auto& row : compress_top) {
            if (dest.children.size() >= static_cast<size_t>(ipc::kMaxSubmenuChildren)) break;
            bool dup = false;
            for (const auto& c : dest.children)
                if (ipc::ToLowerVerb(c.text) == ipc::ToLowerVerb(row.text)) {
                    dup = true;
                    break;
                }
            if (!dup) dest.children.push_back(std::move(row));
        }
        compress_top.clear();
    };

    if (!compress_top.empty() && vendor_index >= 0) {
        append_compress_children(kept[static_cast<size_t>(vendor_index)].entry);
    } else if (!compress_top.empty()) {
        ShellMenuEntry group;
        group.text = ipc::CompressFlyoutText();
        group.from_com = true;
        append_compress_children(group);
        if (!group.children.empty())
            kept.insert(kept.begin(), { std::move(group), 0 });
    }

    std::stable_sort(kept.begin(), kept.end(),
                     [](const Ranked& a, const Ranked& b) { return a.rank < b.rank; });

    std::vector<ShellMenuEntry> out;
    const size_t cap = static_cast<size_t>((std::max)(1, prefs.explorer_cap));
    out.reserve((std::min)(kept.size(), cap));
    for (auto& row : kept) {
        if (out.size() >= cap) break;
        out.push_back(std::move(row.entry));
    }
    return out;
}

std::wstring ContextMenuController::CacheKey(bool background,
                                             const std::wstring& extension,
                                             bool extended) {
    const std::wstring key = background ? L":bg" : extension.empty() ? L":file" : extension;
    return extended ? key + L":extended" : key;
}

void ContextMenuController::StartQuery(
    ContextMenuPrefs& prefs, HWND hwnd,
    std::vector<std::wstring> paths, bool background, std::wstring extension,
    bool extended,
    const std::function<std::wstring(const std::wstring&)>& normalize_path,
    const std::function<void(const std::wstring&)>& prefetch_static,
    uint64_t context_generation) {
    if (paths.empty()) return;
    if (normalize_path) {
        for (auto& path : paths) path = normalize_path(path);
    }
    if (token_ != 0 && background_ == background && extended_ == extended && paths_ == paths &&
        context_generation_ == context_generation &&
        GetTickCount64() - query_started_at_ < ipc::kContextMenuSessionLifetimeMs) {
        if (prefetch_static) prefetch_static(extension);
        return;
    }
    Close();
    uint32_t token = 0;
    if (operations_.query)
        token = operations_.query(paths, hwnd, background, extended,
                                  prefs.DisabledHandlerClsids());
    paths_ = std::move(paths);
    background_ = background;
    extended_ = extended;
    extension_ = std::move(extension);
    if (background && extension_.empty()) extension_ = ipc::kBackgroundVerbKey;
    token_ = token;
    context_generation_ = context_generation;
    com_ready_ = false;
    com_items_.clear();
    query_started_at_ = token == 0 ? 0 : GetTickCount64();
    static_verbs_.clear();
    if (const auto found = static_cache_.find(extension_); found != static_cache_.end())
        static_verbs_ = found->second;
    if (prefetch_static) prefetch_static(extension_);
}

void ContextMenuController::Close() {
    const uint32_t live = token_;
    token_ = 0;
    paths_.clear();
    background_ = false;
    extended_ = false;
    com_ready_ = false;
    com_items_current_ = false;
    com_items_.clear();
    menu_com_items_.clear();
    menu_layout_com_items_.clear();
    menu_com_token_ = 0;
    static_verbs_.clear();
    extension_.clear();
    query_started_at_ = 0;
    context_generation_ = 0;
    if (live && operations_.close) operations_.close(live);
}

void ContextMenuController::WaitUntilReady(
    HWND hwnd, const std::function<void()>& pump_messages, uint32_t timeout_ms) {
    if (!hwnd || token_ == 0 || com_ready_) return;
    if (pump_messages) pump_messages();
    if (com_ready_) return;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (!com_ready_) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE,
                                  static_cast<DWORD>(deadline - now), QS_ALLINPUT);
        if (pump_messages) pump_messages();
    }
}

std::vector<ui::FluentMenuItem> ContextMenuController::BuildDisplay(
    ContextMenuPrefs& prefs, const std::vector<ui::FluentMenuItem>& base_items,
    bool& prefs_changed) const {
    std::vector<ui::FluentMenuItem> display = base_items;
    auto entries = ApplyExplorerPrefs(prefs, ComposeEntries(prefs, prefs_changed));
    for (auto& row : display) {
        if (row.command != CmdNewMenu) continue;
        const size_t builtin_count = row.children.size();
        for (auto it = entries.begin(); it != entries.end();) {
            if (!ipc::IsNativeNewMenuHandler(it->clsid) || it->children.empty()) { ++it; continue; }
            for (const auto& entry : it->children) {
                if (!entry.command || ipc::IsPulseNewTemplateVerb(entry.verb)) continue;
                ui::FluentMenuItem child;
                child.command = entry.command;
                child.text = entry.text;
                child.enabled = entry.enabled;
                row.children.push_back(std::move(child));
            }
            it = entries.erase(it);
        }
        if (row.children.size() > builtin_count && builtin_count)
            row.children[builtin_count - 1].separator_after = true;
    }
    if (new_dropdown_ && !display.empty()) {
        auto children = std::move(display.front().children);
        for (auto& child : children) child.live_update = true;
        return children;
    }
    AppendShellSection(display, entries);
    ui::FluentMenuItem manage;
    manage.command = CmdSettingsContextMenu;
    manage.live_update = true;
    manage.text = pulse::l10n::Get(pulse::l10n::StringId::ManageContext);
    manage.glyph = L"\xE713";
    if (!display.empty()) display.back().separator_after = true;
    display.push_back(std::move(manage));
    return display;
}

bool ContextMenuController::ExecuteShellCommand(
    int command,
    const std::function<void()>& wait_until_ready,
    const std::function<void()>& refresh_folder) {
    if (command >= CmdShellComBase) {
        const uint32_t clicked = static_cast<uint32_t>(command - CmdShellComBase);
        if (!com_items_current_ && wait_until_ready) wait_until_ready();
        if (const auto* item = FindInvokableComItem(clicked)) {
            if (operations_.invoke) operations_.invoke(token_, item->id, item->verb, item->text);
            token_ = 0;
        }
        Close();
        if (refresh_folder) refresh_folder();
        return true;
    }
    if (command >= CmdShellStaticBase && command < CmdShellComBase) {
        const int packed = command - CmdShellStaticBase;
        const size_t index = static_cast<size_t>(packed / ipc::kStaticVerbStride);
        const int child = packed % ipc::kStaticVerbStride;
        if (index < static_verbs_.size()) {
            const StaticVerb* verb = &static_verbs_[index];
            if (child > 0) {
                const size_t ci = static_cast<size_t>(child - 1);
                if (ci < verb->children.size()) verb = &verb->children[ci];
                else verb = nullptr;
            }
            if (verb) {
                for (size_t i = 0; i < paths_.size(); ++i) {
                    if (_wcsicmp(verb->verb.c_str(), L"runas") == 0) {
                        // The registry command alone does not request elevation.
                        // Keep the verb so ShellExecuteEx performs the UAC handoff.
                        if (operations_.execute_verb)
                            operations_.execute_verb(paths_[i], L"runas");
                    } else if (!verb->app_path.empty()) {
                        if (operations_.open_with_app)
                            operations_.open_with_app(verb->app_path, paths_[i]);
                    } else if (!verb->command.empty()) {
                        if (operations_.execute_command)
                            operations_.execute_command(verb->command, paths_[i]);
                    } else if (operations_.execute_verb) {
                        operations_.execute_verb(paths_[i], verb->verb);
                    }
                    if (verb->verb == L"openas") break;
                }
            }
        }
        Close();
        if (refresh_folder) refresh_folder();
        return true;
    }
    if (!background_) Close();
    return false;
}

bool ContextMenuController::RequestStaticPrefetch(const std::wstring& extension) {
    if (extension.empty() || static_pending_.contains(extension)) return false;
    const auto cached = static_cache_.find(extension);
    // Rows seeded from the on-disk cache still get one live pass: the cache can
    // be older than the enumeration code (that is how 打开方式 went missing).
    if (cached != static_cache_.end() && !static_seeded_.contains(extension)) return false;
    static_pending_.insert(extension);
    return true;
}

void ContextMenuController::MergeStaticCache(
    std::unordered_map<std::wstring, std::vector<StaticVerb>> entries) {
    for (auto& [extension, verbs] : entries) {
        if (static_cache_.contains(extension)) continue;
        // Rows with empty display text (written by a build without resources)
        // would be dropped by the menu anyway; drop them here and let the live
        // enumeration replace the rest.
        verbs.erase(std::remove_if(verbs.begin(), verbs.end(),
                                   [](const StaticVerb& v) { return v.display.empty(); }),
                    verbs.end());
        static_cache_.emplace(extension, std::move(verbs));
        static_seeded_.insert(std::move(extension));
    }
}

bool ContextMenuController::CompleteStaticVerbs(const std::wstring& extension,
                                                std::vector<StaticVerb> verbs) {
    static_pending_.erase(extension);
    static_seeded_.erase(extension);
    static_cache_[extension] = std::move(verbs);
    if (extension_ != extension || menu_open_) return false;
    static_verbs_ = static_cache_[extension];
    return true;
}

void ContextMenuController::InvalidateCaches() {
    Close();
    static_cache_.clear();
    static_seeded_.clear();
    com_cache_.clear();
    static_pending_.clear();
}

bool ContextMenuController::SeedComItemsFromCache() {
    if (com_ready_ || !com_items_.empty()) return false;
    const auto found = com_cache_.find(CacheKey(background_, extension_, extended_));
    if (found == com_cache_.end() || found->second.empty()) return false;
    com_items_ = found->second;
    com_items_current_ = false;
    return true;
}

ContextMenuController::QueryCompletion ContextMenuController::CompleteComQuery(
    uint32_t token, std::vector<ops::ShellMenuItem> items, uint64_t completed_at,
    bool partial) {
    QueryCompletion result;
    if (token == 0 || token != token_) return result;
    com_items_ = std::move(items);
    com_items_current_ = true;
    result.accepted = true;
    result.partial = partial;
    result.cache_key = CacheKey(background_, extension_, extended_);
    com_cache_[result.cache_key] = com_items_;
    if (!partial) {
        com_ready_ = true;
        if (query_started_at_ != 0 && completed_at >= query_started_at_) {
            result.elapsed_ms = static_cast<uint32_t>(std::min<uint64_t>(
                completed_at - query_started_at_, UINT32_MAX));
        }
    }
    return result;
}

std::vector<ShellMenuEntry> ContextMenuController::ComposeEntries(
    ContextMenuPrefs& prefs, bool& prefs_changed) const {
    std::vector<ShellMenuEntry> entries;
    const auto displayed_com = DisplayedComItems();
    entries.reserve(static_verbs_.size() + com_items_.size());
    for (size_t i = 0; i < static_verbs_.size() && i < static_cast<size_t>(ipc::kMaxStaticVerbParents);
         ++i) {
        const auto& verb = static_verbs_[i];
        ShellMenuEntry entry;
        entry.text = verb.display;
        entry.verb = verb.verb;
        if (!verb.children.empty()) {
            entry.command = 0;
            const size_t n = (std::min)(verb.children.size(),
                                        static_cast<size_t>(ipc::kMaxSubmenuChildren));
            for (size_t j = 0; j < n; ++j) {
                ShellMenuEntry child;
                child.command = CmdShellStaticBase +
                    static_cast<int>(i) * ipc::kStaticVerbStride + static_cast<int>(j) + 1;
                child.text = verb.children[j].display;
                child.verb = verb.children[j].verb;
                entry.children.push_back(std::move(child));
            }
        } else {
            entry.command = CmdShellStaticBase +
                static_cast<int>(i) * ipc::kStaticVerbStride;
        }
        entries.push_back(std::move(entry));
    }
    for (size_t i = 0; i < displayed_com.size(); ++i) {
        const auto& item = displayed_com[i];
        if (item.child) continue;
        if (item.has_children) {
            ShellMenuEntry parent = ComEntry(item);
            parent.enabled = parent.enabled && com_items_current_;
            for (size_t j = i + 1; j < displayed_com.size() && displayed_com[j].child; ++j) {
                if (displayed_com[j].id > 0x7FFF) continue;
                parent.children.push_back(ComEntry(displayed_com[j],
                    CmdShellComBase + static_cast<int>(displayed_com[j].id)));
                parent.children.back().enabled = parent.children.back().enabled && com_items_current_;
            }
            if (!parent.children.empty()) entries.push_back(std::move(parent));
            continue;
        }
        if (item.id > 0x7FFF) continue;
        entries.push_back(ComEntry(item, CmdShellComBase + static_cast<int>(item.id)));
        entries.back().enabled = entries.back().enabled && com_items_current_;
    }

    prefs_changed = false;
    std::unordered_set<std::wstring> recorded_handlers;
    for (const auto& entry : entries) {
        const bool flyout = !entry.children.empty();
        if (!flyout && ipc::IsCompressTopLevel(entry.text)) {
            if (prefs.RecordSeen(ipc::CompressCatalogKey(), ipc::CompressCatalogText(),
                                 false, ipc::CtxMenuCategory::Software, entry.from_com))
                prefs_changed = true;
            continue;
        }
        if (flyout && entry.clsid.empty() &&
            ipc::ToLowerVerb(entry.text) == ipc::ToLowerVerb(ipc::CompressFlyoutText())) {
            if (prefs.RecordSeen(ipc::CompressCatalogKey(), ipc::CompressCatalogText(),
                                 true, ipc::CtxMenuCategory::Software, true))
                prefs_changed = true;
            continue;
        }
        if (!entry.clsid.empty()) {
            if (!recorded_handlers.insert(ipc::ToLowerVerb(entry.clsid)).second)
                continue;
            const std::wstring name = !entry.text.empty() ? entry.text : entry.handler;
            const auto category = ipc::ClassifyExplorerItem(entry.verb, entry.text, flyout);
            if (prefs.RecordSeen(ipc::HandlerCatalogKey(entry.clsid), name, flyout,
                                 category, true))
                prefs_changed = true;
            continue;
        }
        const auto category = ipc::ClassifyExplorerItem(entry.verb, entry.text, flyout);
        if (prefs.RecordSeen(ipc::CatalogKey(entry.text, flyout), entry.text, flyout,
                             category, entry.from_com)) {
            prefs_changed = true;
        }
    }
    return entries;
}

const ops::ShellMenuItem* ContextMenuController::FindComItem(uint32_t id) const {
    return FindComItemIn(com_items_, id);
}

const ops::ShellMenuItem* ContextMenuController::FindComItemIn(
    const std::vector<ops::ShellMenuItem>& items, uint32_t id) const {
    const ops::ShellMenuItem* match = nullptr;
    for (const auto& item : items) {
        if (item.id != id) continue;
        if (match) return nullptr;
        match = &item;
    }
    return match;
}

const ops::ShellMenuItem* ContextMenuController::FindInvokableComItem(uint32_t id) const {
    // Cached rows describe presentation; only a displayed snapshot of this session authorizes execution.
    if (!token_ || menu_com_token_ != token_ || !com_items_current_ ||
        id == 0 || id > 0x7FFF) return nullptr;
    const auto* snapshot = FindComItemIn(menu_com_items_, id);
    const auto* live = FindComItem(id);
    if (!snapshot || !live || !snapshot->enabled || snapshot->has_children ||
        !live->enabled || live->has_children || snapshot->clsid != live->clsid ||
        snapshot->handler != live->handler || snapshot->verb != live->verb ||
        snapshot->text != live->text) return nullptr;
    return live;
}

void ContextMenuController::OpenMenu(std::vector<ui::FluentMenuItem> base_items, bool new_dropdown) {
    new_dropdown_ = new_dropdown;
    if (const auto found = static_cache_.find(extension_); found != static_cache_.end())
        static_verbs_ = found->second;
    base_items_ = std::move(base_items);
    menu_layout_com_items_ = com_items_;
    menu_open_ = true;
    NotePatchedDisplay();
}

std::vector<ops::ShellMenuItem> ContextMenuController::DisplayedComItems() const {
    if (!menu_open_) return com_items_;
    auto displayed = menu_layout_com_items_;
    std::unordered_set<uint32_t> used_ids;
    for (const auto& item : com_items_) if (item.id) used_ids.insert(item.id);
    uint32_t placeholder = 0x7FFF;
    for (auto& row : displayed) {
        const ops::ShellMenuItem* match = nullptr;
        bool ambiguous = false;
        if (com_items_current_) {
            for (const auto& live : com_items_) {
                if (row.clsid != live.clsid || row.handler != live.handler || row.verb != live.verb ||
                    row.text != live.text || row.child != live.child || row.has_children != live.has_children) continue;
                if (match) { ambiguous = true; break; }
                match = &live;
            }
        }
        if (match && !ambiguous) {
            const bool separator = row.separator_after;
            row = *match;
            row.separator_after = separator;
        } else {
            row.enabled = false;
            // Disabled cached rows still need unique presentation commands when
            // a current handler has reused their old numeric range.
            if (row.id && !row.has_children) {
                while (placeholder && used_ids.contains(placeholder)) --placeholder;
                if (placeholder) { row.id = placeholder; used_ids.insert(placeholder--); }
            }
        }
    }
    return displayed;
}

void ContextMenuController::NotePatchedDisplay() {
    menu_com_items_ = DisplayedComItems();
    menu_com_token_ = com_items_current_ ? token_ : 0;
}

bool ContextMenuController::UpdateCommandState(int command, const std::wstring& text, bool enabled) {
    bool changed = false;
    for (auto& item : base_items_) {
        if (item.command != command || (item.text == text && item.enabled == enabled)) continue;
        item.text = text;
        item.enabled = enabled;
        changed = true;
    }
    return changed;
}

void ContextMenuController::ScheduleFolderRefresh(uint64_t now) noexcept {
    refresh_at_ = now + 400;
    refresh_again_at_ = now + 1600;
}

int ContextMenuController::ConsumeDueRefreshes(uint64_t now) noexcept {
    int due = 0;
    if (refresh_at_ != 0 && now >= refresh_at_) {
        refresh_at_ = 0;
        ++due;
    }
    if (refresh_again_at_ != 0 && now >= refresh_again_at_) {
        refresh_again_at_ = 0;
        ++due;
    }
    return due;
}

} // namespace pulse::app
