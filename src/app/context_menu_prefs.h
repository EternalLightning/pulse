// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// context_menu_prefs.h — Explorer fusion-zone defaults + user overrides.
//
// Factory denylist lives in the category switches; per-item overrides and the
// seen catalog (grown from real right-clicks) persist to context_menu.json
// next to places.json. Host still returns the full COM list; the UI process
// applies these prefs so turning 「发送到」 back on actually works.
#pragma once
#include "../ipc/ctx_menu_util.h"
#include "builtin_menu_items.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::app {

struct SeenMenuItem {
    std::wstring key;
    std::wstring text;
    bool flyout = false;
    bool from_com = false;
    ipc::CtxMenuCategory category = ipc::CtxMenuCategory::Software;
};

struct ContextMenuPrefs {
    bool persist = true;

    // Explorer shows 软件功能 / 打开方式 / 打印 and leaves 发送到 plus the image
    // and system verbs out unless the type registers them; those two groups stay
    // off by default and the settings page turns them on. Only the COM-sourced
    // 打开方式 rows stay off — our own 打开方式… already covers them.
    bool software = true;
    bool share = false;
    bool wallpaper = false;
    bool rotate = false;
    bool shortcut = false;
    bool open_with = true;
    bool open_with_com = false;
    bool system_extra = false;
    bool print = true;

    int explorer_cap = ipc::kDefaultExplorerCap;
    int open_with_mru = 2;
    uint32_t builtin_hidden = 0;
    BuiltinMenuOrder builtin_order[static_cast<size_t>(BuiltinMenuSurface::Count)];
    // Retired NewFolder/NewTextFile APIs address their current NewMenu group.
    bool BuiltinItemEnabled(BuiltinMenuItem item) const;
    void SetBuiltinItemEnabled(BuiltinMenuItem item, bool enabled);
    bool BuiltinGroupEnabled() const;
    void SetBuiltinGroupEnabled(bool enabled);
    bool BuiltinVisible(BuiltinMenuItem item) const { return BuiltinItemEnabled(item); }
    void SetBuiltinVisible(BuiltinMenuItem item, bool enabled) { SetBuiltinItemEnabled(item, enabled); }
    BuiltinMenuOrder BuiltinOrder(BuiltinMenuSurface surface) const;
    bool BuiltinOrderCustom(BuiltinMenuSurface surface) const;
    void SetBuiltinOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& order);
    void ResetBuiltinOrder();

    std::unordered_map<std::wstring, bool> item_enabled;
    std::vector<SeenMenuItem> seen;
    std::unordered_set<std::wstring> disabled_com;

    void ResetToDefaults();
    // Removes reserved builtin identities, re-keys the catalog through CatalogKey, and carries per-item
    // overrides onto the new keys; run whenever the catalog is loaded.
    void MigrateSeenKeys();
    bool CategoryEnabled(ipc::CtxMenuCategory c) const;
    bool GroupEnabled(ipc::CtxMenuGroup g) const;
    void SetGroupEnabled(ipc::CtxMenuGroup g, bool on);
    bool ItemEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const;
    bool RowEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const;
    bool CatalogItemVisible(const std::wstring& key) const;
    std::vector<std::wstring> HiddenCatalogKeys() const;
    void SetItemEnabled(const std::wstring& key, bool on);
    bool HandlerEnabled(const std::wstring& clsid) const;
    std::vector<std::wstring> DisabledHandlerClsids() const;
    bool RecordSeen(const std::wstring& key, const std::wstring& text, bool flyout,
                    ipc::CtxMenuCategory category, bool from_com = false);
    bool ComDisabled(const std::wstring& key) const;
    void SetComDisabled(const std::wstring& key, bool on);

    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    bool Load();
    bool Save() const;

private:
    void MigrateBuiltinCatalog(uint32_t explicit_items);
    void CoalesceCompressCatalog();
};

} // namespace pulse::app
