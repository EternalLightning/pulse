// SPDX-License-Identifier: Apache-2.0
#include "app_internal.h"
#include "settings_input.h"
#include "settings_value_dialog.h"
#include "system_ctx_model.h"
#include "app_hover.h"
#include "../common/localization.h"
#include "../ui/color_picker.h"
#include <windowsx.h>

namespace pulse {
namespace {
using H = ui::HitTestResult;

bool IsSetting(H::Region region) {
    return (region >= H::SettingsContentIndex && region <= H::SettingsContentAction) ||
        (region >= H::SettingsNav && region <= H::SettingsDupDeleteAll) ||
        region == H::SettingsWallpaperLook || region == H::SettingsWallpaperBlur;
}

bool SameControl(const H& a, const H& b) {
    return a.region == b.region && a.index == b.index && a.sub_index == b.sub_index;
}

bool SameTarget(const H& a, const H& b) {
    return SameControl(a, b) && a.path == b.path;
}

bool PreviewDrag(const H& hit) {
    return (hit.region == H::SettingsToggle && hit.index >= ui::kSettingsMenuFixedHit) ||
        (hit.region == H::SettingsSysCtx && hit.index >= ui::kSysCtxPreviewBase &&
            hit.index < ui::kSysCtxSlowSkipBase);
}

D2D1_RECT_F WindowBounds(const AppState& state) {
    return D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
        static_cast<float>(state.compositor.Height()));
}

bool Synchronize(AppState& state) {
    const auto* tab = ActiveTab(state);
    const bool settings = IsSettingsTab(tab);
    const uint64_t identity = settings ? tab->network_identity : 0;
    const int page = settings ? state.settings.page() : -1;
    auto& input = state.settings_input;
    if (input.tab != identity || input.page != page) {
        CancelSettingsPointer(state);
        input = {};
        input.tab = identity;
        input.page = page;
    }
    return settings;
}

void ScrollToFocus(AppState& state) {
    CancelScrollAnimation(state);
    const auto vm = BuildVm(state, false);
    const auto rect = WindowBounds(state);
    const auto content = state.renderer.SettingsContentBounds(vm, rect);
    const auto& bounds = state.settings_input.focus.control_bounds;
    if (state.settings_input.focus.region != H::SettingsNav) {
        const float margin = 4 * state.scale;
        float offset = state.settings.scroll();
        if (bounds.top < content.top + margin) offset += bounds.top - content.top - margin;
        else if (bounds.bottom > content.bottom - margin)
            offset += (std::min)(bounds.bottom - content.bottom + margin, bounds.top - content.top - margin);
        state.settings.SetScroll(offset, state.renderer.SettingsMaxScroll(vm, rect.right, rect.bottom));
    }
    for (const auto& target : state.renderer.SettingsControls(BuildVm(state, false), rect)) {
        if (SameTarget(target, state.settings_input.focus)) {
            state.settings_input.focus = target;
            break;
        }
    }
    state.hitTestVmValid = false;
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void MoveMenuItem(AppState& state, const H& hit, int direction) {
    const auto surface = state.settings.ContextTab() == 1 ? model::BuiltinMenuSurface::Background
        : state.settings.ContextTab() == 2 ? model::BuiltinMenuSurface::RowButtons : model::BuiltinMenuSurface::Item;
    auto order = state.ctxMenuPrefs.BuiltinOrder(surface);
    const auto item = static_cast<model::BuiltinMenuItem>(hit.index - ui::kSettingsMenuRowHit);
    const auto at = std::find(order.begin(), order.end(), item);
    if (at == order.end()) return;
    const auto index = at - order.begin();
    const auto next = index + direction;
    if (next < 0 || next >= static_cast<ptrdiff_t>(order.size())) return;
    std::iter_swap(at, order.begin() + next);
    state.settings.SetMenuOrder(order);
    ScrollToFocus(state);
}

void ActivatePreview(AppState& state, const H& hit) {
    if (!EnsureMenu(state)) return;
    std::vector<ui::FluentMenuItem> items;
    if (hit.region == H::SettingsToggle) {
        items.push_back({}); items.back().command = 1;
        items.back().text = l10n::Pick(L"上移", L"Move up");
        items.push_back({}); items.back().command = 2;
        items.back().text = l10n::Pick(L"下移", L"Move down");
    } else {
        const auto view = state.settings.SystemCtx().View();
        const int index = hit.index - ui::kSysCtxPreviewBase;
        if (!view || index < 0 || index >= static_cast<int>(view->preview.size())) return;
        const auto& item = view->preview[static_cast<size_t>(index)];
        state.settings.SystemCtx().SelectPreview(index);
        for (int zone : {ui::kSysCtxZoneTop, ui::kSysCtxZoneDefault, ui::kSysCtxZoneBottom, ui::kSysCtxZoneHidden}) {
            if (!item.movable && zone != ui::kSysCtxZoneDefault && zone != ui::kSysCtxZoneHidden) continue;
            ui::FluentMenuItem action;
            action.command = zone + 1;
            action.text = zone == ui::kSysCtxZoneTop ? l10n::Pick(L"置于顶部", L"Move to top")
                : zone == ui::kSysCtxZoneBottom ? l10n::Pick(L"置于底部", L"Move to bottom")
                : zone == ui::kSysCtxZoneHidden ? l10n::Pick(L"隐藏", L"Hide")
                : l10n::Pick(L"默认位置", L"Default position");
            items.push_back(std::move(action));
        }
    }
    state.menu->SetTheme(state.darkMode, state.accentColor);
    state.menu->SetHoverFirstOnOpen(true);
    const int command = state.menu->TrackPopup(SettingsPopupPoint(state), std::move(items));
    state.menu->SetHoverFirstOnOpen(false);
    if (command > 0 && Synchronize(state)) {
        if (hit.region == H::SettingsToggle) MoveMenuItem(state, hit, command == 1 ? -1 : 1);
        else state.settings.SystemCtx().DropPreview(hit.index - ui::kSysCtxPreviewBase, command - 1);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}
} // namespace

bool ActivateSettingsControl(AppState& state, const ui::HitTestResult& hit, bool keyboard) {
    if (!IsSetting(hit.region)) return false;
    if (keyboard && PreviewDrag(hit)) {
        ActivatePreview(state, hit);
        return true;
    }
    auto* s = &state;
    const HWND hwnd = state.hwnd;
    const auto vm = BuildVm(state, false);
    if (hit.region == ui::HitTestResult::SettingsNav) {
        OpenSettingsTab(*s, hit.index);
    } else if (hit.region == ui::HitTestResult::SettingsChangeDays) {
        HandleChangeClick(*s, hit, {});
    } else if (hit.region == ui::HitTestResult::SettingsContentIndex) {
        ShowSearchOptions(*s);
    } else if (HandleSettingsControl(*s, hit)) {
    } else if (hit.region == ui::HitTestResult::SettingsToggle) {
        if (hit.index == 21) s->settings.ClearError();
        s->settings.ToggleUi(hit.index);
        if (hit.index == 21 && !s->settings.error().empty())
            MessageBoxW(hwnd, s->settings.error().c_str(),
                l10n::Get(l10n::StringId::SettingsDefaultManager).c_str(), MB_OK | MB_ICONWARNING);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsAccent) {
        if (hit.index == 1) {
            s->settings.AccentChoice(true, 0);
        } else if (EnsureMenu(*s)) {
            uint32_t rgb = 0;
            app::ParseAccentInput(L"#" + vm.settings_accent_hex, rgb);
            if (ui::ColorPickerPopup::Pick(hwnd, &s->compositor, s->menu.get(), s->scale,
                    SettingsPopupPoint(*s), rgb, s->darkMode, rgb))
                s->settings.AccentChoice(false, rgb);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsEffect) {
        if (hit.index >= 0 && hit.index < ui::kWindowEffectCount)
            s->settings.WindowEffect(
                ui::WindowEffectId(static_cast<ui::WindowEffect>(hit.index)));
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsDensity) {
        s->settings.RowHeight(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsTextRender) {
        s->settings.TextRendering(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsFolderSort) {
        s->settings.FolderSort(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsWallpaperLook) {
        std::wstring value = std::to_wstring(s->appPrefs.wallpaper_visibility);
        while (app::PromptSettingsValue(hwnd, l10n::Get(l10n::StringId::SettingsWallpaperLook),
                l10n::Get(l10n::StringId::WallpaperPercentHint), value, value, 32, {},
                s->darkMode, s->accentColor)) {
            int percent = 0;
            if (app::ParseWallpaperVisibility(value, percent)) {
                s->settings.WallpaperVisibility(percent);
                break;
            }
            MessageBoxW(hwnd, l10n::Get(l10n::StringId::WallpaperPercentInvalid).c_str(),
                l10n::Get(l10n::StringId::SettingsWallpaperLook).c_str(), MB_OK | MB_ICONWARNING);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsWallpaperBlur) {
        s->settings.WallpaperBlur(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsLanguage) {
        static constexpr const wchar_t* languages[] = {L"system", L"zh-CN", L"en-US"};
        if (hit.index >= 0 && hit.index < static_cast<int>(std::size(languages)))
            s->settings.Language(languages[hit.index]);
    } else if (hit.region == ui::HitTestResult::SettingsWallpaper) {
        s->settings.Wallpaper(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsIndexVolume) {
        s->settings.ToggleVolume(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsConfigurationAction) {
        s->settings.ConfigurationAction(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsIndexAction) {
        s->settings.IndexAction(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsIndexExcludeAction) {
        s->settings.AddExclude();
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsIndexExcludeRemove) {
        s->settings.RemoveExclude(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsNetworkAction) {
        s->settings.NetworkAction(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsNetworkRemove) {
        s->settings.RemoveNetwork(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsRestore) {
        s->ctxMenuPrefs.ResetToDefaults();
        s->ctxMenuPrefs.Save();
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsDiagnosticsAction) {
        s->settings.DiagnosticsAction(hit.index);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsDupScope) {
        if (!s->duplicateScan.scanning && hit.index >= 0 && hit.index <= 2) {
            s->duplicateScan.scope = static_cast<app::DuplicateScanScope>(hit.index);
            s->duplicateScan.minimum_file_bytes =
                app::DuplicateScanSession::DefaultMinimumBytes(s->duplicateScan.scope);
            PersistDuplicateScanPrefs(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (hit.region == ui::HitTestResult::SettingsDupDrive) {
        if (!s->duplicateScan.scanning && hit.index >= 0) {
            int n = 0;
            for (const auto& volume : s->dup_volume_cache) {
                if (volume.mount_point.empty()) continue;
                if (n == hit.index) {
                    s->duplicateScan.drive_root =
                        app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
                    PersistDuplicateScanPrefs(*s);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
                ++n;
            }
        }
    } else if (hit.region == ui::HitTestResult::SettingsDupBrowse) {
        if (!s->duplicateScan.scanning) {
            std::wstring path;
            if (PickFolder(hwnd, path, l10n::Get(l10n::StringId::DupFolderPlaceholder).c_str())) {
                s->duplicateScan.folder_path = std::move(path);
                PersistDuplicateScanPrefs(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    } else if (hit.region == ui::HitTestResult::SettingsDupSkipSmallFiles) {
        if (!s->duplicateScan.scanning) {
            s->duplicateScan.skip_small_files = !s->duplicateScan.skip_small_files;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (hit.region == ui::HitTestResult::SettingsDupMinSize) {
        static constexpr uint64_t kBytes[] = {1024ull, 1024ull * 1024ull, 10ull * 1024ull * 1024ull};
        if (!s->duplicateScan.scanning && s->duplicateScan.skip_small_files && hit.index >= 0 && hit.index < 3) {
            s->duplicateScan.minimum_file_bytes = kBytes[hit.index];
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (hit.region == ui::HitTestResult::SettingsDupScan) {
        StartDuplicateScan(*s);
    } else if (hit.region == ui::HitTestResult::SettingsDupCancel) {
        CancelDuplicateScan(*s);
    } else if (hit.region == ui::HitTestResult::SettingsDupKeep) {
        if (!s->duplicateScan.scanning)
            s->duplicateScan.SetKeep(static_cast<size_t>(hit.index),
                                     static_cast<size_t>(hit.sub_index));
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsDupOpen) {
        OpenDuplicateLocation(*s, static_cast<size_t>(hit.index),
                              static_cast<size_t>(hit.sub_index));
    } else if (hit.region == ui::HitTestResult::SettingsDupGroupDelete) {
        RecycleDuplicateGroup(*s, static_cast<size_t>(hit.index));
    } else if (hit.region == ui::HitTestResult::SettingsDupGroupIgnore) {
        if (!s->duplicateScan.scanning)
            s->duplicateScan.IgnoreGroup(static_cast<size_t>(hit.index));
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (hit.region == ui::HitTestResult::SettingsDupDeleteAll) {
        RecycleAllDuplicateExtras(*s);
    } else {
        return false;
    }
    return true;
}
void CancelSettingsPointer(AppState& state) {
    const bool captured = state.settings_input.pressed.region != H::None;
    state.settings_input.pressed = {};
    state.hitTestVmValid = false;
    if (captured && GetCapture() == state.hwnd) ReleaseCapture();
    if (captured) InvalidateRect(state.hwnd, nullptr, FALSE);
}

bool BeginSettingsPointer(AppState& state, const H& hit) {
    const bool settings = Synchronize(state);
    if (state.settings_input.keyboard) InvalidateRect(state.hwnd, nullptr, FALSE);
    state.settings_input.keyboard = false;
    CancelSettingsPointer(state);
    if (!settings || !IsSetting(hit.region)) return false;
    const auto targets = state.renderer.SettingsControls(BuildVm(state, false), WindowBounds(state));
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const H& item) {
        return SameControl(item, hit);
    });
    if (target == targets.end()) return true;
    state.settings_input.focus = *target;
    if (hit.region == H::SettingsNav || hit.region == H::SettingsCtxPivot || PreviewDrag(hit)) return false;
    state.settings_input.pressed = *target;
    UpdateHoverFromHit(state, hit, {static_cast<LONG>((target->control_bounds.left + target->control_bounds.right) * 0.5f),
        static_cast<LONG>((target->control_bounds.top + target->control_bounds.bottom) * 0.5f)});
    SetFocus(state.hwnd);
    SetCapture(state.hwnd);
    InvalidateRect(state.hwnd, nullptr, FALSE);
    return true;
}

bool EndSettingsPointer(AppState& state, int x, int y) {
    const auto pressed = state.settings_input.pressed;
    if (pressed.region == H::None) return false;
    const auto source_tab = state.settings_input.tab;
    const auto source_page = state.settings_input.page;
    const bool current = Synchronize(state);
    CancelSettingsPointer(state);
    if (!current || state.settings_input.tab != source_tab || state.settings_input.page != source_page) return true;
    const auto vm = BuildVm(state, false);
    const auto rect = WindowBounds(state);
    const auto hit = state.renderer.HitTest(vm, rect, static_cast<float>(x), static_cast<float>(y));
    for (const auto& target : state.renderer.SettingsControls(vm, rect)) {
        if (SameControl(target, hit) && SameTarget(target, pressed)) {
            ActivateSettingsControl(state, target);
            state.hitTestVmValid = false;
            break;
        }
    }
    return true;
}

POINT SettingsPopupPoint(AppState& state) {
    POINT point{};
    if (state.settings_input.keyboard && state.settings_input.focus.region != H::None) {
        const auto& r = state.settings_input.focus.control_bounds;
        point = {static_cast<LONG>(r.left), static_cast<LONG>(r.bottom)};
        ClientToScreen(state.hwnd, &point);
    } else GetCursorPos(&point);
    return point;
}

bool HandleSettingsKey(AppState& state, WPARAM key, LPARAM flags) {
    if (!Synchronize(state)) return false;
    auto& input = state.settings_input;
    if (key == VK_ESCAPE && input.pressed.region != H::None) {
        CancelSettingsPointer(state);
        return true;
    }
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const bool alt = GetKeyState(VK_MENU) < 0;
    if (ctrl || alt) return false;
    const auto targets = state.renderer.SettingsControls(BuildVm(state, false), WindowBounds(state));
    if (targets.empty()) return key == VK_TAB;
    const auto found = std::find_if(targets.begin(), targets.end(), [&](const H& item) {
        return SameTarget(item, input.focus);
    });
    if (key == VK_TAB || (input.keyboard && (key == VK_UP || key == VK_DOWN ||
        key == VK_LEFT || key == VK_RIGHT))) {
        CancelSettingsPointer(state);
        const bool backwards = key == VK_TAB ? GetKeyState(VK_SHIFT) < 0 : key == VK_UP || key == VK_LEFT;
        const auto count = static_cast<ptrdiff_t>(targets.size());
        const auto index = found == targets.end() ? (backwards ? 0 : count - 1) : found - targets.begin();
        input.focus = targets[static_cast<size_t>((index + (backwards ? count - 1 : 1)) % count)];
        input.keyboard = true;
        SetFocus(state.hwnd);
        ScrollToFocus(state);
        return true;
    }
    if (input.keyboard && (key == VK_SPACE || key == VK_RETURN)) {
        if ((flags & (static_cast<LPARAM>(1) << 30)) || found == targets.end()) return true;
        const H target = *found;
        ActivateSettingsControl(state, target, true);
        if (Synchronize(state)) {
            input.keyboard = true;
            if (target.region == H::SettingsNav || target.region == H::SettingsCtxPivot) {
                input.focus = {};
                input.focus.region = H::SettingsNav;
                input.focus.index = state.settings.page() == 5 ? 2 : state.settings.page();
            }
            ScrollToFocus(state);
        }
        return true;
    }
    return false;
}
} // namespace pulse
