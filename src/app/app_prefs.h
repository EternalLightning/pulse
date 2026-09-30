// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include "folder_view_prefs.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

struct AppPrefs {
    bool persist = true;
    bool launch_on_startup = false;
    bool keep_running_on_close = false;
    bool open_folders_in_pulse = false;
    bool verify_copies = false;
    bool show_status_performance = false;
    bool show_pinned_tab_names = true;
    // Details list presentation.
    bool list_smart_date = true;
    bool list_zebra_rows = true;
    bool list_size_bar = false;
    // 0 folders first, 1 follow the sort direction, 2 mixed with files
    int folder_sort_mode = 0;
    FolderViewPrefs folder_views;
    bool search_pinyin = true;
    bool global_search_enabled = false;
    uint32_t global_search_modifiers = 1; // MOD_ALT
    uint32_t global_search_key = 32; // VK_SPACE
    bool show_hidden_files = false;
    // Hidden + system attributes; File Explorer keeps these behind a separate option.
    bool show_protected_os_files = false;
    bool blank_click_go_back = false;
    bool change_tracking_enabled = false;
    int change_tracking_days = 7;
    // system / zh-CN / en-US
    int theme_mode = -1; // legacy session theme, or 0 system / 1 light / 2 dark
    std::wstring language = L"system";
    // none / acrylic-material / mica / mica-alt  (legacy dwm-blur → acrylic)
    std::wstring window_effect = L"mica-alt";
    std::wstring background_image;
    int wallpaper_visibility = 50; // 0..100 percent of image visibility
    int wallpaper_blur = 1; // image mode blur: 0 off, 1 light, 2 strong
    int row_height = 34; // file-list row height in DIPs (24..48)
    int sidebar_width = 224; // DIPs
    bool address_search_current = false;
    bool address_search_content = false;
    int tray_icon_size = 48; // staging-tray deck icon edge in DIPs (32..64)
    // Empty = theme default, or Windows when explicitly selected.
    std::wstring accent_rgb;
    bool accent_follow_system = false;
    // Tag colors the user added via the custom color dialog (0xRRGGBB),
    // appended after the seven Finder defaults in the swatch strip.
    std::vector<uint32_t> custom_tag_colors;
    int duplicate_scan_scope = 0; // 0 folder, 1 drive, 2 all local disks
    std::wstring duplicate_scan_folder;
    std::wstring duplicate_scan_drive;
    // Version that last ran with these prefs; drives the one-time "updated" toast.
    bool had_file = false; // runtime only: app.json existed when Load() ran

    void ResetToDefaults();
    bool Load();
    bool Save() const;
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);

    // HKCU Run key is the source of truth; call after Load() and on toggle.
    bool ReadLaunchOnStartup() const;
    bool ApplyLaunchOnStartup(bool on);

    // HKCU Directory/Drive open verbs; call after Load() and on toggle.
    bool ReadFolderOpen() const;
    bool ApplyFolderOpen(bool on);

    bool StoreBackgroundImage(const std::wstring& source_path);
    void ClearBackgroundImage();
};

std::wstring FolderOpenCommandLine(const std::wstring& exe);
bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe);
bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept;
bool ParseAccentInput(const std::wstring& text, uint32_t& rgb) noexcept;
bool ParseWallpaperVisibility(std::wstring_view text, int& percent) noexcept;

} // namespace pulse::app
