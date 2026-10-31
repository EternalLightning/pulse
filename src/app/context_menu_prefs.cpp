// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// context_menu_prefs.cpp — JSON load/save for the Explorer fusion-zone prefs.
#include "context_menu_prefs.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <unordered_set>
#include <windows.h>

namespace pulse::app {
namespace {

std::wstring ExtractObject(const pulse::json::Document& doc, const std::wstring& key) {
    return pulse::json::ExtractRaw(doc, key, pulse::json::Kind::Object);
}

int ClampCap(int v, int lo, int hi, int fallback) {
    if (v < lo || v > hi) return fallback;
    return v;
}

constexpr const wchar_t* kOrderKeys[] = {L"item", L"background", L"row_buttons"};
static_assert(sizeof(kOrderKeys) / sizeof(kOrderKeys[0]) == static_cast<size_t>(BuiltinMenuSurface::Count));

bool IsLegacyQuickAccessKey(std::wstring_view key) {
    return key == L"pulse:quick-access";
}

constexpr uint32_t kRetiredBuiltinBits = BuiltinMenuBit(BuiltinMenuItem::NewFolder) |
    BuiltinMenuBit(BuiltinMenuItem::NewTextFile);
constexpr uint32_t kActiveBuiltinMask = kBuiltinMenuMask & ~kRetiredBuiltinBits;

BuiltinMenuItem CurrentBuiltinItem(BuiltinMenuItem item) {
    return item == BuiltinMenuItem::NewFolder || item == BuiltinMenuItem::NewTextFile
        ? BuiltinMenuItem::NewMenu : item;
}

} // namespace

void ContextMenuPrefs::ResetToDefaults() {
    // Explorer parity for the visible groups; 发送到 and the image / system verbs
    // stay off until the user asks for them from the settings page.
    software = true;
    share = false;
    wallpaper = false;
    rotate = false;
    shortcut = false;
    open_with = true;
    open_with_com = false;
    system_extra = false;
    print = true;
    explorer_cap = ipc::kDefaultExplorerCap;
    open_with_mru = 2;
    builtin_hidden = 0;
    ResetBuiltinOrder();
    item_enabled.clear();
    seen.clear();
    disabled_com.clear();
}

bool ContextMenuPrefs::CategoryEnabled(ipc::CtxMenuCategory c) const {
    switch (c) {
    case ipc::CtxMenuCategory::Share: return share;
    case ipc::CtxMenuCategory::Wallpaper: return wallpaper;
    case ipc::CtxMenuCategory::Rotate: return rotate;
    case ipc::CtxMenuCategory::Shortcut: return shortcut;
    case ipc::CtxMenuCategory::OpenWith: return open_with;
    case ipc::CtxMenuCategory::SystemExtra: return system_extra;
    case ipc::CtxMenuCategory::Print: return print;
    default: return software;
    }
}

bool ContextMenuPrefs::BuiltinItemEnabled(BuiltinMenuItem item) const {
    return !(builtin_hidden & BuiltinMenuBit(CurrentBuiltinItem(item)));
}

void ContextMenuPrefs::SetBuiltinItemEnabled(BuiltinMenuItem item, bool enabled) {
    item = CurrentBuiltinItem(item);
    builtin_hidden &= ~kRetiredBuiltinBits;
    if (enabled) builtin_hidden &= ~BuiltinMenuBit(item);
    else builtin_hidden |= BuiltinMenuBit(item);
}

bool ContextMenuPrefs::BuiltinGroupEnabled() const {
    return (builtin_hidden & kActiveBuiltinMask) != kActiveBuiltinMask;
}

void ContextMenuPrefs::SetBuiltinGroupEnabled(bool enabled) {
    builtin_hidden = enabled ? 0u : kActiveBuiltinMask;
}

BuiltinMenuOrder ContextMenuPrefs::BuiltinOrder(BuiltinMenuSurface surface) const {
    if (surface >= BuiltinMenuSurface::Count) return {};
    return NormalizeBuiltinMenuOrder(surface, builtin_order[static_cast<size_t>(surface)]);
}

bool ContextMenuPrefs::BuiltinOrderCustom(BuiltinMenuSurface surface) const {
    return surface < BuiltinMenuSurface::Count && BuiltinOrder(surface) != BuiltinMenuDefaultOrder(surface);
}

void ContextMenuPrefs::SetBuiltinOrder(BuiltinMenuSurface surface, const BuiltinMenuOrder& order) {
    if (surface >= BuiltinMenuSurface::Count) return;
    auto normalized = NormalizeBuiltinMenuOrder(surface, order);
    if (normalized == BuiltinMenuDefaultOrder(surface)) normalized.clear();
    builtin_order[static_cast<size_t>(surface)] = std::move(normalized);
}

void ContextMenuPrefs::ResetBuiltinOrder() {
    for (auto& order : builtin_order) order.clear();
}

bool ContextMenuPrefs::GroupEnabled(ipc::CtxMenuGroup g) const {
    switch (g) {
    case ipc::CtxMenuGroup::OpenWith: return open_with;
    case ipc::CtxMenuGroup::Share: return share;
    case ipc::CtxMenuGroup::System:
        return wallpaper || rotate || shortcut || system_extra;
    case ipc::CtxMenuGroup::Print: return print;
    default: return software;
    }
}

void ContextMenuPrefs::SetGroupEnabled(ipc::CtxMenuGroup g, bool on) {
    switch (g) {
    case ipc::CtxMenuGroup::OpenWith: open_with = on; break;
    case ipc::CtxMenuGroup::Share: share = on; break;
    case ipc::CtxMenuGroup::System:
        wallpaper = rotate = shortcut = system_extra = on;
        break;
    case ipc::CtxMenuGroup::Print: print = on; break;
    default: software = on; break;
    }
}

bool ContextMenuPrefs::ItemEnabled(const std::wstring& key, ipc::CtxMenuCategory c,
                                   bool from_com) const {
    if (IsLegacyQuickAccessKey(key)) return BuiltinItemEnabled(BuiltinMenuItem::QuickAccess);
    const auto it = item_enabled.find(key);
    if (it != item_enabled.end()) return it->second;
    if (c == ipc::CtxMenuCategory::OpenWith && from_com) return open_with_com;
    return CategoryEnabled(c);
}

bool ContextMenuPrefs::RowEnabled(const std::wstring& key, ipc::CtxMenuCategory c, bool from_com) const {
    return ItemEnabled(key, c, from_com) && !ComDisabled(key);
}

bool ContextMenuPrefs::CatalogItemVisible(const std::wstring& key) const {
    if (IsLegacyQuickAccessKey(key)) return BuiltinItemEnabled(BuiltinMenuItem::QuickAccess);
    for (const auto& item : seen)
        if (item.key == key) return RowEnabled(item.key, item.category, item.from_com);
    const auto override = item_enabled.find(key);
    return (override == item_enabled.end() || override->second) && !ComDisabled(key);
}

std::vector<std::wstring> ContextMenuPrefs::HiddenCatalogKeys() const {
    std::vector<std::wstring> keys;
    for (const auto& item : seen)
        if (!IsLegacyQuickAccessKey(item.key) && !RowEnabled(item.key, item.category, item.from_com))
            keys.push_back(item.key);
    for (const auto& [key, enabled] : item_enabled)
        if (!enabled && !IsLegacyQuickAccessKey(key)) keys.push_back(key);
    for (const auto& key : disabled_com)
        if (!IsLegacyQuickAccessKey(key)) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

void ContextMenuPrefs::SetItemEnabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
    if (IsLegacyQuickAccessKey(key)) {
        SetBuiltinItemEnabled(BuiltinMenuItem::QuickAccess, on);
        MigrateBuiltinCatalog(kBuiltinMenuMask);
        return;
    }
    item_enabled[key] = on;
    if (on) SetComDisabled(key, false);
}

bool ContextMenuPrefs::HandlerEnabled(const std::wstring& clsid) const {
    if (clsid.empty()) return true;
    const std::wstring key = ipc::HandlerCatalogKey(clsid);
    if (ComDisabled(key)) return false;
    const auto it = item_enabled.find(key);
    if (it != item_enabled.end()) return it->second;
    return true;
}

std::vector<std::wstring> ContextMenuPrefs::DisabledHandlerClsids() const {
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> listed;
    auto add = [&](std::wstring clsid) {
        clsid = ipc::ToLowerVerb(clsid);
        if (clsid.empty() || !listed.insert(clsid).second) return;
        out.push_back(std::move(clsid));
    };
    for (const auto& kv : item_enabled) {
        if (!kv.second && ipc::IsHandlerCatalogKey(kv.first))
            add(ipc::HandlerClsidFromKey(kv.first));
    }
    for (const auto& key : disabled_com) {
        if (ipc::IsHandlerCatalogKey(key))
            add(ipc::HandlerClsidFromKey(key));
    }
    return out;
}

bool ContextMenuPrefs::RecordSeen(const std::wstring& key, const std::wstring& text,
                                  bool flyout, ipc::CtxMenuCategory category, bool from_com) {
    if (key.empty() || text.empty() || IsLegacyQuickAccessKey(key)) return false;
    for (const auto& item : seen)
        if (item.key == key) return false;
    if (seen.size() >= 400) return false;
    SeenMenuItem item;
    item.key = key;
    item.text = text;
    item.flyout = flyout;
    item.from_com = from_com;
    item.category = category;
    seen.push_back(std::move(item));
    return true;
}

bool ContextMenuPrefs::ComDisabled(const std::wstring& key) const {
    if (IsLegacyQuickAccessKey(key)) return !BuiltinItemEnabled(BuiltinMenuItem::QuickAccess);
    return disabled_com.contains(key);
}

void ContextMenuPrefs::SetComDisabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
    if (IsLegacyQuickAccessKey(key)) {
        SetItemEnabled(key, !on);
        return;
    }
    if (on) disabled_com.insert(key);
    else disabled_com.erase(key);
}

std::wstring ContextMenuPrefs::ToJson() const {
    std::wstring out;
    out += L"{\n  \"version\":2,\n";
    out += L"  \"explorer_cap\":";
    out += std::to_wstring(explorer_cap);
    out += L",\n  \"open_with_mru\":";
    out += std::to_wstring(open_with_mru);
    out += L",\n  \"categories\":{\n";
    auto cat = [&](const wchar_t* name, bool v, bool last) {
        out += L"    \"";
        out += name;
        out += L"\":";
        out += v ? L"true" : L"false";
        out += last ? L"\n" : L",\n";
    };
    cat(L"software", software, false);
    cat(L"share", share, false);
    cat(L"wallpaper", wallpaper, false);
    cat(L"rotate", rotate, false);
    cat(L"shortcut", shortcut, false);
    cat(L"open_with", open_with, false);
    cat(L"open_with_com", open_with_com, false);
    cat(L"system_extra", system_extra, false);
    cat(L"print", print, true);
    out += L"  },\n  \"pulse_items\":{\n";
    for (int index = 0; index < kBuiltinMenuItemCount; ++index) {
        const auto item = static_cast<BuiltinMenuItem>(index);
        out += L"    \"";
        out += BuiltinMenuKey(item);
        out += BuiltinItemEnabled(item) ? L"\":true" : L"\":false";
        out += index + 1 == kBuiltinMenuItemCount ? L"\n" : L",\n";
    }
    out += L"  },\n  \"pulse_order\":{";
    bool first_order = true;
    for (int index = 0; index < static_cast<int>(BuiltinMenuSurface::Count); ++index) {
        const auto surface = static_cast<BuiltinMenuSurface>(index);
        if (!BuiltinOrderCustom(surface)) continue;
        out += first_order ? L"\n    \"" : L",\n    \"";
        out += kOrderKeys[index];
        out += L"\":\"";
        bool first_key = true;
        for (const auto item : BuiltinOrder(surface)) {
            if (!first_key) out += L",";
            out += BuiltinMenuKey(item);
            first_key = false;
        }
        out += L"\"";
        first_order = false;
    }
    out += first_order ? L"},\n  \"items\":{\n" : L"\n  },\n  \"items\":{\n";
    size_t n = 0;
    for (const auto& kv : item_enabled) {
        std::wstring key;
        pulse::json::Escape(kv.first, key);
        out += L"    \"";
        out += key;
        out += L"\":{\"enabled\":";
        out += kv.second ? L"true" : L"false";
        out += L"}";
        ++n;
        out += (n == item_enabled.size()) ? L"\n" : L",\n";
    }
    out += L"  },\n  \"seen\":[\n";
    for (size_t i = 0; i < seen.size(); ++i) {
        const auto& item = seen[i];
        std::wstring key, text;
        pulse::json::Escape(item.key, key);
        pulse::json::Escape(item.text, text);
        out += L"    {\"key\":\"";
        out += key;
        out += L"\",\"text\":\"";
        out += text;
        out += L"\",\"kind\":\"";
        out += item.flyout ? L"flyout" : L"verb";
        out += L"\",\"category\":\"";
        out += ipc::CtxMenuCategoryId(item.category);
        out += L"\",\"source\":\"";
        out += item.from_com ? L"com" : L"static";
        out += L"\"}";
        out += (i + 1 == seen.size()) ? L"\n" : L",\n";
    }
    out += L"  ],\n  \"slow_ext\":{\n";
    size_t se = 0;
    // Retain the legacy container name for explicit-disable compatibility.
    for (const auto& disabled : disabled_com) {
        std::wstring key;
        pulse::json::Escape(disabled, key);
        out += L"    \"";
        out += key;
        out += L"\":{\"disabled\":true}";
        ++se;
        out += (se == disabled_com.size()) ? L"\n" : L",\n";
    }
    out += L"  }\n}\n";
    return out;
}

bool ContextMenuPrefs::FromJson(const std::wstring& json) {
    const pulse::json::Document doc(json);
    if (!doc.Is(pulse::json::Kind::Object)) return false;
    const auto version = pulse::json::ExtractInt(doc, L"version", 1);
    builtin_hidden = 0;
    const auto builtin = ExtractObject(doc, L"pulse_items");
    const pulse::json::Document builtin_doc(builtin);
    uint32_t explicit_builtin_items = 0;
    for (int index = 0; index < kBuiltinMenuItemCount; ++index) {
        const auto item = static_cast<BuiltinMenuItem>(index);
        if (BuiltinMenuBit(item) & kRetiredBuiltinBits) continue;
        const auto* value = builtin_doc.Find(BuiltinMenuKey(item));
        if (value && value->kind == pulse::json::Kind::Boolean)
            explicit_builtin_items |= BuiltinMenuBit(item);
        SetBuiltinItemEnabled(item, pulse::json::ExtractBool(
            builtin_doc, std::wstring(BuiltinMenuKey(item)), true));
    }
    if ((explicit_builtin_items & BuiltinMenuBit(BuiltinMenuItem::NewMenu)) == 0 &&
        !pulse::json::ExtractBool(builtin_doc, L"new_folder", true) &&
        !pulse::json::ExtractBool(builtin_doc, L"new_text", true)) {
        SetBuiltinItemEnabled(BuiltinMenuItem::NewMenu, false);
    }
    ResetBuiltinOrder();
    const auto* orders = doc.Find(L"pulse_order");
    if (orders && orders->kind == pulse::json::Kind::Object) {
        for (int index = 0; index < static_cast<int>(BuiltinMenuSurface::Count); ++index) {
            const std::wstring keys = pulse::json::String(orders->Find(kOrderKeys[index]));
            BuiltinMenuOrder order;
            for (size_t at = 0; at < keys.size();) {
                size_t end = keys.find(L',', at);
                if (end == std::wstring::npos) end = keys.size();
                const std::wstring_view key(keys.data() + at, end - at);
                for (int item = 0; item < kBuiltinMenuItemCount; ++item) {
                    const auto builtin_item = static_cast<BuiltinMenuItem>(item);
                    if (BuiltinMenuKey(builtin_item) == key) order.push_back(builtin_item);
                }
                at = end + 1;
            }
            SetBuiltinOrder(static_cast<BuiltinMenuSurface>(index), order);
        }
    }
    explorer_cap = ClampCap(pulse::json::ExtractInt(doc, L"explorer_cap", ipc::kDefaultExplorerCap),
                            1, 48, ipc::kDefaultExplorerCap);
    open_with_mru = ClampCap(pulse::json::ExtractInt(doc, L"open_with_mru", 2), 0, 8, 2);

    const std::wstring cats = ExtractObject(doc, L"categories");
    const std::wstring& src = cats.empty() ? json : cats;
    const pulse::json::Document category_doc(src);
    software = pulse::json::ExtractBool(category_doc, L"software", true);
    share = pulse::json::ExtractBool(category_doc, L"share", false);
    wallpaper = pulse::json::ExtractBool(category_doc, L"wallpaper", false);
    rotate = pulse::json::ExtractBool(category_doc, L"rotate", false);
    shortcut = pulse::json::ExtractBool(category_doc, L"shortcut", false);
    open_with = pulse::json::ExtractBool(category_doc, L"open_with", true);
    open_with_com = pulse::json::ExtractBool(category_doc, L"open_with_com", false);
    system_extra = pulse::json::ExtractBool(category_doc, L"system_extra", false);
    print = pulse::json::ExtractBool(category_doc, L"print", true);

    item_enabled.clear();
    if (const auto* items = doc.Find(L"items"); items && items->kind == pulse::json::Kind::Object) {
        for (const auto& item : items->children) {
            if (item.kind == pulse::json::Kind::Object)
                item_enabled[item.name] = pulse::json::Bool(item.Find(L"enabled"), true);
        }
    }

    seen.clear();
    for (const auto& block : pulse::json::ExtractObjectArray(doc, L"seen")) {
        const pulse::json::Document item_doc(block);
        SeenMenuItem item;
        item.key = pulse::json::ExtractString(item_doc, L"key");
        item.text = pulse::json::ExtractString(item_doc, L"text");
        item.flyout = pulse::json::ExtractString(item_doc, L"kind") == L"flyout";
        item.from_com = pulse::json::ExtractString(item_doc, L"source") == L"com";
        item.category = ipc::ParseCtxMenuCategory(pulse::json::ExtractString(item_doc, L"category"));
        if (!item.key.empty() && !item.text.empty()) seen.push_back(std::move(item));
    }

    disabled_com.clear();
    if (const auto* slow = doc.Find(L"slow_ext"); slow && slow->kind == pulse::json::Kind::Object) {
        for (const auto& item : slow->children) {
            if (item.kind != pulse::json::Kind::Object || item.name.empty()) continue;
            const bool disabled = pulse::json::Bool(item.Find(L"disabled"));
            // Version 1 treated successful replies over one second as timeouts.
            // Recover those automatic skips; explicit item overrides still apply.
            const uint64_t timeout_hits = pulse::json::Unsigned(item.Find(L"timeout"));
            if (version < 2 && timeout_hits >= 3 && timeout_hits <= UINT32_MAX) continue;
            if (disabled) disabled_com.insert(item.name);
        }
    }
    MigrateBuiltinCatalog(explicit_builtin_items);
    MigrateSeenKeys();
    CoalesceCompressCatalog();
    return true;
}

void ContextMenuPrefs::MigrateBuiltinCatalog(uint32_t explicit_items) {
    constexpr const wchar_t* key = L"pulse:quick-access";
    const auto choice = item_enabled.find(key);
    const auto bit = BuiltinMenuBit(BuiltinMenuItem::QuickAccess);
    if ((explicit_items & bit) == 0) {
        if (choice != item_enabled.end())
            SetBuiltinItemEnabled(BuiltinMenuItem::QuickAccess, choice->second);
        else if (disabled_com.contains(key))
            SetBuiltinItemEnabled(BuiltinMenuItem::QuickAccess, false);
    }
    item_enabled.erase(key);
    disabled_com.erase(key);
    std::erase_if(seen, [](const auto& item) { return IsLegacyQuickAccessKey(item.key); });
}

// Older builds keyed the catalog by raw display text, so one verb showed up once
// per file type ("新建(N)" / "新建(W)", "用 X 打开" with and without spaces) and
// switching one of them off left its siblings on. Re-key every row through
// CatalogKey, merge the duplicates, and move the stored overrides onto the
// normalized keys so a switch the user already flipped keeps applying.
void ContextMenuPrefs::MigrateSeenKeys() {
    // Only explicit Pulse identities can be retired safely. A caption-derived
    // v:/f: key may belong to an independent Shell extension with the same label.
    MigrateBuiltinCatalog(kBuiltinMenuMask);
    std::vector<SeenMenuItem> merged;
    merged.reserve(seen.size());
    for (auto& item : seen) {
        if (ipc::IsHandlerCatalogKey(item.key)) {
            merged.push_back(std::move(item));
            continue;
        }
        const std::wstring canonical = ipc::CatalogKey(item.text, item.flyout);
        if (canonical.empty()) continue;
        bool duplicate = false;
        for (const auto& kept : merged) {
            if (kept.key == canonical) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        item.key = canonical;
        merged.push_back(std::move(item));
    }
    seen = std::move(merged);

    std::unordered_map<std::wstring, bool> moved;
    moved.reserve(item_enabled.size());
    for (const auto& kv : item_enabled) {
        std::wstring key = kv.first;
        if (!ipc::IsHandlerCatalogKey(key) && key.size() > 2 && key[1] == L':' &&
            (key[0] == L'v' || key[0] == L'f')) {
            key = key.substr(0, 2) + ipc::NormalizeCatalogText(key.substr(2));
        }
        moved[key] = kv.second;
    }
    item_enabled = std::move(moved);
}

void ContextMenuPrefs::CoalesceCompressCatalog() {
    std::vector<SeenMenuItem> kept;
    kept.reserve(seen.size());
    bool compress_seen = false;
    bool compress_forced_on = false;
    bool compress_forced_off = false;
    for (auto& item : seen) {
        if (item.key == ipc::CompressCatalogKey()) {
            compress_seen = true;
            kept.push_back(std::move(item));
            continue;
        }
        if (ipc::IsCompressTopLevel(item.text) && !item.flyout) {
            const auto it = item_enabled.find(item.key);
            if (it != item_enabled.end()) {
                if (it->second) compress_forced_on = true;
                else compress_forced_off = true;
                item_enabled.erase(it);
            }
            continue;
        }
        kept.push_back(std::move(item));
    }
    if (kept.size() != seen.size() && !compress_seen) {
        SeenMenuItem row;
        row.key = ipc::CompressCatalogKey();
        row.text = ipc::CompressCatalogText();
        row.from_com = true;
        row.category = ipc::CtxMenuCategory::Software;
        kept.push_back(std::move(row));
        compress_seen = true;
    }
    seen = std::move(kept);
    if (compress_forced_on && !compress_forced_off)
        item_enabled[ipc::CompressCatalogKey()] = true;
    else if (compress_forced_off && !compress_forced_on)
        item_enabled[ipc::CompressCatalogKey()] = false;
}

bool ContextMenuPrefs::Load() {
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wstring json;
    if (!ReadUtf8File(dir + L"\\context_menu.json", json) || json.empty()) return false;
    return FromJson(json);
}

bool ContextMenuPrefs::Save() const {
    if (!persist) return true;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\context_menu.json", ToJson());
}

} // namespace pulse::app
