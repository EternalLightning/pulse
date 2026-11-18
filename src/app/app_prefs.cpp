// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// app_prefs.cpp — Persist general settings; sync 开机自启 with the Run key.
#include "app_prefs.h"
#include "default_file_manager.h"
#include "session.h"
#include "startup_launch.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <cwctype>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace pulse::app {
namespace {

constexpr const wchar_t* kRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"Pulse";

std::wstring ExePath() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return {};
    path.resize(n);
    return path;
}

std::wstring ReadStartupCommand() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    std::wstring command(32768, L'\0');
    DWORD bytes = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    DWORD type = 0;
    const LSTATUS result = RegQueryValueExW(key, kRunValue, nullptr, &type,
        reinterpret_cast<BYTE*>(command.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        bytes % sizeof(wchar_t) != 0) return {};
    command.resize(bytes / sizeof(wchar_t));
    if (!command.empty() && command.back() == L'\0') command.pop_back();
    if (command.find(L'\0') != std::wstring::npos) return {};
    return command;
}

} // namespace

namespace {

void DeleteStoredWallpapers(const std::wstring& dir) {
    static constexpr const wchar_t* kExt[] = {
        L".jpg", L".jpeg", L".png", L".bmp", L".webp", L".jfif", L".img"
    };
    for (const wchar_t* ext : kExt)
        DeleteFileW((dir + L"\\wallpaper" + ext).c_str());
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

} // namespace

void AppPrefs::ResetToDefaults() {
    saves_enabled_ = true;
    launch_on_startup = false;
    start_to_tray = false;
    close_last_tab_window = false;
    keep_running_on_close = false;
    open_folders_in_pulse = false;
    take_over_win_e = false;
    take_over_this_pc = false;
    experimental_explorer_takeover = false;
    file_dialog_bridge_registered = false;
    verify_copies = false;
    show_status_performance = false;
    text_compare_enabled = true;
    show_pinned_tab_names = true;
    new_tab_home = false;
    home_quick_access = {};
    external_open_mode = ExternalOpenMode::ReuseOpenedLocation;
    list_smart_date = true;
    list_zebra_rows = true;
    list_size_bar = false;
    auto_folder_size = true;
    folder_sort_mode = 0;
    folder_views.Clear();
    search_pinyin = false;
    global_search_enabled = false;
    global_search_modifiers = 1;
    global_search_key = 32;
    command_shortcuts.clear();
    toolbar_order = {186, 2, 3, 4, 6, 5, 7, 306};
    show_hidden_files = false;
    show_protected_os_files = false;
    blank_click_go_back = false;
    change_tracking_enabled = false;
    change_tracking_days = 7;
    theme_mode = -1;
    language = L"system";
    window_effect = L"mica-alt";
    background_image.clear();
    lock_window_size = false;
    wallpaper_visibility = 50;
    wallpaper_blur = 1;
    row_height = 34;
    text_render = 0;
    sidebar_width = 224;
    address_search_current = false;
    address_search_content = false;
    staging_tray_enabled = true;
    accent_rgb.clear();
    accent_follow_system = false;
    custom_tag_colors.clear();
    duplicate_scan_scope = 0;
    duplicate_scan_folder.clear();
    duplicate_scan_drive.clear();
}

std::wstring AppPrefs::ToJson() const {
    std::wstring escaped_effect;
    std::wstring escaped_image;
    std::wstring escaped_language;
    pulse::json::Escape(window_effect, escaped_effect);
    pulse::json::Escape(background_image, escaped_image);
    pulse::json::Escape(language, escaped_language);
    std::wstring out = L"{\n  \"version\":4,\n  \"launch_on_startup\":";
    out += launch_on_startup ? L"true" : L"false";
    out += L",\n  \"start_to_tray\":";
    out += start_to_tray ? L"true" : L"false";
    out += L",\n  \"close_last_tab_window\":";
    out += close_last_tab_window ? L"true" : L"false";
    out += L",\n  \"keep_running_on_close\":";
    out += keep_running_on_close ? L"true" : L"false";
    out += L",\n  \"open_folders_in_pulse\":";
    out += open_folders_in_pulse ? L"true" : L"false";
    out += L",\n  \"take_over_win_e\":";
    out += take_over_win_e ? L"true" : L"false";
    out += L",\n  \"take_over_this_pc\":";
    out += take_over_this_pc ? L"true" : L"false";
    out += L",\n  \"experimental_explorer_takeover\":";
    out += experimental_explorer_takeover ? L"true" : L"false";
    out += L",\n  \"verify_copies\":";
    out += verify_copies ? L"true" : L"false";
    out += L",\n  \"show_status_performance\":";
    out += show_status_performance ? L"true" : L"false";
    out += L",\n  \"text_compare_enabled\":";
    out += text_compare_enabled ? L"true" : L"false";
    out += L",\n  \"show_pinned_tab_names\":";
    out += show_pinned_tab_names ? L"true" : L"false";
    out += L",\n  \"new_tab_home\":";
    out += new_tab_home ? L"true" : L"false";
    out += L",\n  \"external_open_mode\":" + std::to_wstring(static_cast<int>(external_open_mode));
    out += L",\n  \"list_smart_date\":";
    out += list_smart_date ? L"true" : L"false";
    out += L",\n  \"list_zebra_rows\":";
    out += list_zebra_rows ? L"true" : L"false";
    out += L",\n  \"list_size_bar\":";
    out += list_size_bar ? L"true" : L"false";
    out += L",\n  \"auto_folder_size\":";
    out += auto_folder_size ? L"true" : L"false";
    out += L",\n  \"folder_sort_mode\":";
    out += std::to_wstring(folder_sort_mode >= 0 && folder_sort_mode <= 2 ? folder_sort_mode : 0);
    out += L",\n  \"show_hidden_files\":";
    out += show_hidden_files ? L"true" : L"false";
    out += L",\n  \"show_protected_os_files\":";
    out += show_protected_os_files ? L"true" : L"false";
    out += L",\n  \"search_pinyin\":";
    out += search_pinyin ? L"true" : L"false";
    out += L",\n  \"global_search_enabled\":";
    out += global_search_enabled ? L"true" : L"false";
    out += L",\n  \"global_search_modifiers\":" + std::to_wstring(global_search_modifiers);
    out += L",\n  \"global_search_key\":" + std::to_wstring(global_search_key);
    out += L",\n  \"command_shortcuts\":[";
    for (size_t i = 0; i < command_shortcuts.size(); ++i) {
        const auto& entry = command_shortcuts[i];
        if (i) out += L",";
        out += L"{\"command\":" + std::to_wstring(entry.command) +
            L",\"key\":" + std::to_wstring(entry.shortcut.key) +
            L",\"ctrl\":" + (entry.shortcut.ctrl ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"shift\":" + (entry.shortcut.shift ? std::wstring(L"true") : std::wstring(L"false")) +
            L",\"alt\":" + (entry.shortcut.alt ? std::wstring(L"true") : std::wstring(L"false")) + L"}";
    }
    out += L"],\n  \"toolbar_order\":[";
    for (size_t i = 0; i < toolbar_order.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(toolbar_order[i]);
    }
    out += L"]";
    out += L",\n  \"blank_click_go_back\":";
    out += blank_click_go_back ? L"true" : L"false";
    out += L",\n  \"change_tracking_enabled\":";
    out += change_tracking_enabled ? L"true" : L"false";
    out += L",\n  \"change_tracking_days\":";
    out += std::to_wstring(change_tracking_days == 1 || change_tracking_days == 3 ? change_tracking_days : 7);
    out += L",\n  \"theme_mode\":";
    out += std::to_wstring(theme_mode);
    out += L",\n  \"language\":\"";
    out += escaped_language;
    out += L"\"";
    out += L",\n  \"window_effect\":\"";
    out += escaped_effect;
    out += L"\",\n  \"background_image\":\"";
    out += escaped_image;
    out += L"\",\n  \"lock_window_size\":";
    out += lock_window_size ? L"true" : L"false";
    out += L",\n  \"row_height\":";
    out += std::to_wstring(row_height);
    out += L",\n  \"text_render\":";
    out += std::to_wstring(text_render >= 0 && text_render <= 2 ? text_render : 0);
    out += L",\n  \"sidebar_width\":" + std::to_wstring(sidebar_width);
    out += L",\n  \"address_search_current\":" + std::to_wstring(address_search_current);
    out += L",\n  \"address_search_content\":" + std::to_wstring(address_search_content);
    out += L",\n  \"staging_tray_enabled\":";
    out += staging_tray_enabled ? L"true" : L"false";
    out += L",\n  \"wallpaper_visibility\":";
    out += std::to_wstring(wallpaper_visibility);
    out += L",\n  \"wallpaper_blur\":";
    out += std::to_wstring(wallpaper_blur);
    out += L",\n  \"accent_rgb\":\"";
    {
        std::wstring escaped_accent;
        pulse::json::Escape(accent_rgb, escaped_accent);
        out += escaped_accent;
    }
    out += L"\",\n  \"accent_follow_system\":" + std::to_wstring(accent_follow_system);
    out += L",\n  \"custom_tag_colors\":[";
    for (size_t i = 0; i < custom_tag_colors.size(); ++i) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", custom_tag_colors[i] & 0x00FFFFFFu);
        if (i > 0) out += L",";
        out += L"\"";
        out += hex;
        out += L"\"";
    }
    out += L"],\n  \"duplicate_scan_scope\":";
    out += std::to_wstring(duplicate_scan_scope);
    out += L",\n  \"duplicate_scan_folder\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_folder, escaped);
        out += escaped;
    }
    out += L"\",\n  \"duplicate_scan_drive\":\"";
    {
        std::wstring escaped;
        pulse::json::Escape(duplicate_scan_drive, escaped);
        out += escaped;
    }
    out += L"\"";
    out += L",\n  \"home_quick_access\":{\"hidden\":" +
        std::to_wstring(home_quick_access.hidden & model::kHomeQuickAccessHiddenMask) + L",\"folders\":[";
    for (size_t i = 0; i < home_quick_access.folders.size(); ++i) {
        std::wstring escaped;
        pulse::json::Escape(home_quick_access.folders[i], escaped);
        if (i) out += L",";
        out += L"\"" + escaped + L"\"";
    }
    out += L"]}";
    folder_views.AppendJson(out);
    out += L"\n}\n";
    return out;
}

bool AppPrefs::FromJson(const std::wstring& json) {
    const pulse::json::Document doc(json);
    if (!doc.Is(pulse::json::Kind::Object)) return false;
    saves_enabled_ = true;
    folder_views.ReadJson(json);
    launch_on_startup = pulse::json::ExtractBool(doc, L"launch_on_startup", false);
    start_to_tray = pulse::json::ExtractBool(doc, L"start_to_tray", false);
    close_last_tab_window = pulse::json::ExtractBool(doc, L"close_last_tab_window", false);
    keep_running_on_close = pulse::json::ExtractBool(doc, L"keep_running_on_close", false);
    open_folders_in_pulse = pulse::json::ExtractBool(doc, L"open_folders_in_pulse", false);
    take_over_win_e = pulse::json::ExtractBool(doc, L"take_over_win_e", false);
    take_over_this_pc = pulse::json::ExtractBool(doc, L"take_over_this_pc", false);
    experimental_explorer_takeover = pulse::json::ExtractBool(doc, L"experimental_explorer_takeover", false);
    verify_copies = pulse::json::ExtractBool(doc, L"verify_copies", false);
    show_status_performance = pulse::json::ExtractBool(doc, L"show_status_performance", false);
    text_compare_enabled = pulse::json::ExtractBool(doc, L"text_compare_enabled", true);
    show_pinned_tab_names = pulse::json::ExtractBool(doc, L"show_pinned_tab_names", true);
    new_tab_home = pulse::json::ExtractBool(doc, L"new_tab_home", false);
    home_quick_access = {};
    if (const auto* home = doc.Find(L"home_quick_access"); home && home->kind == pulse::json::Kind::Object) {
        const int hidden = pulse::json::Int(home->Find(L"hidden"));
        home_quick_access.hidden = hidden >= 0 ? static_cast<uint32_t>(hidden) & model::kHomeQuickAccessHiddenMask : 0;
        if (const auto* folders = home->Find(L"folders"); folders && folders->kind == pulse::json::Kind::Array) {
            for (const auto& folder : folders->children)
                if (folder.kind == pulse::json::Kind::String) home_quick_access.AddFolder(folder.text);
        }
    }
    const int external_mode = pulse::json::ExtractInt(doc, L"external_open_mode", 0);
    external_open_mode = external_mode >= 0 && external_mode <= 2
        ? static_cast<ExternalOpenMode>(external_mode) : ExternalOpenMode::ReuseOpenedLocation;
    list_smart_date = pulse::json::ExtractBool(doc, L"list_smart_date", true);
    list_zebra_rows = pulse::json::ExtractBool(doc, L"list_zebra_rows", true);
    list_size_bar = pulse::json::ExtractBool(doc, L"list_size_bar", false);
    auto_folder_size = pulse::json::ExtractBool(doc, L"auto_folder_size", true);
    folder_sort_mode = pulse::json::ExtractInt(doc, L"folder_sort_mode", 0);
    if (folder_sort_mode < 0 || folder_sort_mode > 2) folder_sort_mode = 0;
    search_pinyin = pulse::json::ExtractBool(doc, L"search_pinyin", false);
    global_search_enabled = pulse::json::ExtractBool(doc, L"global_search_enabled", false);
    const int modifiers = pulse::json::ExtractInt(doc, L"global_search_modifiers", 1);
    const int key = pulse::json::ExtractInt(doc, L"global_search_key", 32);
    global_search_modifiers = modifiers > 0 && modifiers <= 15 ? static_cast<uint32_t>(modifiers) : 1;
    global_search_key = key > 0 && key <= 254 ? static_cast<uint32_t>(key) : 32;
    command_shortcuts.clear();
    if (const auto* entries = doc.Find(L"command_shortcuts"); entries && entries->kind == pulse::json::Kind::Array) {
        for (const auto& entry : entries->children) {
            if (entry.kind != pulse::json::Kind::Object) continue;
            const int command = pulse::json::Int(entry.Find(L"command"));
            const int shortcut_key = pulse::json::Int(entry.Find(L"key"), -1);
            if (command <= 0 || command >= 6000 || shortcut_key < 0 || shortcut_key > 254 ||
                std::any_of(command_shortcuts.begin(), command_shortcuts.end(),
                    [=](const auto& existing) { return existing.command == command; })) continue;
            command_shortcuts.push_back({command, {static_cast<unsigned>(shortcut_key),
                pulse::json::Bool(entry.Find(L"ctrl")), pulse::json::Bool(entry.Find(L"shift")),
                pulse::json::Bool(entry.Find(L"alt"))}});
            if (command_shortcuts.size() >= 256) break;
        }
    }
    toolbar_order = {186, 2, 3, 4, 6, 5, 7, 306};
    if (const auto* entries = doc.Find(L"toolbar_order"); entries && entries->kind == pulse::json::Kind::Array) {
        toolbar_order.clear();
        for (const auto& entry : entries->children) {
            const int command = pulse::json::Int(&entry);
            if (command > 0 && command < 6000 &&
                std::find(toolbar_order.begin(), toolbar_order.end(), command) == toolbar_order.end())
                toolbar_order.push_back(command);
            if (toolbar_order.size() >= 64) break;
        }
    }
    show_hidden_files = pulse::json::ExtractBool(doc, L"show_hidden_files", false);
    show_protected_os_files = pulse::json::ExtractBool(doc, L"show_protected_os_files", false);
    blank_click_go_back = pulse::json::ExtractBool(doc, L"blank_click_go_back", false);
    change_tracking_enabled = pulse::json::ExtractBool(doc, L"change_tracking_enabled", false);
    change_tracking_days = pulse::json::ExtractInt(doc, L"change_tracking_days", 7);
    if (change_tracking_days != 1 && change_tracking_days != 3 && change_tracking_days != 7)
        change_tracking_days = 7;
    theme_mode = pulse::json::ExtractInt(doc, L"theme_mode", -1);
    if (theme_mode < -1 || theme_mode > 2) theme_mode = -1;
    language = pulse::json::ExtractString(doc, L"language", L"system");
    if (language != L"system" && language != L"zh-CN" && language != L"en-US")
        language = L"system";
    window_effect = pulse::json::ExtractString(doc, L"window_effect", L"mica-alt");
    if (window_effect == L"dwm-blur") window_effect = L"acrylic-material";
    else if (window_effect.empty()) window_effect = L"mica-alt";
    background_image = pulse::json::ExtractString(doc, L"background_image");
    lock_window_size = pulse::json::ExtractBool(doc, L"lock_window_size", false);
    row_height = pulse::json::ExtractInt(doc, L"row_height", 34);
    text_render = pulse::json::ExtractInt(doc, L"text_render", 0);
    if (text_render < 0 || text_render > 2) text_render = 0;
    sidebar_width = pulse::json::ExtractInt(doc, L"sidebar_width", 224);
    // The stored value is the user's intent; the window caps it while drawing.
    if (sidebar_width < static_cast<int>(ui::kSidebarMinWidthDip) ||
        sidebar_width > static_cast<int>(ui::kPanelWidthMaxDip)) sidebar_width = 224;
    address_search_current = pulse::json::ExtractInt(doc, L"address_search_current", 0) != 0;
    address_search_content = pulse::json::ExtractInt(doc, L"address_search_content", 0) != 0;
    if (row_height < 24 || row_height > 48) row_height = 34;
    staging_tray_enabled = pulse::json::ExtractBool(doc, L"staging_tray_enabled", true);
    const int legacy_look = pulse::json::ExtractInt(doc, L"wallpaper_look", 1);
    const int migrated = legacy_look == 0 ? 35 : legacy_look == 2 ? 65 : 50;
    wallpaper_visibility = pulse::json::ExtractInt(doc, L"wallpaper_visibility", migrated);
    wallpaper_visibility = std::clamp(wallpaper_visibility, 0, 100);
    wallpaper_blur = pulse::json::ExtractInt(doc, L"wallpaper_blur", 1);
    if (wallpaper_blur < 0 || wallpaper_blur > 2) wallpaper_blur = 1;
    accent_rgb = pulse::json::ExtractString(doc, L"accent_rgb");
    accent_follow_system = pulse::json::ExtractInt(doc, L"accent_follow_system", 0) != 0;
    uint32_t accent_parsed = 0;
    if (!accent_rgb.empty() && ParseAccentRgb(accent_rgb, accent_parsed)) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"%06X", accent_parsed);
        accent_rgb = hex;
    } else {
        accent_rgb.clear();
    }
    custom_tag_colors.clear();
    for (const std::wstring& entry :
         pulse::json::ExtractStringArray(doc, L"custom_tag_colors")) {
        const wchar_t* text = entry.c_str();
        if (*text == L'#') ++text;
        wchar_t* end = nullptr;
        const unsigned long v = wcstoul(text, &end, 16);
        if (end && *end == L'\0' && v <= 0xFFFFFFul && wcslen(text) == 6)
            custom_tag_colors.push_back(static_cast<uint32_t>(v));
    }
    duplicate_scan_scope = pulse::json::ExtractInt(doc, L"duplicate_scan_scope", 0);
    if (duplicate_scan_scope < 0 || duplicate_scan_scope > 2) duplicate_scan_scope = 0;
    duplicate_scan_folder = pulse::json::ExtractString(doc, L"duplicate_scan_folder");
    duplicate_scan_drive = pulse::json::ExtractString(doc, L"duplicate_scan_drive");
    return true;
}

bool AppPrefs::StoreBackgroundImage(const std::wstring& source_path) {
    if (source_path.empty()) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        background_image = source_path;
        return true;
    }
    const wchar_t* ext = PathFindExtensionW(source_path.c_str());
    std::wstring dest = dir + L"\\wallpaper";
    dest += (ext && ext[0]) ? ext : L".img";
    if (!SamePath(source_path, dest)) {
        DeleteStoredWallpapers(dir);
        if (!CopyFileW(source_path.c_str(), dest.c_str(), FALSE)) {
            background_image = source_path;
            return true;
        }
    }
    background_image = dest;
    return true;
}

void AppPrefs::ClearBackgroundImage() {
    const std::wstring dir = GetPulseDataDir();
    if (!dir.empty() && !background_image.empty()) {
        const std::wstring prefix = dir + L"\\wallpaper";
        if (background_image.size() >= prefix.size() &&
            CompareStringOrdinal(background_image.c_str(), static_cast<int>(prefix.size()),
                                 prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL) {
            DeleteFileW(background_image.c_str());
        }
    }
    background_image.clear();
}

bool AppPrefs::ReadLaunchOnStartup() const {
    return !ReadStartupCommand().empty();
}

bool AppPrefs::ApplyLaunchOnStartup(bool on) {
    if (!persist) { launch_on_startup = on; return true; }
    HKEY key = nullptr;
    const LSTATUS opened = on
        ? RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (!on && opened == ERROR_FILE_NOT_FOUND) { launch_on_startup = false; return true; }
    if (opened != ERROR_SUCCESS) return false;
    LONG st = ERROR_SUCCESS;
    if (on) {
        const std::wstring exe = ExePath();
        if (exe.empty()) { RegCloseKey(key); return false; }
        const std::wstring cmd = StartupCommandLine(exe);
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (st == ERROR_SUCCESS) launch_on_startup = on;
    return st == ERROR_SUCCESS;
}

namespace {
std::wstring ReadLegacyFolderCommand(const wchar_t* cls) {
    const std::wstring path = std::wstring(L"Software\\Classes\\") + cls + L"\\shell\\open\\command";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    wchar_t command[32768]{};
    DWORD bytes = sizeof(command) - sizeof(wchar_t), type = 0;
    const LSTATUS result = RegQueryValueExW(key, nullptr, nullptr, &type,
        reinterpret_cast<BYTE*>(command), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) ? command : L"";
}
}

bool AppPrefs::ReadFolderOpen() const {
    const std::wstring exe = ExePath();
    return DefaultManagerCommandIsOurs(ReadLegacyFolderCommand(L"Directory"), exe) &&
        DefaultManagerCommandIsOurs(ReadLegacyFolderCommand(L"Drive"), exe);
}
bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept {
    const wchar_t* p = text.c_str();
    if (!p || !*p) return false;
    if (*p == L'#') ++p;
    if (wcslen(p) != 6) return false;
    for (int i = 0; i < 6; ++i) {
        if (!iswxdigit(p[i])) return false;
    }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(p, &end, 16);
    if (!end || *end != L'\0' || v > 0xFFFFFFul) return false;
    rgb = static_cast<uint32_t>(v);
    return true;
}

bool ParseAccentInput(const std::wstring& text, uint32_t& rgb) noexcept {
    if (ParseAccentRgb(text, rgb)) return true;
    const wchar_t* p = text.c_str();
    unsigned channel[3]{};
    for (int i = 0; i < 3; ++i) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (*p < L'0' || *p > L'9') return false;
        unsigned value = 0;
        do {
            value = value * 10 + static_cast<unsigned>(*p++ - L'0');
            if (value > 255) return false;
        } while (*p >= L'0' && *p <= L'9');
        channel[i] = value;
        while (*p == L' ' || *p == L'\t') ++p;
        if (i < 2) { if (*p++ != L',') return false; }
    }
    if (*p != L'\0') return false;
    rgb = (channel[0] << 16) | (channel[1] << 8) | channel[2];
    return true;
}

bool ParseWallpaperVisibility(std::wstring_view text, int& percent) noexcept {
    if (text.empty()) return false;
    if (text.back() == L'%') text.remove_suffix(1);
    if (text.empty()) return false;
    int value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        value = value * 10 + (ch - L'0');
        if (value > 100) return false;
    }
    percent = value;
    return true;
}

bool AppPrefs::Load() {
    saves_enabled_ = false;
    had_file = false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) {
        launch_on_startup = ReadLaunchOnStartup();
        return false;
    }
    std::wstring json;
    const auto path = dir + L"\\app.json";
    const DWORD attrs = GetFileAttributesW(path.c_str());
    const DWORD error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attrs == INVALID_FILE_ATTRIBUTES &&
        (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    had_file = attrs != INVALID_FILE_ATTRIBUTES;
    const bool loaded = ReadUtf8File(path, json) && FromJson(json);
    saves_enabled_ = loaded || missing;
    const std::wstring startup_command = ReadStartupCommand();
    launch_on_startup = !startup_command.empty();
    if (persist && launch_on_startup && StartupCommandNeedsRepair(startup_command, ExePath()))
        ApplyLaunchOnStartup(true);
    // Association state and legacy repair run in SystemIntegration's background worker.
    return loaded;
}

bool AppPrefs::Save() const {
    if (!persist) return true;
    if (!saves_enabled_) return false;
    const std::wstring dir = GetPulseDataDir();
    if (dir.empty()) return false;
    return WriteUtf8FileAtomic(dir + L"\\app.json", ToJson());
}

} // namespace pulse::app
