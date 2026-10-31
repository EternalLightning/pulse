// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/settings_controller.h"
#include "change_tracking_menu_test.h"
#include "folder_actions_menu_cases.h"
#include "session_layout_cases.h"
#include "../app/session.h"
#include "../app/context_menu_controller.h"
#include "../app/quick_access_menu.h"
#include "../app/single_instance_coordinator.h"
#include "../app/tray_controller.h"
#include "../app/blank_pane_click.h"
#include "../app/unc_probe_scheduler.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/user_storage.h"
#include "../ui/panel_metrics.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../shell_host/ctx_handlers.h"
#include "../ipc/ctx_menu_util.h"

#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>

namespace pulse::app {

std::wstring GetPulseDataDir() {
    return {};
}
struct SettingsControllerTestPeer {
    static bool Start(SettingsController& controller, SettingsTask task,
                      SettingsTaskOperation operation, SettingsTaskCompletion completion) {
        return controller.StartTask(std::move(task), std::move(operation),
                                    std::move(completion));
    }
};

} // namespace pulse::app

namespace {

bool Report(const char* name, bool passed) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

const std::vector<pulse::ui::FluentMenuItem>& ShellSection(
    const std::vector<pulse::ui::FluentMenuItem>& items) {
    for (const auto& item : items)
        if (item.glyph == L"\xE712" && item.live_update &&
            item.text == pulse::l10n::Get(pulse::l10n::StringId::MenuMoreOptions)) return item.children;
    static const std::vector<pulse::ui::FluentMenuItem> empty;
    return empty;
}

bool RunContextMenuComTests(pulse::app::ContextMenuPrefs& context) {
    using namespace pulse::app;
    bool passed = true;
    ContextMenuController context_menu;
    uint32_t queried_token = 0;
    uint32_t invoked_command = 0;
    std::wstring invoked_verb;
    std::wstring invoked_text;
    int closed_sessions = 0;
    int folder_refreshes = 0;
    context_menu.SetShellOperations({
        [&](std::vector<std::wstring> paths, HWND, bool background, bool extended,
            std::vector<std::wstring>) {
            queried_token = paths == std::vector<std::wstring>{L"C:\\one.txt"} &&
                !background && !extended ? 17u : 0u;
            return queried_token;
        },
        [&](uint32_t) { ++closed_sessions; },
        [&](uint32_t, uint32_t command, std::wstring verb, std::wstring text) {
            invoked_command = command;
            invoked_verb = std::move(verb);
            invoked_text = std::move(text);
        },
        {}, {},
    });
    passed &= Report("context menu deduplicates static prefetch requests",
        context_menu.RequestStaticPrefetch(L".txt") &&
        !context_menu.RequestStaticPrefetch(L".txt"));
    context_menu.CompleteStaticVerbs(
        L".txt", { { L"edit", L"Edit text", L"" } });
    context_menu.StartQuery(context, nullptr, { L"C:\\one.txt" }, false, L".txt",
        false, [](const std::wstring& path) { return path; }, {});
    passed &= Report("context menu begins a typed shell session",
        queried_token == 17);
    std::vector<pulse::ops::ShellMenuItem> com_items;
    pulse::ops::ShellMenuItem com_item;
    com_item.id = 23;
    com_item.verb = L"customverb";
    com_item.text = L"Custom action";
    com_items.push_back(com_item);
    const auto partial = context_menu.CompleteComQuery(
        17, com_items, GetTickCount64(), true);
    passed &= Report("context menu partial COM snapshot keeps the session open",
        partial.accepted && partial.partial);
    passed &= Report("context menu accepts only the active COM response",
        !context_menu.CompleteComQuery(99, {}, 1200).accepted);
    const auto completion = context_menu.CompleteComQuery(
        17, com_items, GetTickCount64());
    passed &= Report("context menu final COM snapshot finishes the session",
        completion.accepted && !completion.partial && completion.cache_key == L".txt");
    pulse::ui::FluentMenuItem base_item;
    base_item.command = 1;
    base_item.text = L"Base";
    bool prefs_changed = false;
    const auto display = context_menu.BuildDisplay(context, { base_item }, prefs_changed);
    passed &= Report("context menu composes cached static and live COM rows",
        display.size() == 2 && ShellSection(display).size() >= 2 && prefs_changed);
    context_menu.InvalidateCaches();
    passed &= Report("cache invalidation retires the old active COM session",
        !context_menu.CompleteComQuery(17, {com_item}, GetTickCount64()).accepted);
    context_menu.StartQuery(context, nullptr, {L"C:\\one.txt"}, false, L".txt", false, {}, {});
    context_menu.CompleteComQuery(17, {com_item}, GetTickCount64());
    context_menu.OpenMenu({ std::move(base_item) });
    passed &= Report("context menu owns popup baseline state",
        context_menu.menu_open() && !context_menu.base_items().empty());
    pulse::ops::ShellMenuItem live_item = com_item;
    live_item.id = 42;
    passed &= Report("context menu remaps snapshot ids onto the live session",
        context_menu.CompleteComQuery(17, { live_item }, GetTickCount64()).accepted);
    context_menu.CloseMenu();
    context_menu.ScheduleFolderRefresh(2000);
    passed &= Report("context menu owns two-stage refresh deadlines",
        context_menu.ConsumeDueRefreshes(2399) == 0 &&
        context_menu.ConsumeDueRefreshes(2400) == 1 &&
        context_menu.ConsumeDueRefreshes(3600) == 1);
    const bool handled_shell = context_menu.ExecuteShellCommand(
        pulse::app::CmdShellComBase + 23, {}, [&] { ++folder_refreshes; });
    passed &= Report("context menu rejects a snapshot whose live ids differ",
        handled_shell && invoked_command == 0 && closed_sessions == 2 && folder_refreshes == 1);
    context_menu.StartQuery(context, nullptr, { L"C:\\one.txt" }, false, L".txt",
        false, {}, {});
    context_menu.CompleteComQuery(17, {live_item}, GetTickCount64());
    context_menu.OpenMenu({});
    context_menu.CloseMenu();
    context_menu.ExecuteShellCommand(pulse::app::CmdShellComBase + 42, {}, [&] { ++folder_refreshes; });
    passed &= Report("context menu invokes the exact command in the displayed session",
        invoked_command == 42 &&
        invoked_verb == L"customverb" && invoked_text == L"Custom action" &&
        closed_sessions == 2 && folder_refreshes == 2);
    return passed;
}

int worker_event_calls = 0;
int worker_thread_calls = 0;
int worker_fail_event = 0;
bool worker_fail_thread = false;

HANDLE WINAPI CreateWorkerTestEvent(LPSECURITY_ATTRIBUTES attributes, BOOL manual,
                                    BOOL initial, LPCWSTR name) {
    ++worker_event_calls;
    if (worker_event_calls == worker_fail_event) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return CreateEventW(attributes, manual, initial, name);
}

HANDLE WINAPI CreateWorkerTestThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T size,
                                     LPTHREAD_START_ROUTINE entry, LPVOID context,
                                     DWORD flags, LPDWORD id) {
    ++worker_thread_calls;
    if (worker_fail_thread) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return CreateThread(attributes, size, entry, context, flags, id);
}

struct WorkerExitProbe {
    pulse::shell::CtxWorkerHandles* handles = nullptr;
    std::atomic<bool> stopped{false};
};

DWORD WINAPI WaitWorkerExit(void* context) {
    auto& probe = *static_cast<WorkerExitProbe*>(context);
    if (WaitForSingleObject(probe.handles->exit_event, INFINITE) == WAIT_OBJECT_0)
        probe.stopped.store(true);
    return 0;
}

bool RunContextMenuSafetyTests(pulse::app::ContextMenuPrefs& context) {
    using namespace pulse::app;
    bool passed = true;
    for (int failure = 1; failure <= 3; ++failure) {
        worker_event_calls = worker_thread_calls = 0;
        worker_fail_event = failure <= 2 ? failure : 0;
        worker_fail_thread = failure == 3;
        pulse::shell::CtxWorkerHandles handles;
        WorkerExitProbe probe;
        probe.handles = &handles;
        DWORD handles_before = 0;
        const bool counted_before = GetProcessHandleCount(GetCurrentProcess(), &handles_before) != FALSE;
        const bool started = handles.Start(WaitWorkerExit, &probe, CreateWorkerTestEvent, CreateWorkerTestThread);
        const DWORD failure_code = GetLastError();
        DWORD handles_after = 0;
        const bool closed = counted_before && GetProcessHandleCount(GetCurrentProcess(), &handles_after) &&
            handles_before == handles_after;
        passed &= Report(failure == 1 ? "first event failure never starts a worker" :
            failure == 2 ? "second event failure closes the first event without starting a worker" :
            "thread failure closes both events",
            !started && !handles.done_event && !handles.exit_event && !handles.thread && closed &&
            failure_code == ERROR_NOT_ENOUGH_MEMORY && worker_thread_calls == (failure == 3 ? 1 : 0));
    }
    worker_fail_event = 0;
    worker_fail_thread = false;
    worker_event_calls = worker_thread_calls = 0;
    pulse::shell::CtxWorkerHandles handles;
    WorkerExitProbe probe;
    probe.handles = &handles;
    const bool started = handles.Start(WaitWorkerExit, &probe, CreateWorkerTestEvent, CreateWorkerTestThread);
    handles.Stop();
    passed &= Report("worker cleanup waits for its context use to finish",
        started && probe.stopped.load() && !handles.done_event && !handles.exit_event && !handles.thread);

    pulse::shell::CtxItemOut host_item;
    host_item.id = 2;
    host_item.verb = L"custom";
    host_item.text = L"Action";
    const auto find_host = [&](std::vector<pulse::shell::CtxItemOut> items, uint32_t id,
                               const std::wstring& verb = L"custom", const std::wstring& text = L"Action") {
        return pulse::shell::FindItemId(items, id, verb, text);
    };
    passed &= Report("host accepts an exact current enabled leaf command", find_host({host_item}, 2) == 2);
    passed &= Report("host never rebinds an old handler id or a caption-only request",
        find_host({host_item}, 258) == 0 && find_host({host_item}, 0) == 0);
    auto other_handler = host_item;
    other_handler.id = 258;
    other_handler.clsid = L"handler-b";
    passed &= Report("identical captions in different handlers still select the exact id",
        find_host({host_item, other_handler}, 258) == 258);
    passed &= Report("host refuses an id reused for different command text or verb",
        find_host({host_item}, 2, L"other") == 0 && find_host({host_item}, 2, L"custom", L"Other") == 0);
    auto unavailable = host_item;
    unavailable.enabled = false;
    auto header = host_item;
    header.has_children = true;
    passed &= Report("host refuses disabled headers and duplicate command ids",
        find_host({unavailable}, 2) == 0 && find_host({header}, 2) == 0 &&
        find_host({host_item, host_item}, 2) == 0);
    passed &= Report("host refuses commands outside the selected handler range",
        pulse::shell::FindItemId({host_item}, 2, host_item.verb, host_item.text, 257, 512) == 0);

    pulse::ops::ShellMenuItem item;
    item.id = 23;
    item.verb = L"custom";
    item.text = L"Action";
    item.clsid = L"handler-a";
    const auto test_click = [&](const char* name, std::vector<pulse::ops::ShellMenuItem> current,
                                bool expected) {
        ContextMenuController controller;
        int invoked = 0;
        ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 1u; };
        operations.invoke = [&](uint32_t token, uint32_t id, auto, auto) {
            if (token == 1 && id == item.id) ++invoked;
        };
        controller.SetShellOperations(std::move(operations));
        controller.StartQuery(context, nullptr, {L"C:\\one.txt"}, false, L".txt", false, {}, {});
        controller.CompleteComQuery(1, {item}, GetTickCount64());
        controller.OpenMenu({});
        controller.CompleteComQuery(1, std::move(current), GetTickCount64());
        controller.CloseMenu();
        controller.ExecuteShellCommand(CmdShellComBase + static_cast<int>(item.id), {}, {});
        return Report(name, invoked == (expected ? 1 : 0));
    };
    passed &= test_click("controller executes a current snapshot", {item}, true);
    auto changed = item;
    changed.clsid = L"handler-b";
    passed &= test_click("controller refuses a numeric id reused by a different handler", {changed}, false);
    changed = item;
    changed.verb = L"other";
    passed &= test_click("controller refuses a numeric id reused by a different verb", {changed}, false);
    changed = item;
    changed.id = 258;
    auto same_caption = changed;
    same_caption.id = 2;
    passed &= test_click("controller does not choose among duplicate verbs and captions", {changed, same_caption}, false);
    changed = item;
    changed.enabled = false;
    passed &= test_click("controller refuses a command disabled since display", {changed}, false);
    changed = item;
    changed.has_children = true;
    passed &= test_click("controller refuses a command turned into a header", {changed}, false);
    passed &= test_click("controller refuses duplicate live ids", {item, item}, false);

    ContextMenuController cache_controller;
    uint32_t queries = 0;
    uint32_t invokes = 0;
    ContextMenuController::ShellOperations cache_operations;
    cache_operations.query = [&](auto, HWND, bool, bool, auto) { return ++queries; };
    cache_operations.invoke = [&](uint32_t, uint32_t, auto, auto) { ++invokes; };
    cache_controller.SetShellOperations(std::move(cache_operations));
    cache_controller.StartQuery(context, nullptr, {L"C:\\one.txt"}, false, L".txt", false, {}, {});
    cache_controller.CompleteComQuery(1, {item}, GetTickCount64());
    cache_controller.StartQuery(context, nullptr, {L"C:\\one.txt"}, false, L".txt", true, {}, {});
    passed &= Report("extended mode starts a new same-path session with its own display cache",
        queries == 2 && !cache_controller.SeedComItemsFromCache());
    const auto extended_result = cache_controller.CompleteComQuery(2, {item}, GetTickCount64());
    passed &= Report("extended timing and cache keys remain distinct", extended_result.cache_key == L".txt:extended");
    cache_controller.Close();
    cache_controller.StartQuery(context, nullptr, {L"C:\\two.txt"}, false, L".txt", false, {}, {});
    const bool seeded = cache_controller.SeedComItemsFromCache();
    cache_controller.OpenMenu({});
    bool cache_prefs_changed = false;
    auto pending_display = cache_controller.BuildDisplay(context, {}, cache_prefs_changed);
    const auto pending_com_enabled = [&](const auto& rows) {
        for (const auto& row : rows) {
            if (row.command >= CmdShellComBase && row.enabled) return true;
            for (const auto& child : row.children)
                if (child.command >= CmdShellComBase && child.enabled) return true;
        }
        return false;
    };
    passed &= Report("cached COM presentation is disabled until this session completes",
        seeded && !pending_com_enabled(pending_display));
    cache_controller.CompleteComQuery(3, {item}, GetTickCount64(), true);
    pending_display = cache_controller.BuildDisplay(context, {}, cache_prefs_changed);
    passed &= Report("current partial COM presentation enables its already queried commands",
        pending_com_enabled(pending_display));
    cache_controller.CompleteComQuery(3, {item}, GetTickCount64());
    pending_display = cache_controller.BuildDisplay(context, {}, cache_prefs_changed);
    passed &= Report("completed current COM presentation enables its command",
        pending_com_enabled(pending_display));
    cache_controller.ExecuteShellCommand(CmdShellComBase + static_cast<int>(item.id), {}, {});
    passed &= Report("cached rows cannot authorize a command in a new session", seeded && invokes == 0);
    cache_controller.StartQuery(context, nullptr, {L"C:\\two.txt"}, false, L".txt", false, {}, {});
    cache_controller.SeedComItemsFromCache();
    cache_controller.OpenMenu({});
    cache_controller.CompleteComQuery(4, {item}, GetTickCount64());
    cache_controller.NotePatchedDisplay();
    cache_controller.ExecuteShellCommand(CmdShellComBase + static_cast<int>(item.id), {}, {});
    passed &= Report("a patched current-session display can replace cached presentation safely", invokes == 1);

    ContextMenuController partial_controller;
    uint32_t partial_invokes = 0, partial_waits = 0;
    ContextMenuController::ShellOperations partial_operations;
    partial_operations.query = [](auto, HWND, bool, bool, auto) { return 1u; };
    partial_operations.invoke = [&](uint32_t, uint32_t id, auto, auto) {
        if (id == item.id) ++partial_invokes;
    };
    partial_controller.SetShellOperations(std::move(partial_operations));
    partial_controller.StartQuery(context, nullptr, {L"C:\\one.txt"}, false, L".txt", false, {}, {});
    partial_controller.CompleteComQuery(1, {item}, GetTickCount64(), true);
    partial_controller.OpenMenu({});
    partial_controller.CloseMenu();
    partial_controller.ExecuteShellCommand(CmdShellComBase + static_cast<int>(item.id),
        [&] { ++partial_waits; }, {});
    passed &= Report("displayed current partial command invokes without waiting for unrelated handlers",
        partial_invokes == 1 && partial_waits == 0);

    ContextMenuController subset_controller;
    uint32_t subset_queries = 0, subset_invoked = 0, subset_waits = 0;
    auto missing = item;
    missing.id = 258; missing.clsid = L"handler-pending"; missing.text = L"Pending action";
    auto ready = item;
    ready.id = missing.id;
    ContextMenuController::ShellOperations subset_operations;
    subset_operations.query = [&](auto, HWND, bool, bool, auto) { return ++subset_queries; };
    subset_operations.invoke = [&](uint32_t token, uint32_t id, auto, auto) {
        if (token == 2) subset_invoked = id;
    };
    subset_controller.SetShellOperations(std::move(subset_operations));
    subset_controller.StartQuery(context, nullptr, {L"C:\\one.txt"}, false, L".txt", false, {}, {});
    subset_controller.CompleteComQuery(1, {item, missing}, GetTickCount64());
    subset_controller.Close();
    subset_controller.StartQuery(context, nullptr, {L"C:\\two.txt"}, false, L".txt", false, {}, {});
    subset_controller.SeedComItemsFromCache();
    subset_controller.OpenMenu({});
    bool subset_prefs_changed = false;
    const auto cached_display = subset_controller.BuildDisplay(context, {}, subset_prefs_changed);
    const auto cached_layout = ShellSection(cached_display);
    subset_controller.CompleteComQuery(2, {ready}, GetTickCount64(), true);
    const auto ready_display = subset_controller.BuildDisplay(context, {}, subset_prefs_changed);
    const auto ready_layout = ShellSection(ready_display);
    passed &= Report("ready partial subset enables its matching cached row while preserving pending rows and geometry",
        cached_layout.size() == ready_layout.size() && ready_layout.size() >= 2 &&
        cached_layout[0].text == ready_layout[0].text && cached_layout[1].text == ready_layout[1].text &&
        !cached_layout[0].enabled && !cached_layout[1].enabled && ready_layout[0].enabled &&
        !ready_layout[1].enabled && ready_layout[0].command == CmdShellComBase + static_cast<int>(ready.id) &&
        ready_layout[0].command != ready_layout[1].command);
    subset_controller.NotePatchedDisplay();
    subset_controller.CloseMenu();
    subset_controller.ExecuteShellCommand(ready_layout.front().command, [&] { ++subset_waits; }, {});
    passed &= Report("partial subset executes only its current exact handler id without waiting for the missing handler",
        subset_invoked == ready.id && subset_waits == 0);

    for (int mode = 0; mode != 4; ++mode) {
        ContextMenuController controller;
        std::vector<std::wstring> selected, executed;
        for (int i = 0; i != 19; ++i) selected.push_back(L"C:\\selected-" + std::to_wstring(i) + L".txt");
        StaticVerb verb;
        verb.verb = mode == 0 ? L"runas" : L"custom";
        verb.display = L"Action";
        if (mode == 1) verb.app_path = L"tool.exe";
        if (mode == 2) verb.command = L"tool.exe \"%1\"";
        ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 1u; };
        operations.execute_verb = [&](const auto& path, const auto&) { executed.push_back(path); };
        operations.open_with_app = [&](const auto&, const auto& path) { executed.push_back(path); };
        operations.execute_command = [&](const auto&, const auto& path) { executed.push_back(path); };
        controller.SetShellOperations(std::move(operations));
        controller.CompleteStaticVerbs(L".txt", {verb});
        controller.StartQuery(context, nullptr, selected, false, L".txt", false, {}, {});
        controller.ExecuteShellCommand(CmdShellStaticBase, {}, {});
        passed &= Report("static shell command delivers every selected target beyond sixteen", executed == selected);
    }
    return passed;
}

} // namespace

#include "context_menu_refresh_cases.h"
#include "context_menu_overflow_cases.h"
#include "builtin_menu_contract_cases.h"


int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--folder-actions-menu") {
        int failures = 0;
        RunFolderActionsMenuChecks([&](bool passed, const char* message) {
            if (!Report(message, passed)) ++failures;
        });
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--recent-changes-menu") {
        int failures = 0;
        RunRecentChangesMenuChecks([&](bool passed, const char* message) {
            if (!Report(message, passed)) ++failures;
        });
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--builtin-menu-contract") {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        return RunBuiltinMenuContractChecks();
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--context-settings-save") {
        using namespace pulse::app;
        bool passed = true;
        AppPrefs prefs;
        prefs.persist = false;
        ContextMenuPrefs context;
        context.persist = true;
        const auto handler = pulse::ipc::HandlerCatalogKey(L"{11111111-2222-3333-4444-555555555555}");
        context.RecordSeen(handler, L"Example handler", false, pulse::ipc::CtxMenuCategory::Software, true);
        context.SetComDisabled(handler, true);
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        SettingsController settings;
        int effects = 0;
        SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](SettingsEffect effect) { if (effect != SettingsEffect::None) ++effects; };
        settings.BindUi(prefs, context, index, network, std::move(callbacks));
        const std::wstring before = context.ToJson();
        settings.ToggleUi(kBuiltinGroupToggle);
        passed &= Report("builtin group save failure preserves menu settings and reports an error",
                         context.ToJson() == before && !settings.error().empty() && effects == 0);
        settings.ToggleUi(10);
        passed &= Report("category save failure preserves menu settings",
                         context.ToJson() == before && !settings.error().empty());
        settings.ToggleUi(100 + static_cast<int>(context.seen.size()));
        passed &= Report("builtin row save failure preserves menu settings and effects",
                         context.ToJson() == before && !settings.error().empty() && effects == 0);
        settings.ToggleUi(100);
        passed &= Report("handler save failure preserves both its row override and skip marker",
                         context.ToJson() == before && context.ComDisabled(handler) && !settings.error().empty());
        context.persist = false;
        settings.ToggleUi(100);
        passed &= Report("one successful toggle restores a skipped handler and clears the save error",
                         context.RowEnabled(handler, pulse::ipc::CtxMenuCategory::Software, true) &&
                         !context.ComDisabled(handler) && settings.error().empty());
        settings.ResetUi();
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-overflow") {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        return RunContextMenuOverflowTests() ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--native-new") {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 2;
        const auto directory = std::filesystem::absolute(L"bench_data/native-new") /
            std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(directory);
        bool passed = false;
        {
            pulse::shell::CtxBind bind;
            const bool bound = pulse::shell::BindCtxSelection({directory.wstring()}, true, bind);
            passed = Report("native New test binds its isolated directory", bound);
            if (bound) {
                pulse::shell::CtxHandlerDesc desc;
                desc.clsid_text = L"{d969a300-e7ff-11d0-a93b-00a0c90f2719}";
                desc.name = L"New";
                CLSIDFromString(desc.clsid_text.c_str(), &desc.clsid);
                pulse::shell::CtxHandlerSlot slot;
                const auto hr = pulse::shell::QueryOneHandler(desc, bind, nullptr, 1,
                    256, CMF_NORMAL, slot);
                std::vector<pulse::shell::CtxItemOut> items;
                pulse::shell::CollectHandlerItems(slot, true, items);
                bool native = false, duplicate = false;
                for (const auto& item : items) {
                    if (!item.child) continue;
                    native |= item.enabled && item.id != 0;
                    duplicate |= pulse::ipc::IsPulseNewTemplateVerb(item.verb);
                    char label[2048]{};
                    WideCharToMultiByte(CP_UTF8, 0, item.text.c_str(), -1, label,
                        sizeof(label), nullptr, nullptr);
                    std::printf("[TEMPLATE] %s\n", label);
                }
                passed &= Report("native New handler exposes installed templates",
                    SUCCEEDED(hr) && native);
                passed &= Report("native Folder and Text templates are filtered", !duplicate);
                pulse::shell::ReleaseHandlerSlot(slot);
            }
        }
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        passed &= Report("native New test removes only its isolated fixture", !cleanup_error);
        CoUninitialize();
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--session-layout")
        return pulse::bench::RunSessionLayoutTests() ? 0 : 1;
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-refresh") {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        return RunContextMenuRefreshTests() ? 0 : 1;
    }
    if (argc == 2 && wcscmp(argv[1], L"--unc-probe-only") == 0) {
        bool ok = true;
        using pulse::app::UncProbeScheduler;
        UncProbeScheduler probes;
        const std::wstring path = L"probe-path", other = L"other-path";
        const auto a = probes.Begin(path);
        ok &= Report("logical timeout releases slot but retains latest path ticket",
            probes.Finish(a) && probes.Accept(path, a, false));
        ok &= Report("latest late success remains eligible without a newer request",
            probes.Accept(path, a) && !probes.Accept(path, a));
        const auto old = probes.Begin(path); probes.Finish(old);
        const auto newer = probes.Begin(path);
        ok &= Report("stale result cannot release a newer scheduling slot",
            !probes.Finish(old) && probes.IsActive(newer));
        ok &= Report("newer same-path completion supersedes old late outcome",
            probes.Accept(path, newer) && !probes.Accept(path, old));
        probes.Finish(newer);
        const auto pending_old = probes.Begin(path); probes.Finish(pending_old);
        ok &= Report("queued replacement reserves freshness before physical admission",
            probes.Enqueue(path) && !probes.Accept(path, pending_old) && probes.Pending());
        const auto queued_id = probes.Pending()->id;
        probes.BeginPending();
        ok &= Report("queued admission retains its original request identity",
            probes.IsActive(queued_id) && probes.Accept(path, queued_id, false));
        probes.Finish(queued_id);
        const auto own = probes.Begin(other);
        ok &= Report("unrelated path admission preserves a valid late result",
            probes.Accept(path, queued_id) && probes.IsActive(own));
        probes.Finish(own); probes.Accept(other, own);
        UncProbeScheduler restarted;
        ok &= Report("new scheduler lifetime cannot reuse outstanding request ids",
            restarted.Begin(path) != own && probes.latest.empty());
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-quick-access") {
        using namespace pulse::app;
        setvbuf(stdout, nullptr, _IONBF, 0);
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        bool passed = true;
        pulse::ui::FluentMenuItem pin;
        pin.command = CmdPinQuickAccess;
        pin.text = L"固定到快速访问";
        ShellMenuEntry native;
        native.command = CmdShellComBase + 71;
        native.verb = L"PiNtOhOmE";
        native.text = L"Pin to Quick access (&H)";
        ShellMenuEntry vendor;
        vendor.command = CmdShellComBase + 72;
        vendor.verb = L"vendor.pin";
        vendor.text = pin.text;
        ShellMenuEntry unknown = vendor;
        unknown.command = CmdShellComBase + 73;
        unknown.verb.clear();
        unknown.clsid = L"{fixture-vendor}";
        std::vector<pulse::ui::FluentMenuItem> display{pin};
        AppendShellSection(display, {native, vendor, unknown, vendor});
        passed &= Report("quick access merge removes native pin by canonical identity across localized captions",
            display.size() == 3 && display.front().command == CmdPinQuickAccess);
        passed &= Report("quick access merge retains distinct and unknown third-party commands with the same caption",
            display.size() == 3 && display[1].command == vendor.command && display[2].command == unknown.command);
        passed &= Report("shell merge removes a repeated command identity rather than a matching caption",
            std::count_if(display.begin(), display.end(), [&](const auto& row) { return row.command == vendor.command; }) == 1);
        pin.command = CmdUnpinQuickAccess;
        pin.text = L"从快速访问取消固定";
        native.verb = L"unpinfromhome";
        native.text = L"Unpin from Quick access";
        display = {pin};
        AppendShellSection(display, {native});
        passed &= Report("all-pinned selections retain Pulse unpin and suppress only the equivalent native command",
            display.size() == 1 && display.front().command == CmdUnpinQuickAccess);
        display.clear();
        AppendShellSection(display, {native});
        passed &= Report("merge does not disable a native pin command when Pulse has no equivalent entry",
            display.size() == 1 && display.front().command == native.command);
        ShellMenuEntry group;
        group.text = L"Vendor tools";
        group.children = {native, unknown};
        display = {pin};
        AppendShellSection(display, {group});
        passed &= Report("submenu dedup keeps unknown vendor commands and removes only the known native equivalent",
            display.size() == 2 && display.back().children.size() == 1 && display.back().children.front().command == unknown.command);
        ContextMenuPrefs prefs;
        prefs.persist = false;
        PlacesCatalog places;
        places.persist = false;
        const std::vector<std::wstring> folders{L"C:\\one", L"C:\\two"};
        passed &= Report("load conflicting legacy and current menu configuration",
            prefs.FromJson(LR"({"pulse_items":{"quick_access":true},"items":{"pulse:quick-access":{"enabled":false}}})"));
        display.clear();
        const auto rebuild_quick_access = [&] {
            AppendQuickAccessMenuCommand(display, places, folders);
            display = ApplyBuiltinMenuPrefs(std::move(display), prefs, BuiltinMenuSurface::Item);
        };
        rebuild_quick_access();
        passed &= Report("enabled built-in pin survives a disabled obsolete catalog switch",
            display.size() == 1 && display.front().command == CmdPinQuickAccess &&
            display.front().text == L"固定到快速访问" && prefs.seen.empty());
        rebuild_quick_access();
        passed &= Report("rebuilding pin does not duplicate the command", display.size() == 1);
        places.SetQuickAccessPinned({folders.front()}, true);
        rebuild_quick_access();
        passed &= Report("mixed folder pin state offers pin", display.size() == 1 &&
            display.front().command == CmdPinQuickAccess);
        places.SetQuickAccessPinned(folders, true);
        rebuild_quick_access();
        passed &= Report("all pinned folders offer unpin", display.size() == 1 &&
            display.front().command == CmdUnpinQuickAccess);
        prefs.SetBuiltinItemEnabled(BuiltinMenuItem::QuickAccess, false);
        rebuild_quick_access();
        passed &= Report("current built-in switch hides the pin command", display.empty());
        prefs.SetBuiltinItemEnabled(BuiltinMenuItem::QuickAccess, true);
        rebuild_quick_access();
        passed &= Report("current built-in switch restores unpin despite the obsolete override",
            display.size() == 1 && display.front().command == CmdUnpinQuickAccess);
        AppendQuickAccessMenuCommand(display, places, {});
        passed &= Report("ineligible selection clears a stale pin command", display.empty());
        ContextMenuController controller;
        ContextMenuController::ShellOperations operations;
        operations.query = [](auto, HWND, bool, bool, auto) { return 19u; };
        controller.SetShellOperations(std::move(operations));
        controller.MergeStaticCache({{L":folder", {{L"pintohome", L"Pin to Quick access (&H)", L""}}}});
        controller.StartQuery(prefs, nullptr, {L"C:\\fixture"}, false, L":folder", false, {}, {});
        bool changed = false;
        pin.command = CmdPinQuickAccess;
        display = controller.BuildDisplay(prefs, {pin}, changed);
        passed &= Report("old registry cache pin entries do not duplicate the Pulse command in the displayed menu",
            std::none_of(display.begin(), display.end(), [](const auto& row) {
                return row.command >= CmdShellStaticBase && row.command < CmdShellComBase;
            }));
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-safety") {
        setvbuf(stdout, nullptr, _IONBF, 0);
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        pulse::app::ContextMenuPrefs context;
        context.persist = false;
        return RunContextMenuSafetyTests(context) ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-com") {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        pulse::app::ContextMenuPrefs context;
        context.persist = false;
        return RunContextMenuComTests(context) ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--context-menu-runas") {
        using namespace pulse::app;
        ContextMenuPrefs prefs;
        prefs.persist = false;
        ContextMenuController controller;
        std::wstring executed_path, executed_verb, executed_command;
        int verb_calls = 0;
        int command_calls = 0;
        ContextMenuController::ShellOperations operations;
        operations.execute_verb = [&](const std::wstring& path, const std::wstring& verb) {
            executed_path = path;
            executed_verb = verb;
            ++verb_calls;
        };
        operations.execute_command = [&](const std::wstring& command, const std::wstring&) {
            executed_command = command;
            ++command_calls;
        };
        controller.SetShellOperations(std::move(operations));
        const std::wstring path = L"C:\\Program Files\\Example\\app.exe";
        StaticVerb runas;
        runas.verb = L"RuNaS";
        runas.display = L"Run as administrator";
        runas.command = L"\"%1\" %*";
        controller.CompleteStaticVerbs(L".exe", { runas });
        controller.StartQuery(prefs, nullptr, { path }, false, L".exe", false, {}, {});
        const bool handled = controller.ExecuteShellCommand(CmdShellStaticBase, {}, {});
        bool ok = Report("runas preserves elevation despite a cached registry command",
            handled && verb_calls == 1 && command_calls == 0 &&
            executed_path == path && executed_verb == L"runas");
        StaticVerb custom;
        custom.verb = L"custom";
        custom.display = L"Custom command";
        custom.command = L"tool.exe \"%1\"";
        controller.CompleteStaticVerbs(L".exe", { custom });
        controller.StartQuery(prefs, nullptr, { path }, false, L".exe", false, {}, {});
        controller.ExecuteShellCommand(CmdShellStaticBase, {}, {});
        ok &= Report("ordinary static commands retain their command template",
            verb_calls == 1 && command_calls == 1 && executed_command == custom.command);
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--external-open-prefs") {
        using pulse::app::AppPrefs;
        using pulse::app::ExternalOpenMode;
        AppPrefs prefs, loaded;
        prefs.persist = loaded.persist = false;
        bool ok = Report("external folder opens reuse an open location by default",
            prefs.external_open_mode == ExternalOpenMode::ReuseOpenedLocation);
        for (auto mode : {ExternalOpenMode::ReuseOpenedLocation, ExternalOpenMode::Tab, ExternalOpenMode::NewWindow}) {
            prefs.external_open_mode = mode;
            ok &= Report("external open choice survives a preferences round trip",
                loaded.FromJson(prefs.ToJson()) && loaded.external_open_mode == mode);
        }
        for (const auto* json : {L"{}", L"{\"external_open_mode\":-1}", L"{\"external_open_mode\":3}",
                L"{\"external_open_mode\":\"window\"}"}) {
            loaded.external_open_mode = ExternalOpenMode::NewWindow;
            ok &= Report("missing or invalid external open preferences restore the default",
                loaded.FromJson(json) && loaded.external_open_mode == ExternalOpenMode::ReuseOpenedLocation);
        }
        loaded.external_open_mode = ExternalOpenMode::NewWindow;
        loaded.ResetToDefaults();
        ok &= Report("reset restores the external open default",
            loaded.external_open_mode == ExternalOpenMode::ReuseOpenedLocation);
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--new-tab-home") {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporary)) return 1;
        pulse::storage::OverrideDefaultRootForTesting(std::wstring(temporary) +
            L"pulse-new-tab-prefs-" + std::to_wstring(GetCurrentProcessId()));
        pulse::app::AppPrefs prefs;
        prefs.persist = false;
        pulse::app::ContextMenuPrefs context;
        context.persist = false;
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        pulse::app::SettingsController settings;
        settings.BindUi(prefs, context, index, network, {});
        bool ok = Report("new tabs duplicate the current page by default", !prefs.new_tab_home);
        settings.ToggleUi(20);
        pulse::app::AppPrefs loaded;
        loaded.persist = false;
        ok &= Report("Home selection persists", prefs.new_tab_home &&
            loaded.FromJson(prefs.ToJson()) && loaded.new_tab_home);
        settings.ToggleUi(20);
        ok &= Report("duplicate selection persists", !prefs.new_tab_home &&
            loaded.FromJson(prefs.ToJson()) && !loaded.new_tab_home);
        loaded.new_tab_home = true;
        ok &= Report("legacy preferences keep duplicate behavior", loaded.FromJson(L"{}") && !loaded.new_tab_home);
        loaded.new_tab_home = true;
        loaded.ResetToDefaults();
        ok &= Report("reset restores duplicate behavior", !loaded.new_tab_home);
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--layout-search-prefs") {
        pulse::app::AppPrefs prefs;
        prefs.persist = false;
        prefs.sidebar_width = 360;
        prefs.address_search_current = true;
        prefs.address_search_content = true;
        pulse::app::AppPrefs loaded;
        loaded.persist = false;
        loaded.FromJson(prefs.ToJson());
        bool ok = Report("sidebar and search preferences round trip", loaded.sidebar_width == 360 &&
            loaded.address_search_current && loaded.address_search_content);
        loaded.FromJson(L"{\"sidebar_width\":9999}");
        ok &= Report("invalid width restores default", loaded.sidebar_width == 224);
        loaded.FromJson(L"{}");
        ok &= Report("old preferences load defaults", loaded.sidebar_width == 224 &&
            !loaded.address_search_current && !loaded.address_search_content);
        loaded.ResetToDefaults();
        ok &= Report("reset restores sidebar width", loaded.sidebar_width == 224);
        {
            // The sidebar must leave room for both the file list and the shared toolbar.
            using namespace pulse::ui;
            ok &= Report("panel limit: without details the shared toolbar retains 360 DIP",
                std::abs(MaxSidebarWidthDip(1600.0f, 340.0f, false, 8.0f) - 1240.0f) < 0.5f);
            ok &= Report("panel limit: an open details panel is reserved by the sidebar",
                std::abs(MaxSidebarWidthDip(1600.0f, 340.0f, true, 8.0f) - 1140.0f) < 0.5f);
            ok &= Report("panel limit: the details panel keeps the sidebar and the list",
                std::abs(MaxDetailsWidthDip(1600.0f, 224.0f, false, 8.0f) - 1256.0f) < 0.5f);
            ok &= Report("panel limit: a collapsed sidebar leaves its rail",
                std::abs(MaxDetailsWidthDip(880.0f, 224.0f, true, 8.0f) - 712.0f) < 0.5f);
            ok &= Report("panel limit: narrow windows stop at the pane minimums",
                MaxSidebarWidthDip(400.0f, 340.0f, true, 8.0f) == kSidebarMinWidthDip &&
                MaxDetailsWidthDip(400.0f, 224.0f, false, 8.0f) == kDetailsMinWidthDip);
        }
        return ok ? 0 : 1;
    }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    if (argc == 2 && std::wstring(argv[1]) == L"--global-search") {
        pulse::app::AppPrefs prefs;
        prefs.persist = false;
        bool ok = Report("global search defaults to off with Alt Space", !prefs.global_search_enabled && prefs.global_search_modifiers == MOD_ALT && prefs.global_search_key == VK_SPACE);
        pulse::app::ContextMenuPrefs context;
        context.persist = false;
        pulse::index::IndexClient index;
        pulse::index::NetworkAgentClient network;
        pulse::app::SettingsController settings;
        pulse::app::SettingsEffect effect = pulse::app::SettingsEffect::None;
        pulse::app::SettingsController::UiCallbacks callbacks;
        callbacks.apply_effects = [&](pulse::app::SettingsEffect value) { effect = value; };
        settings.BindUi(prefs, context, index, network, std::move(callbacks));
        settings.ToggleUi(15);
        ok &= Report("global search toggle applies registration", prefs.global_search_enabled && pulse::app::HasEffect(effect, pulse::app::SettingsEffect::GlobalSearch));
        settings.BeginGlobalSearchHotkeyCapture();
        settings.CaptureGlobalSearchHotkey(VK_CONTROL, MOD_CONTROL);
        settings.CaptureGlobalSearchHotkey('A', 0);
        ok &= Report("invalid shortcut keeps capture and previous binding", settings.global_search_hotkey_capturing() && prefs.global_search_key == VK_SPACE);
        settings.CaptureGlobalSearchHotkey(VK_ESCAPE, 0);
        ok &= Report("Escape cancels shortcut capture", !settings.global_search_hotkey_capturing() && prefs.global_search_key == VK_SPACE);
        settings.BeginGlobalSearchHotkeyCapture();
        effect = pulse::app::SettingsEffect::None;
        settings.CaptureGlobalSearchHotkey('K', MOD_CONTROL | MOD_SHIFT);
        ok &= Report("shortcut capture saves and applies combination", !settings.global_search_hotkey_capturing() && prefs.global_search_key == 'K' && prefs.global_search_modifiers == (MOD_CONTROL | MOD_SHIFT) && pulse::app::HasEffect(effect, pulse::app::SettingsEffect::GlobalSearch));
        pulse::app::AppPrefs loaded;
        loaded.persist = false;
        loaded.FromJson(prefs.ToJson());
        ok &= Report("global search preferences round trip", loaded.global_search_enabled && loaded.global_search_key == 'K' && loaded.global_search_modifiers == (MOD_CONTROL | MOD_SHIFT));
        loaded.FromJson(L"{}");
        ok &= Report("older preferences retain safe defaults", !loaded.global_search_enabled && loaded.global_search_key == VK_SPACE && loaded.global_search_modifiers == MOD_ALT);
        prefs.persist = true; // This executable's data-directory stub is empty, so Save fails without touching disk.
        effect = pulse::app::SettingsEffect::None;
        settings.BeginGlobalSearchHotkeyCapture();
        settings.CaptureGlobalSearchHotkey('L', MOD_ALT);
        ok &= Report("failed shortcut save rolls back and leaves registration unchanged", prefs.global_search_key == 'K' &&
            prefs.global_search_modifiers == (MOD_CONTROL | MOD_SHIFT) && effect == pulse::app::SettingsEffect::None && settings.global_search_hotkey_capturing());
        settings.ToggleUi(15);
        ok &= Report("failed toggle save rolls back without registration", prefs.global_search_enabled && effect == pulse::app::SettingsEffect::None);
        prefs.persist = false;
        settings.ResetUi();
        ok &= Report("settings reset cancels capture", !settings.global_search_hotkey_capturing());
        return ok ? 0 : 1;
    }
    using pulse::app::HasEffect;
    using pulse::app::SettingsController;
    using pulse::app::SettingsEffect;
    using pulse::app::ContextMenuController;
    using pulse::app::SingleInstanceCoordinator;
    using pulse::app::TrayController;

    bool passed = true;

    {
        pulse::app::UncProbeScheduler probes;
        const uint64_t first = probes.Begin();
        const bool first_completed = probes.Finish(first);
        const uint64_t second = probes.Begin();
        const bool stale_completed = probes.Finish(first);
        const bool second_still_active = probes.IsActive(second);
        const bool second_completed = probes.Finish(second);
        passed &= Report("late UNC result cannot finish a newer same-path probe",
                         first_completed && first != second && !stale_completed &&
                         second_still_active && second_completed && probes.active_id == 0);
    }

    passed &= Report("display paths strip extended UNC prefixes without losing the server",
        pulse::path::StripExtendedPathPrefix(L"\\\\?\\UNC\\192.168.0.254\\资料\\项目") ==
            L"\\\\192.168.0.254\\资料\\项目");
    passed &= Report("display paths preserve normal and long local paths",
        pulse::path::StripExtendedPathPrefix(L"C:\\资料") == L"C:\\资料" &&
        pulse::path::StripExtendedPathPrefix(L"\\\\?\\C:\\" + std::wstring(300, L'a')) ==
            L"C:\\" + std::wstring(300, L'a'));
    {
        pulse::app::BlankPaneClickRelease click;
        click.pending = true;
        click.owns_capture = true;
        click.same_context = true;
        click.blank_list_hit = true;
        click.drag_width = 4;
        click.drag_height = 6;
        const auto accepts = pulse::app::IsBlankPaneBackClick;
        passed &= Report("blank pane click navigates back on release", accepts(click));
        auto changed = click;
        changed.delta_x = -3;
        changed.delta_y = 5;
        passed &= Report("blank pane click tolerates motion below drag threshold", accepts(changed));
        bool rejects_boundary = true;
        for (const int delta : {-4, 4}) {
            changed = click;
            changed.delta_x = delta;
            rejects_boundary &= !accepts(changed);
        }
        for (const int delta : {-6, 6}) {
            changed = click;
            changed.delta_y = delta;
            rejects_boundary &= !accepts(changed);
        }
        passed &= Report("blank pane release at either drag threshold never goes back", rejects_boundary);
        changed = click;
        changed.marquee_active = true;
        passed &= Report("marquee returning to its origin never goes back", !accepts(changed));
        changed = click;
        changed.modified = true;
        passed &= Report("modified blank pane release never goes back", !accepts(changed));
        changed = click;
        changed.owns_capture = false;
        passed &= Report("capture loss cancels blank pane back", !accepts(changed));
        changed = click;
        changed.same_context = false;
        passed &= Report("changed pane tab or view cancels blank pane back", !accepts(changed));
        changed = click;
        changed.blank_list_hit = false;
        passed &= Report("row control and outside-list release never go back", !accepts(changed));
        changed = click;
        changed.pending = false;
        passed &= Report("cancelled or second double-click release never goes back", !accepts(changed));
    }
    const std::wstring mutex_name = L"Local\\Pulse.ControllerTest." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    SingleInstanceCoordinator primary;
    SingleInstanceCoordinator duplicate;
    passed &= Report("single-instance coordinator owns the primary mutex",
        primary.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Primary);
    passed &= Report("single-instance coordinator detects a duplicate",
        duplicate.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Existing);
    primary.Release();
    passed &= Report("single-instance mutex is released by its owner",
        duplicate.Acquire(mutex_name) == SingleInstanceCoordinator::AcquireResult::Primary);

    std::wstring decoded;
    std::wstring path = L"C:\\目录\\file.txt";
    COPYDATASTRUCT data{};
    data.dwData = SingleInstanceCoordinator::OpenPathMessageId();
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = path.data();
    passed &= Report("single-instance IPC decodes a terminated UTF-16 path",
        SingleInstanceCoordinator::DecodeOpenPath(&data, decoded) && decoded == path);
    data.cbData -= sizeof(wchar_t);
    passed &= Report("single-instance IPC rejects a non-terminated path",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));
    data.cbData = 3;
    passed &= Report("single-instance IPC rejects odd byte counts",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));
    wchar_t embedded[] = {L'C', L':', L'\0', L'x', L'\0'};
    data.cbData = sizeof(embedded);
    data.lpData = embedded;
    passed &= Report("single-instance IPC rejects embedded NUL characters",
        !SingleInstanceCoordinator::DecodeOpenPath(&data, decoded));

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
        L"Pulse tray controller test", WS_POPUP, -32000, -32000, 100, 100,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    TrayController tray;
    tray.Attach(hwnd, GetModuleHandleW(nullptr));
    tray.RestoreWindow();
    passed &= Report("tray controller owns window restore behavior",
        hwnd && IsWindowVisible(hwnd));
    ShowWindow(hwnd, SW_HIDE);
    tray.Detach();
    if (hwnd) DestroyWindow(hwnd);

    pulse::app::AppPrefs prefs;
    passed &= Report("pinned tab names are shown by default", prefs.show_pinned_tab_names);
    pulse::app::ContextMenuPrefs context;
    prefs.persist = false;
    context.persist = false;
    SettingsController settings;
    settings.SelectPage(2);
    settings.ScrollBy(-120.0f, 1.0f, 100.0f);
    passed &= Report("settings controller owns page and clamped scroll state",
        settings.page() == 2 && settings.scroll() == 72.0f);
    passed &= Report("settings controller maps stable page routes",
        SettingsController::PageFromName(L"index") == 1 &&
        std::wstring(SettingsController::PageName(2)) == L"context" &&
        SettingsController::PageFromName(L"about") == 3 &&
        std::wstring(SettingsController::PageName(3)) == L"about" &&
        SettingsController::PageFromName(L"duplicates") == 4 &&
        std::wstring(SettingsController::PageName(4)) == L"duplicates");

    pulse::app::SettingsTaskResult completed_task;
    completed_task.task.kind = pulse::app::SettingsTaskKind::InstallService;
    completed_task.ok = true;
    auto task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion owns service refresh state",
        task_effect.refresh_index && settings.service_installed());
    completed_task.task.kind = pulse::app::SettingsTaskKind::NetworkAdd;
    completed_task.task.path = L"\\\\server\\share";
    completed_task.task.pin = true;
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion returns network pin side effect",
        !task_effect.refresh_index && task_effect.pin_network == completed_task.task.path);
    completed_task.task.kind = pulse::app::SettingsTaskKind::DiagnosticsExport;
    completed_task.task.path = L"C:\\diagnostics-export";
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("diagnostics completion opens export without index refresh",
        !task_effect.refresh_index && task_effect.open_path == completed_task.task.path);
    completed_task.task.kind = pulse::app::SettingsTaskKind::NetworkRemove;
    completed_task.ok = false;
    task_effect = settings.CompleteTask(completed_task, true);
    passed &= Report("settings completion owns operation error text",
        !task_effect.refresh_index && settings.error() == L"无法移除服务器文件夹。");

    pulse::index::IndexClient index_client;
    pulse::index::NetworkAgentClient network_client;
    SettingsController& settings_ui = settings;
    int applied_effects = 0;
    SettingsEffect last_effect = SettingsEffect::None;
    bool picked_image = false;
    SettingsController::UiCallbacks ui_callbacks;
    ui_callbacks.pick_image = [&](std::wstring& selected) {
        picked_image = true;
        selected = L"C:\\wallpaper.png";
        return true;
    };
    ui_callbacks.apply_effects = [&](SettingsEffect effect) {
        last_effect = effect;
        if (effect != SettingsEffect::None) ++applied_effects;
    };
    settings_ui.BindUi(prefs, context, index_client, network_client,
                       std::move(ui_callbacks));
    settings_ui.WindowEffect(L"mica");
    passed &= Report("settings window effect returns material invalidation",
        prefs.window_effect == L"mica" &&
        HasEffect(last_effect, SettingsEffect::WindowMaterial));
    settings_ui.AccentChoice(false, 0x12AB34);
    passed &= Report("settings accent is normalized and reports its side effect",
        prefs.accent_rgb == L"12AB34" && HasEffect(last_effect, SettingsEffect::Accent));
    settings_ui.RowHeight(2);
    passed &= Report("settings density owns preference mutation",
        prefs.row_height == 40 && HasEffect(last_effect, SettingsEffect::RowHeight));
    settings_ui.TrayIconSize(0);
    passed &= Report("settings tray size owns preference mutation",
        prefs.tray_icon_size == 40 && HasEffect(last_effect, SettingsEffect::TrayDeckIcon));
    settings_ui.Language(L"en-US");
    passed &= Report("settings language uses a stable identifier",
        prefs.language == L"en-US" && HasEffect(last_effect, SettingsEffect::Language));
    settings_ui.ToggleUi(2);
    passed &= Report("settings close behavior requests tray synchronization",
        prefs.keep_running_on_close && HasEffect(last_effect, SettingsEffect::TrayVisibility));
    settings_ui.ToggleUi(4);
    passed &= Report("settings status performance toggle persists",
        prefs.show_status_performance &&
        HasEffect(last_effect, SettingsEffect::StatusBarPerformance));
    settings_ui.WindowEffect(L"mica-alt");
    settings_ui.RowHeight(0);
    settings_ui.AccentChoice(false, 0x2468AC);
    passed &= Report("settings UI controller routes preference commands",
        prefs.window_effect == L"mica-alt" && prefs.row_height == 28 &&
        prefs.accent_rgb == L"2468AC" && applied_effects == 10);
    settings_ui.ToggleUi(5);
    passed &= Report("settings hidden visibility toggle persists and refreshes panes",
        prefs.show_hidden_files && HasEffect(last_effect, SettingsEffect::FileVisibility));
    settings_ui.ToggleUi(5);
    passed &= Report("settings hidden visibility toggle is reversible", !prefs.show_hidden_files);
    pulse::app::AppPrefs protected_prefs;
    settings_ui.ToggleUi(16);
    passed &= Report("settings protected system files toggle persists and refreshes panes",
        prefs.show_protected_os_files && HasEffect(last_effect, SettingsEffect::FileVisibility) &&
        protected_prefs.FromJson(prefs.ToJson()) && protected_prefs.show_protected_os_files);
    settings_ui.ToggleUi(16);
    passed &= Report("settings protected system files toggle is reversible",
        !prefs.show_protected_os_files);
    settings_ui.ToggleUi(6);
    pulse::app::AppPrefs parsed_prefs;
    passed &= Report("pinned tab names toggle off and persist",
        !prefs.show_pinned_tab_names && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.show_pinned_tab_names);
    settings_ui.ToggleUi(6);
    passed &= Report("pinned tab names toggle on and persist",
        prefs.show_pinned_tab_names && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.show_pinned_tab_names);
    settings_ui.Wallpaper(0);
    passed &= Report("blank click navigation defaults off", !prefs.blank_click_go_back);
    settings_ui.ToggleUi(7);
    passed &= Report("blank click navigation enables and persists",
        prefs.blank_click_go_back && parsed_prefs.FromJson(prefs.ToJson()) &&
        parsed_prefs.blank_click_go_back);
    settings_ui.ToggleUi(7);
    passed &= Report("blank click navigation disables and persists",
        !prefs.blank_click_go_back && parsed_prefs.FromJson(prefs.ToJson()) &&
        !parsed_prefs.blank_click_go_back);
    parsed_prefs.blank_click_go_back = true;
    passed &= Report("legacy preferences leave blank click navigation off",
        parsed_prefs.FromJson(L"{}") && !parsed_prefs.blank_click_go_back);
    parsed_prefs.blank_click_go_back = true;
    parsed_prefs.ResetToDefaults();
    passed &= Report("reset disables blank click navigation", !parsed_prefs.blank_click_go_back);
    passed &= Report("settings UI controller owns image selection flow",
        picked_image);
    settings_ui.ResetUi();
    passed &= Report("settings UI controller has explicit binding lifecycle",
        !settings_ui.ui_bound());

    passed &= pulse::bench::RunSessionLayoutTests();

    passed &= RunContextMenuComTests(context);

    std::mutex task_mutex;
    std::condition_variable task_cv;
    bool task_done = false;
    bool task_started = false;
    bool release_task = false;
    pulse::app::SettingsTaskResult task_result;
    pulse::app::SettingsTask volume_task;
    volume_task.kind = pulse::app::SettingsTaskKind::Volume;
    volume_task.key = L"volume-async";
    volume_task.enabled = true;
    passed &= Report("settings async task admits one volume request",
        pulse::app::SettingsControllerTestPeer::Start(settings, volume_task,
            [&](const pulse::app::SettingsTask&, std::wstring&) {
                std::unique_lock<std::mutex> lock(task_mutex);
                task_started = true;
                task_cv.notify_one();
                task_cv.wait(lock, [&] { return release_task; });
                return true;
            }, [&](pulse::app::SettingsTaskResult result) {
                std::lock_guard<std::mutex> lock(task_mutex);
                task_result = std::move(result);
                task_done = true;
                task_cv.notify_one();
            }));
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        task_cv.wait_for(lock, std::chrono::seconds(2), [&] { return task_started; });
    }
    passed &= Report("settings async task coalesces duplicate volume request",
        !pulse::app::SettingsControllerTestPeer::Start(settings, volume_task,
            [](const pulse::app::SettingsTask&, std::wstring&) { return true; },
            [](pulse::app::SettingsTaskResult) {}));
    pulse::app::SettingsTask exclude_task;
    exclude_task.kind = pulse::app::SettingsTaskKind::Exclude;
    exclude_task.path = L"C:\\excluded";
    passed &= Report("settings serializes all local index configuration",
        !pulse::app::SettingsControllerTestPeer::Start(settings, std::move(exclude_task),
            [](const pulse::app::SettingsTask&, std::wstring&) { return true; },
            [](pulse::app::SettingsTaskResult) {}));
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        release_task = true;
        task_cv.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        const bool completed = task_cv.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return task_done; });
        passed &= Report("settings async task invokes completion",
                         completed && task_result.ok && task_result.task.key == L"volume-async");
    }
    passed &= Report("settings async task releases volume pending state",
        !settings.VolumePending(L"volume-async"));

    task_done = false;
    pulse::app::SettingsTask failed_task;
    failed_task.kind = pulse::app::SettingsTaskKind::NetworkAdd;
    failed_task.path = L"\\\\server\\missing";
    passed &= Report("settings async task propagates operation failure",
        pulse::app::SettingsControllerTestPeer::Start(settings, std::move(failed_task),
            [](const pulse::app::SettingsTask&, std::wstring& error) {
                error = L"offline";
                return false;
            }, [&](pulse::app::SettingsTaskResult result) {
                std::lock_guard<std::mutex> lock(task_mutex);
                task_result = std::move(result);
                task_done = true;
                task_cv.notify_one();
            }));
    {
        std::unique_lock<std::mutex> lock(task_mutex);
        const bool completed = task_cv.wait_for(lock, std::chrono::seconds(2),
                                                [&] { return task_done; });
        passed &= Report("settings async task preserves failure detail",
                         completed && !task_result.ok && task_result.error == L"offline");
    }
    passed &= Report("settings async task releases network pending state",
        !settings.network_pending());

    {
        SettingsController migration_ui;
        pulse::app::SettingsTask task;
        task.kind = pulse::app::SettingsTaskKind::ConfigureIndexPath;
        task.path = L"D:\\Index";
        bool pending_seen = false;
        pulse::app::SettingsTaskResult result;
        pulse::app::SettingsControllerTestPeer::Start(migration_ui, task,
            [&](const pulse::app::SettingsTask&, std::wstring& error) {
                pending_seen = migration_ui.migration_pending();
                error = L"cleanup warning";
                return false;
            }, [&](pulse::app::SettingsTaskResult completed) { result = std::move(completed); });
        migration_ui.Stop();
        passed &= Report("migration displays pending state and clears it on failure",
            pending_seen && !migration_ui.migration_pending());
        const auto effect = migration_ui.CompleteTask(result, true);
        passed &= Report("migration failure preserves message and refreshes actual location",
            effect.refresh_index && migration_ui.error() == L"cleanup warning");
    }

    bool lifecycle_completed = false;
    {
        SettingsController lifecycle;
        pulse::app::SettingsTask task;
        task.kind = pulse::app::SettingsTaskKind::NetworkRebuild;
        pulse::app::SettingsControllerTestPeer::Start(lifecycle, std::move(task),
            [](const pulse::app::SettingsTask&, std::wstring&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return true;
            }, [&](pulse::app::SettingsTaskResult) { lifecycle_completed = true; });
    }
    passed &= Report("settings controller joins tasks during destruction",
        lifecycle_completed);

    std::printf("\n== app controller tests: %s ==\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
