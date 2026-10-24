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
    return !(builtin_hidden & BuiltinMenuBit(item));
}

void ContextMenuPrefs::SetBuiltinItemEnabled(BuiltinMenuItem item, bool enabled) {
    if (enabled) builtin_hidden &= ~BuiltinMenuBit(item);
    else builtin_hidden |= BuiltinMenuBit(item);
}

bool ContextMenuPrefs::BuiltinGroupEnabled() const {
    return (builtin_hidden & kBuiltinMenuMask) != kBuiltinMenuMask;
}

void ContextMenuPrefs::SetBuiltinGroupEnabled(bool enabled) {
    builtin_hidden = enabled ? 0u : kBuiltinMenuMask;
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
    const auto it = item_enabled.find(key);
    if (it != item_enabled.end()) return it->second;
    if (c == ipc::CtxMenuCategory::OpenWith && from_com) return open_with_com;
    return CategoryEnabled(c);
}

void ContextMenuPrefs::SetItemEnabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
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
    if (key.empty() || text.empty()) return false;
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
    return disabled_com.contains(key);
}

void ContextMenuPrefs::SetComDisabled(const std::wstring& key, bool on) {
    if (key.empty()) return;
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
    out += L"  },\n  \"items\":{\n";
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
    for (int index = 0; index < kBuiltinMenuItemCount; ++index) {
        const auto item = static_cast<BuiltinMenuItem>(index);
        SetBuiltinItemEnabled(item, pulse::json::ExtractBool(
            builtin_doc, std::wstring(BuiltinMenuKey(item)), true));
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

    MigrateSeenKeys();
    CoalesceCompressCatalog();

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
    return true;
}

// Older builds keyed the catalog by raw display text, so one verb showed up once
// per file type ("新建(N)" / "新建(W)", "用 X 打开" with and without spaces) and
// switching one of them off left its siblings on. Re-key every row through
// CatalogKey, merge the duplicates, and move the stored overrides onto the
// normalized keys so a switch the user already flipped keeps applying.
void ContextMenuPrefs::MigrateSeenKeys() {
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
