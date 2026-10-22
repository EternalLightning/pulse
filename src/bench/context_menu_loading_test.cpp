#include "../app/context_menu_controller.h"
#include "../common/localization.h"
#include "../ipc/shell_client.h"

#include <cstdio>
#include <commctrl.h>
#include <filesystem>
#include <memory>
#include <mutex>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
}

namespace pulse::ui {
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
            if (argc == 2 && wcscmp(argv[1], L"--real-shell") == 0) RunRealShell(owner, compositor);
            else RunSynthetic(owner, compositor);
        }
    }
    DestroyWindow(owner);
    CoUninitialize();
    return failures ? 1 : 0;
}
