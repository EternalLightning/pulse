// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/context_menu_controller.h"
#include "../app/device_menu.h"
#include "../common/localization.h"
#include "../ipc/shell_client.h"
#include "../ui/typography.h"
#include "../ui/notification_toast.h"
#include "../ui/animation_clock.h"

#include <algorithm>
#include <cstdio>
#include <commctrl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

namespace pulse::ui {
struct NotificationToastTestPeer {
    static float Opacity(const NotificationToast& toast) { return toast.reveal_.value; }
    static void Advance(NotificationToast& toast, uint64_t elapsed) { toast.reveal_.started = AnimationTimeMs() - elapsed; }
    static void Expire(NotificationToast& toast) { toast.remaining_ = 0; }
};
struct FluentMenuTestPeer {
    static void Begin(FluentMenu& menu, std::vector<FluentMenuItem> items) {
        menu.model_.SetItems(std::move(items));
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_);
        menu.popup_pt_ = {300, 200}; menu.base_x_ = 300; menu.base_y_ = 200;
        menu.open_ = true; menu.animating_out_ = false;
        menu.Render();
        for (int row = 0; row < menu.model_.Count(); ++row)
            if (menu.model_.At(row)->glyph == L"\xE712") menu.OpenSubmenu(row);
    }
    static bool Loading(const FluentMenu& menu) {
        if (!menu.open_flyouts_) return false;
        const auto& model = menu.flyouts_[0]->model;
        for (int row = 0; row < model.Count(); ++row)
            if (model.At(row)->text == l10n::Get(l10n::StringId::LoadingEllipsis)) return true;
        return false;
    }
    static int Flyouts(const FluentMenu& menu) { return menu.open_flyouts_; }
    static bool LabelsFit(const FluentMenu& menu) {
        for (int row = 0; row < menu.model_.Count(); ++row)
            if (menu.model_.LabelTruncated(row)) return false;
        return true;
    }
    static bool ShortcutsFit(const FluentMenu& menu) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> caption;
        auto* factory = menu.compositor_->DwriteFactory();
        if (FAILED(typography::CreateTextFormat(factory,
                {typography::FontRole::Text, 12.0f * menu.scale_, DWRITE_FONT_WEIGHT_NORMAL}, &caption))) return false;
        const float available = std::max(24.0f * menu.scale_,
            menu.model_.WidthPx() - 148.0f * menu.scale_);
        for (int row = 0; row < menu.model_.Count(); ++row) {
            const auto* item = menu.model_.At(row);
            if (!item->shortcut.empty() && typography::MeasureAdvance(factory, caption.Get(), item->shortcut) +
                    4.0f * menu.scale_ > available) return false;
        }
        return true;
    }
    static void OpenOverflow(FluentMenu& menu) {
        for (int row = 0; row < menu.model_.Count(); ++row)
            if (menu.model_.At(row)->glyph == L"\xE712") menu.OpenSubmenu(row);
    }
};
}

namespace {
int failures = 0;
void Check(bool passed, const char* name) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}

void RunToastAnimation(HWND owner, pulse::ui::Compositor& compositor, const std::filesystem::path& output) {
    using namespace pulse::ui;
    std::filesystem::create_directories(output);
    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) {
        compositor.RecreateTextFormats(scale);
        NotificationToast toast;
        const auto theme = MakeTheme(dark, D2D1::ColorF(0x0078D4));
        const auto draw = [&](const wchar_t* phase, bool high_contrast = false) {
            auto* dc = compositor.Dc();
            dc->BeginDraw(); dc->Clear(theme.bg);
            toast.Draw(compositor, theme, scale, high_contrast);
            Check(SUCCEEDED(dc->EndDraw()), "toast animation frame renders");
            const auto name = std::wstring(dark ? L"dark-" : L"light-") +
                std::to_wstring(static_cast<int>(scale * 100)) + L"-" + phase + L".png";
            Check(compositor.SaveSnapshot((output / name).c_str()), "save toast animation frame");
        };
        toast.Show(owner, L"无法完成复制", L"操作失败。", false);
        Sleep(180);
        draw(L"enter-start");
        Check(toast.Animating() && NotificationToastTestPeer::Opacity(toast) == 0,
            "delayed first paint still starts the entry transition at zero opacity");
        NotificationToastTestPeer::Advance(toast, 70);
        draw(L"enter-middle");
        Check(toast.Animating() && NotificationToastTestPeer::Opacity(toast) > 0 &&
            NotificationToastTestPeer::Opacity(toast) < 1, "entry has an intermediate frame");
        NotificationToastTestPeer::Advance(toast, 200);
        draw(L"shown");
        Check(!toast.Animating() && toast.IsVisible(), "settled toast releases animation frames");
        const auto bounds = toast.Bounds();
        const auto close = MAKELPARAM(static_cast<int>(bounds.right - 16 * scale),
            static_cast<int>((bounds.top + bounds.bottom) * 0.5f));
        toast.HandleMessage(owner, WM_LBUTTONDOWN, 0, close);
        toast.HandleMessage(owner, WM_LBUTTONUP, 0, close);
        Check(!toast.IsVisible() && toast.Animating(), "close retains the painted toast for its exit transition");
        NotificationToastTestPeer::Advance(toast, 60);
        draw(L"exit-middle");
        Check(toast.Animating() && NotificationToastTestPeer::Opacity(toast) > 0 &&
            NotificationToastTestPeer::Opacity(toast) < 1, "exit has an intermediate frame");
        NotificationToastTestPeer::Advance(toast, 200);
        draw(L"hidden");
        Check(!toast.Animating(), "exit stops requesting frames at zero opacity");
        toast.Show(owner, L"Status", L"Auto close", false);
        draw(L"auto-start");
        NotificationToastTestPeer::Advance(toast, 200);
        draw(L"auto-shown");
        NotificationToastTestPeer::Expire(toast);
        toast.HandleMessage(owner, WM_TIMER, 0x50554C54, 0);
        Check(!toast.IsVisible() && toast.Animating(), "automatic expiry uses the same exit transition");
        toast.Show(owner, L"Status", L"Replacement", true);
        draw(L"replacement");
        Check(toast.IsVisible() && toast.Animating(), "a new toast replaces an exiting toast");
        toast.SetMotionEnabled(false);
        draw(L"no-motion");
        toast.HandleMessage(owner, WM_KEYDOWN, VK_ESCAPE, 0);
        Check(!toast.IsVisible() && !toast.Animating(), "disabled motion dismisses immediately");
        toast.SetMotionEnabled(true);
        toast.Show(owner, L"Status", L"High contrast", true);
        draw(L"high-contrast", true);
        Check(!toast.Animating(), "high contrast settles the reveal immediately");
    }
}

void RunDeviceMenus(HWND owner, pulse::ui::Compositor& compositor,
                    const std::filesystem::path& output) {
    using namespace pulse;
    std::filesystem::create_directories(output);
    const auto enabled = [](const auto& items, int command) {
        for (const auto& row : items) if (row.command == command) return row.enabled;
        return false;
    };
    app::CommandContext context;
    context.selected_count = 1;
    context.selection_filesystem = context.device_selection = true;
    context.filesystem_folder = context.can_paste = true;
    app::ContextMenuPrefs prefs;
    prefs.persist = false;
    app::ContextMenuController controller;
    uint32_t token = 0;
    app::ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, HWND, bool, bool, auto) { return ++token; };
    operations.close = [](uint32_t) {};
    controller.SetShellOperations(std::move(operations));
    bool changed = false;
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        const auto file = app::BuildDeviceItemMenu(context, false);
        const auto folder = app::BuildDeviceItemMenu(context, true);
        const auto background = app::BuildDeviceBackgroundMenu(context);
        Check(file.size() == 3 && enabled(file, app::CmdOpen) && enabled(file, app::CmdCopy) &&
            enabled(file, app::CmdProperties) && file[1].shortcut == L"Ctrl+C",
            "phone file menu exposes Open, Copy and Properties before any Shell reply");
        Check(folder.size() == 4 && enabled(folder, app::CmdOpenInNewTab),
            "phone folder menu retains Open in new tab");
        context.selected_count = 2;
        const auto multiple = app::BuildDeviceItemMenu(context, true);
        Check(multiple.size() == 3 && enabled(multiple, app::CmdCopy) &&
            !enabled(multiple, app::CmdOpenInNewTab), "phone multi-selection can copy without single-folder actions");
        context.selected_count = 1;
        Check(background.size() == 2 && enabled(background, app::CmdPaste) &&
            enabled(background, app::CmdNewFolder) && background[0].shortcut == L"Ctrl+V",
            "writable phone background exposes Paste and New folder");
        context.can_paste = false;
        auto unavailable = app::BuildDeviceBackgroundMenu(context);
        Check(!enabled(unavailable, app::CmdPaste) && enabled(unavailable, app::CmdNewFolder),
            "empty clipboard disables Paste while preserving New folder");
        context.read_only = true;
        context.filesystem_folder = false;
        unavailable = app::BuildDeviceBackgroundMenu(context);
        Check(!enabled(unavailable, app::CmdPaste) && !enabled(unavailable, app::CmdNewFolder) &&
            enabled(app::BuildDeviceItemMenu(context, false), app::CmdCopy),
            "read-only phone or device root disables writes while preserving Copy");
        context.read_only = false;
        context.filesystem_folder = context.can_paste = true;
        for (const auto& rows : {file, folder, background, unavailable}) {
            controller.StartQuery(prefs, owner, {L"::pidl:fixture"}, rows.size() == 2,
                L":device:fixture", false, {}, {});
            controller.OpenMenu(rows);
            const auto pending = controller.BuildDisplay(prefs, rows, changed);
            Check(enabled(pending, rows[0].command) == rows[0].enabled,
                "device actions remain available while the Shell menu is loading");
            controller.CompleteComQuery(token, {}, GetTickCount64());
            const auto display = controller.BuildDisplay(prefs, rows, changed);
            Check(display.size() == rows.size(), "menu composition keeps all built-in device actions without Shell items");
            for (const auto& row : rows)
                Check(std::count_if(display.begin(), display.end(), [&](const auto& item) {
                    return item.command == row.command && item.enabled == row.enabled;
                }) == 1,
                    "device action identity and availability survive menu composition");
            for (const float scale : {1.0f, 1.5f}) for (const bool dark : {false, true}) {
                compositor.RecreateTextFormats(scale);
                ui::FluentMenu menu;
                Check(menu.Create(owner, &compositor, scale), "create production device menu renderer");
                menu.SetTheme(dark, D2D1::ColorF(0.0f, 0.47f, 0.84f));
                const auto name = std::wstring(language) + L"-" +
                    (rows.size() == 4 ? L"folder" : rows.size() == 3 ? L"file" :
                        rows[0].enabled ? L"background" : L"readonly") +
                    (dark ? L"-dark-" : L"-light-") + std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                Check(menu.SaveDebugSnapshot((output / name).c_str(), display),
                    "render phone actions through the production popup");
                Check(ui::FluentMenuTestPeer::LabelsFit(menu), "phone menu labels fit at tested theme and DPI");
                Check(ui::FluentMenuTestPeer::ShortcutsFit(menu), "phone menu shortcuts fit without ellipsizing");
            }
            controller.CloseMenu();
            controller.Close();
        }
    }
}

constexpr UINT kReply = WM_APP + 51;
constexpr UINT kOpenOverflow = WM_APP + 52;
struct Reply {
    uint32_t token;
    std::vector<pulse::ops::ShellMenuItem> items;
    bool partial;
    bool failed = false;
};
struct ReplyContext {
    pulse::app::ContextMenuController& controller;
    pulse::ui::FluentMenu& menu;
    pulse::app::ContextMenuPrefs& prefs;
    bool background;
    uint64_t started;
    bool completed = false;
    bool succeeded = false;
    size_t published = 0;
};
LRESULT CALLBACK ReplyProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                            UINT_PTR, DWORD_PTR data) {
    auto& context = *reinterpret_cast<ReplyContext*>(data);
    if (message == kOpenOverflow) {
        pulse::ui::FluentMenuTestPeer::OpenOverflow(context.menu);
        return 0;
    }
    if (message == WM_TIMER && wp == 51) {
        context.menu.Dismiss();
        return 0;
    }
    if (message != kReply) return DefSubclassProc(window, message, wp, lp);
    std::unique_ptr<Reply> reply(reinterpret_cast<Reply*>(lp));
    const auto count = reply->items.size();
    const auto result = context.controller.CompleteComQuery(reply->token,
        std::move(reply->items), GetTickCount64(), reply->partial);
    if (!result.accepted) return 0;
    bool changed = false;
    const bool published = context.menu.ReplaceItems(context.controller.BuildDisplay(
        context.prefs, context.controller.base_items(), changed));
    Check(published, "owner PostMessage dispatch publishes actual Shell reply during the popup modal loop");
    if (published) { context.controller.NotePatchedDisplay(); ++context.published; }
    if (!reply->partial) {
        context.completed = true;
        context.succeeded = !reply->failed;
        std::printf("[INFO] real directory background=%d elapsed=%llu items=%zu published=%zu loading=%d\n",
            context.background, GetTickCount64() - context.started, count, context.published,
            pulse::ui::FluentMenuTestPeer::Loading(context.menu));
        Check(!pulse::ui::FluentMenuTestPeer::Loading(context.menu), "final Shell reply clears the actual open loading flyout");
        context.menu.Dismiss();
    }
    return 0;
}

void RunSynthetic(HWND owner, pulse::ui::Compositor& compositor) {
    using namespace pulse;
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    uint32_t token = 1;
    app::ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, HWND, bool, bool, auto) { return token; };
    operations.close = [](uint32_t) {};
    controller.SetShellOperations(std::move(operations));
    ui::FluentMenuItem builtin; builtin.command = app::CmdOpen; builtin.text = L"Open";
    ui::FluentMenu menu;
    Check(menu.Create(owner, &compositor, 1), "create actual popup for completion regressions");
    bool changed = false;
    controller.StartQuery(prefs, owner, {L"C:\\fixture"}, false, L":folder", false, {}, {});
    controller.OpenMenu({builtin});
    ui::FluentMenuTestPeer::Begin(menu, controller.BuildDisplay(prefs, {builtin}, changed));
    Check(ui::FluentMenuTestPeer::Loading(menu), "open real More options flyout with pending placeholder");
    controller.CompleteComQuery(token, {}, GetTickCount64());
    Check(menu.ReplaceItems(controller.BuildDisplay(prefs, {builtin}, changed)),
        "empty final reply publishes removal of the pending overflow");
    Check(!ui::FluentMenuTestPeer::Loading(menu) && ui::FluentMenuTestPeer::Flyouts(menu) == 0,
        "empty final reply closes the actual loading flyout");
    menu.Dismiss(); controller.CloseMenu(); controller.Close();

    token = 2;
    controller.StartQuery(prefs, owner, {L"C:\\fixture"}, false, L":folder", false, {}, {});
    controller.OpenMenu({builtin});
    ui::FluentMenuTestPeer::Begin(menu, controller.BuildDisplay(prefs, {builtin}, changed));
    ops::ShellMenuItem action; action.id = 13; action.text = L"Fixture software action";
    action.verb = L"fixture.action"; action.clsid = L"{fixture}";
    controller.CompleteComQuery(token, {action}, GetTickCount64(), true);
    Check(menu.ReplaceItems(controller.BuildDisplay(prefs, {builtin}, changed)) &&
        !ui::FluentMenuTestPeer::Loading(menu), "partial current reply replaces the displayed placeholder");
    controller.NotePatchedDisplay();
    controller.CompleteComQuery(token, {}, GetTickCount64());
    Check(menu.ReplaceItems(controller.BuildDisplay(prefs, {builtin}, changed)) &&
        !ui::FluentMenuTestPeer::Loading(menu), "failed final query never restores Loading after a partial reply");
    menu.Dismiss(); controller.CloseMenu(); controller.Close();
    token = 3;
    controller.StartQuery(prefs, owner, {L"C:\\old"}, false, L":folder", false, {}, {});
    token = 4;
    controller.StartQuery(prefs, owner, {L"C:\\new"}, false, L":folder", false, {}, {});
    Check(!controller.CompleteComQuery(3, {action}, GetTickCount64()).accepted,
        "changing directories rejects a late previous-target completion");
    controller.Close();
    Check(!controller.CompleteComQuery(4, {action}, GetTickCount64()).accepted,
        "cancellation rejects a late completion");
}

void RunOpenWith(HWND owner, pulse::ui::Compositor& compositor,
                 const std::filesystem::path& output, bool admin = false) {
    using namespace pulse;
    std::filesystem::create_directories(output);
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    app::StaticVerb chooser;
    chooser.verb = L"openas"; chooser.display = l10n::Get(l10n::StringId::ShellOpenWith);
    app::StaticVerb runas;
    runas.verb = L"runas"; runas.display = l10n::Get(l10n::StringId::ShellRunAs);
    controller.CompleteStaticVerbs(L".txt", admin ? std::vector<app::StaticVerb>{chooser, runas}
        : std::vector<app::StaticVerb>{chooser});
    controller.StartQuery(prefs, owner, {L"C:\\fixture.txt"}, false, L".txt", false, {}, {});
    std::vector<ui::FluentMenuItem> base;
    for (const auto& [command, label] : std::vector<std::pair<int, l10n::StringId>>{
        {app::CmdOpen, l10n::StringId::Open},
        {app::CmdOpenTerminal, l10n::StringId::OpenTerminalHere},
        {app::CmdCopyPath, l10n::StringId::CopyPath},
        {app::CmdProperties, l10n::StringId::Properties}}) {
        ui::FluentMenuItem row; row.command = command; row.text = l10n::Get(label);
        row.separator_after = command == app::CmdOpenTerminal || command == app::CmdCopyPath;
        base.push_back(std::move(row));
    }
    controller.OpenMenu(base);
    bool changed = false;
    const auto display = controller.BuildDisplay(prefs, base, changed);
    if (admin) Check(display.size() == base.size() + 2 && display[1].text == runas.display &&
        display[2].text == chooser.display && display[1].children.empty(),
        "production composition puts Run as administrator after Open and before Open with");
    else Check(display.size() == base.size() + 1 && display[1].text == chooser.display &&
        display[1].children.empty(), "production composition places the chooser directly after Open");
    for (const float scale : {1.0f, 1.5f}) {
        for (const bool dark : {false, true}) {
            ui::FluentMenu menu;
            Check(menu.Create(owner, &compositor, scale), "create production Open With menu renderer");
            menu.SetTheme(dark, D2D1::ColorF(0.0f, 0.47f, 0.84f));
            const auto filename = std::wstring(admin ? L"run-as-" : L"open-with-") +
                std::to_wstring(static_cast<int>(scale * 100)) +
                (dark ? L"-dark.png" : L"-light.png");
            Check(menu.SaveDebugSnapshot((output / filename).c_str(), display, 1),
                "render promoted chooser with the production popup without showing it");
            Check(ui::FluentMenuTestPeer::LabelsFit(menu), "Open With menu labels fit at the tested theme and DPI");
        }
    }
    controller.CloseMenu(); controller.Close();
}

#include "context_menu_sevenzip_cases.h"

void RunLateRunAs(HWND owner, pulse::ui::Compositor& compositor) {
    using namespace pulse;
    app::ContextMenuPrefs prefs; prefs.persist = false;
    app::ContextMenuController controller;
    uint32_t token = 1, invoked_token = 0, invoked_id = 0;
    app::ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, HWND, bool, bool, auto) { return token; };
    operations.close = [](uint32_t) {};
    operations.invoke = [&](uint32_t session, uint32_t id, auto, auto) {
        invoked_token = session; invoked_id = id;
    };
    controller.SetShellOperations(std::move(operations));
    ui::FluentMenuItem open; open.command = app::CmdOpen; open.text = L"Open";
    open.separator_after = true;
    ui::FluentMenuItem properties; properties.command = app::CmdProperties; properties.text = L"Properties";
    const std::vector<ui::FluentMenuItem> base{open, properties};
    controller.StartQuery(prefs, owner, {L"C:\\fixture.exe"}, false, L".exe", false, {}, {});
    controller.OpenMenu(base);
    bool changed = false;
    const auto pending = controller.BuildDisplay(prefs, base, changed);
    ui::FluentMenu menu;
    Check(menu.Create(owner, &compositor, 1), "create actual popup for a cold Run as query");
    ui::FluentMenuTestPeer::Begin(menu, pending);
    ops::ShellMenuItem runas; runas.id = 17; runas.text = L"Run as administrator";
    runas.verb = L"runas"; runas.clsid = L"{runas}"; runas.handler = L"RunAs";
    auto other = runas; other.id = 18; other.text = L"Other software"; other.verb = L"inspect";
    controller.CompleteComQuery(token, {runas, other}, GetTickCount64(), true);
    auto display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() == pending.size() && display[0].command == open.command &&
        display[0].separator_after == pending[0].separator_after &&
        display[1].command == properties.command && display.back().children.size() == 2 &&
        display.back().children.front().command == app::CmdShellComBase + 17,
        "late COM Run as stays in overflow and preserves stable built-in rows");
    const bool partial_published = menu.ReplaceItems(display);
    Check(partial_published, "actual cold popup accepts the late Run as batch");
    if (partial_published) controller.NotePatchedDisplay();
    controller.CompleteComQuery(token, {runas, other}, GetTickCount64());
    display = controller.BuildDisplay(prefs, base, changed);
    const bool final_published = menu.ReplaceItems(display);
    Check(final_published, "final Run as batch replaces the loading overflow");
    if (final_published) controller.NotePatchedDisplay();
    menu.Dismiss(); controller.CloseMenu();
    controller.ExecuteShellCommand(app::CmdShellComBase + 17, {}, {});
    Check(invoked_token == 1 && invoked_id == 17, "late overflow Run as invokes the displayed current session");
    token = 2;
    controller.StartQuery(prefs, owner, {L"C:\\fixture.exe"}, false, L".exe", false, {}, {});
    Check(controller.SeedComItemsFromCache(), "completed Run as batch seeds the next menu");
    controller.OpenMenu(base);
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() == 4 && display[1].text == runas.text && !display[1].enabled,
        "next menu promotes the cached Run as row without authorizing stale commands");
    ui::FluentMenuTestPeer::Begin(menu, display);
    runas.id = 27; other.id = 28;
    controller.CompleteComQuery(token, {runas, other}, GetTickCount64());
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display[1].enabled && display[1].command == app::CmdShellComBase + 27 && menu.ReplaceItems(display),
        "cached promoted Run as validates and remaps in place");
    controller.NotePatchedDisplay(); menu.Dismiss(); controller.CloseMenu();
    controller.ExecuteShellCommand(app::CmdShellComBase + 27, {}, {});
    Check(invoked_token == 2 && invoked_id == 27, "validated promoted Run as invokes the new session");
}

void RunRealShell(HWND window, pulse::ui::Compositor& compositor) {
    using namespace pulse;
    const auto directory = std::filesystem::absolute(L"bench_data/context-menu-loading") /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    ipc::ShellClient::Callbacks callbacks;
    callbacks.ctx_items = [&](uint32_t token, auto items, bool partial, auto) {
        auto reply = std::make_unique<Reply>(Reply{token, std::move(items), partial});
        if (PostMessageW(window, kReply, 0, reinterpret_cast<LPARAM>(reply.get()))) reply.release();
    };
    callbacks.done = [&](uint32_t token, uint32_t hr, bool, auto) {
        std::printf("[INFO] Shell failure token=%u hr=%08x\n", token, hr);
        auto reply = std::make_unique<Reply>(Reply{token, {}, false, true});
        if (PostMessageW(window, kReply, 0, reinterpret_cast<LPARAM>(reply.get()))) reply.release();
    };
    auto& client = ipc::ShellClient::Instance();
    auto callback_owner = client.Acquire(std::move(callbacks));
    for (bool background : {false, true}) {
        app::ContextMenuPrefs prefs; prefs.persist = false;
        app::ContextMenuController controller;
        app::ContextMenuController::ShellOperations operations;
        operations.query = [&](auto paths, HWND owner, bool bg, bool extended, auto disabled) {
            return client.QueryContextMenu(paths, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner)),
                bg, extended, disabled, {}, callback_owner);
        };
        operations.close = [&](uint32_t token) { client.CloseContextMenu(token); };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(prefs, window, {directory.wstring()}, background,
            background ? L":bg" : L":folder", false, {}, {});
        ui::FluentMenuItem builtin; builtin.command = app::CmdOpen; builtin.text = L"Open";
        controller.OpenMenu({builtin});
        bool changed = false;
        ui::FluentMenu menu;
        Check(menu.Create(window, &compositor, 1), "create actual popup for real directory Shell query");
        ReplyContext context{controller, menu, prefs, background, GetTickCount64()};
        SetWindowSubclass(window, ReplyProc, 51, reinterpret_cast<DWORD_PTR>(&context));
        SetTimer(window, 51, 18000, nullptr);
        PostMessageW(window, kOpenOverflow, 0, 0);
        menu.TrackPopup({300, 200}, controller.BuildDisplay(prefs, {builtin}, changed));
        KillTimer(window, 51);
        RemoveWindowSubclass(window, ReplyProc, 51);
        Check(context.completed && context.succeeded,
            background ? "real directory background query finishes without Loading" :
                         "real directory item query finishes without Loading");
        menu.Dismiss(); controller.CloseMenu(); controller.Close();
    }
    client.Release(callback_owner);
    MSG pending{};
    while (PeekMessageW(&pending, window, kReply, kReply, PM_REMOVE))
        delete reinterpret_cast<Reply*>(pending.lParam);
    std::error_code error;
    Check(std::filesystem::remove(directory, error) && !error, "remove exact isolated empty directory fixture");
}
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 700, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        pulse::ui::Compositor compositor;
        Check(owner && compositor.Init(owner), "initialize isolated production compositor");
        if (!failures) {
            if (argc == 3 && wcscmp(argv[1], L"--toast-animation") == 0) RunToastAnimation(owner, compositor, argv[2]);
            else if (argc == 3 && wcscmp(argv[1], L"--device-menus") == 0) RunDeviceMenus(owner, compositor, argv[2]);
            else if (argc == 3 && wcscmp(argv[1], L"--open-with") == 0) RunOpenWith(owner, compositor, argv[2]);
            else if (argc == 3 && wcscmp(argv[1], L"--run-as") == 0) {
                for (const auto* language : {L"zh-CN", L"en-US"}) {
                    pulse::l10n::SetLanguage(language);
                    RunOpenWith(owner, compositor, std::filesystem::path(argv[2]) / language, true);
                }
            }
            else if (argc == 2 && wcscmp(argv[1], L"--runas-live-only") == 0) RunLateRunAs(owner, compositor);
            else if (argc == 2 && wcscmp(argv[1], L"--sevenzip-policy-only") == 0) RunSevenZipPrefs();
            else if (argc == 3 && wcscmp(argv[1], L"--sevenzip") == 0) RunSevenZip(owner, compositor, argv[2]);
            else if (argc == 2 && wcscmp(argv[1], L"--real-shell") == 0) RunRealShell(owner, compositor);
            else RunSynthetic(owner, compositor);
        }
    }
    DestroyWindow(owner);
    CoUninitialize();
    return failures ? 1 : 0;
}
