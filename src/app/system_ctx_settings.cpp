// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/windows_compat.h"
#include "system_ctx_settings.h"
#include "system_ctx_apply.h"
#include "app_state.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "about_info.h"
#include "system_ctx_model.h"
#include "../common/localization.h"

#include <cmath>
#include <filesystem>
#include <utility>

namespace pulse {
namespace {

int PopupSystemContextMenu(AppState& s, std::vector<ui::FluentMenuItem> items) {
    if (!EnsureMenu(s)) return 0;
    s.menu->SetTheme(s.darkMode, s.accentColor);
    POINT point{};
    GetCursorPos(&point);
    return s.menu->TrackPopup(point, std::move(items));
}

std::wstring SystemContextProgramFolder(const std::wstring& file) {
    return file.empty() ? std::wstring() : std::filesystem::path(file).parent_path().wstring();
}

void ToggleSystemContextPulseRow(AppState& s, int row) {
    s.settings.SystemCtx().TogglePulseRow(row);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void OpenSystemContextProgramFolder(AppState& s, const std::wstring& file) {
    const auto folder = SystemContextProgramFolder(file);
    if (!folder.empty()) NewTab(s, folder);
}

void ShowSystemContextRowMenu(AppState& s, int row) {
    auto& model = s.settings.SystemCtx();
    const auto key = model.RowRegistryPath(row);
    const auto file = model.RowFile(row);
    const auto pulse_key = model.RowPulseKey(row);
    const auto folder = SystemContextProgramFolder(file);
    const auto view = model.View();
    const bool ready = view && view->state == ui::SysCtxView::State::Ready;
    std::vector<ui::FluentMenuItem> items;
    auto add = [&](int command, const wchar_t* text, bool enabled) {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = text;
        item.enabled = enabled;
        items.push_back(std::move(item));
    };
    add(1, l10n::Pick(L"复制注册表路径", L"Copy registry path"), !key.empty());
    add(2, l10n::Pick(L"复制程序路径", L"Copy program path"), !file.empty());
    add(3, l10n::Pick(L"在新标签页打开所在文件夹", L"Open its folder in a new tab"), !folder.empty());
    if (!pulse_key.empty()) {
        items.back().separator_after = true;
        add(4, l10n::Pick(L"只在 Pulse 右键菜单中隐藏", L"Hide in Pulse's menu only"), ready);
        items.back().checked = model.RowPulseHidden(row);
    }
    if (model.RowDeletable(row)) {
        items.back().separator_after = true;
        add(5, model.RowDeleting(row)
            ? l10n::Pick(L"取消删除", L"Keep it")
            : l10n::Pick(L"删除残留项（点“应用更改”后生效）", L"Delete leftover (on Apply changes)"), ready);
    }
    const int command = PopupSystemContextMenu(s, std::move(items));
    if (command == 1) app::CopyTextToClipboard(s.hwnd, key);
    else if (command == 2) app::CopyTextToClipboard(s.hwnd, file);
    else if (command == 3 && !folder.empty()) NewTab(s, folder);
    else if (command == 4) ToggleSystemContextPulseRow(s, row);
    else if (command == 5) {
        model.DeleteRow(row);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

void RestoreSystemContextChanges(AppState& s) {
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem restore;
    restore.command = 1;
    restore.text = l10n::Pick(L"撤销本次会话的全部注册表改动",
                              L"Undo every registry change made during this session");
    items.push_back(std::move(restore));
    ui::FluentMenuItem cancel;
    cancel.text = l10n::Pick(L"取消", L"Cancel");
    items.push_back(std::move(cancel));
    if (PopupSystemContextMenu(s, std::move(items)) == 1) s.settings.SystemCtx().RestoreAll();
}

} // namespace

void HandleSystemContextAction(AppState& s, int index) {
    auto& model = s.settings.SystemCtx();
    if (index == ui::kSysCtxLocalLayer) {
        OpenSettingsTab(s, 2);
        return;
    }
    if (index == ui::kSysCtxDetailPulse) {
        ToggleSystemContextPulseRow(s, -1);
        return;
    }
    if (index == ui::kSysCtxLog) {
        const auto directory = model.DataDirectory();
        if (!directory.empty()) NewTab(s, directory);
        return;
    }
    if (index >= ui::kSysCtxRemoveBase) {
        model.DeleteRow(index - ui::kSysCtxRemoveBase);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (index >= ui::kSysCtxSlowSkipBase) {
        if (index >= ui::kSysCtxMarkRestoreBase) model.RestoreMarkRow(index - ui::kSysCtxMarkRestoreBase);
        else if (index >= ui::kSysCtxSlowToggleBase) model.ToggleSlowRow(index - ui::kSysCtxSlowToggleBase);
        else model.QueueSlowSkip(index - ui::kSysCtxSlowSkipBase);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (index >= ui::kSysCtxPreviewBase) {
        const int item = index - ui::kSysCtxPreviewBase;
        const auto vm = BuildVm(s, false);
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                       static_cast<float>(s.compositor.Height()));
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(s.hwnd, &point);
        const float top = s.renderer.SysCtxPreviewItemTop(vm, bounds, item);
        if (std::isnan(top)) return;
        model.BeginPreviewDrag(item, static_cast<float>(point.y), static_cast<float>(point.y) - top);
        if (model.PreviewDragItem() >= 0) SetCapture(s.hwnd);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (index >= ui::kSysCtxMoreBase) {
        ShowSystemContextRowMenu(s, index - ui::kSysCtxMoreBase);
        return;
    }
    if (index == ui::kSysCtxDetailCopy) {
        app::CopyTextToClipboard(s.hwnd, model.RowRegistryPath(-1));
        return;
    }
    if (index == ui::kSysCtxDetailFolder) {
        OpenSystemContextProgramFolder(s, model.RowFile(-1));
        return;
    }
    if (index == ui::kSysCtxDetailDelete) model.DeleteRow(-1);
    else if (index == ui::kSysCtxClean) model.DeleteSceneLeftovers();
    else if (index == ui::kSysCtxRestoreAll) RestoreSystemContextChanges(s);
    else if (index == ui::kSysCtxRefresh) model.Refresh();
    else if (index == ui::kSysCtxApply) {
        if (ApplySystemContextChanges(model, s.ctxMenuPrefs)) s.context_menu.InvalidateCaches();
    } else if (index == ui::kSysCtxDiscard) model.DiscardPending();
    else if (index == ui::kSysCtxUndo) model.Undo();
    else if (index == ui::kSysCtxTabPreview || index == ui::kSysCtxTabDetail)
        model.SetSideTab(index == ui::kSysCtxTabDetail ? 1 : 0);
    else if (index == ui::kSysCtxDetailSwitch) model.ToggleRow(-1);
    else if (index == ui::kSysCtxOverview) model.ShowOverview();
    else if (index >= ui::kSysCtxSelectBase) model.SelectRow(index - ui::kSysCtxSelectBase);
    else if (index >= ui::kSysCtxRowBase) model.ToggleRow(index - ui::kSysCtxRowBase);
    else if (index >= ui::kSysCtxSceneBase) model.SelectScene(index - ui::kSysCtxSceneBase);
    else if (index >= ui::kSysCtxSubBase) model.SelectSub(index - ui::kSysCtxSubBase);
    else if (index >= ui::kSysCtxFilterBase) model.SetFilter(index - ui::kSysCtxFilterBase);
    const auto vm = BuildVm(s, false);
    const float maximum = s.renderer.SettingsMaxScroll(vm, static_cast<float>(s.compositor.Width()),
                                                       static_cast<float>(s.compositor.Height()));
    s.settings.SetScroll(s.settings.scroll(), maximum);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
