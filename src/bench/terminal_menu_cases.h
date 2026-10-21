#pragma once
#include "../app/app_internal.h"
#include <cstdio>
#include <thread>

namespace pulse::ops {
struct TerminalMenuTestProbe {
    static void Capture(OpsManager& ops) {
        ops.accepting_ = true;
        ops.open_mailbox_ = std::make_shared<OpsManager::OpenMailbox>();
    }
    static bool TakeTerminal(OpsManager& ops, const std::wstring& directory) {
        auto& queue = ops.open_mailbox_->queue;
        if (queue.size() != 1) { queue.clear(); return false; }
        const auto item = std::move(queue.front());
        queue.clear();
        return item.open_file == L"wt.exe" && item.open_path == directory &&
            item.open_args == TerminalCommandLine(directory);
    }
    static bool Empty(const OpsManager& ops) { return ops.open_mailbox_->queue.empty(); }
    static void Reset(OpsManager& ops) {
        ops.accepting_ = false;
        ops.open_mailbox_.reset();
    }
};
}

inline int RunTerminalMenuChecks() {
    using namespace pulse;
    using Probe = ops::TerminalMenuTestProbe;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    auto owned = std::make_unique<AppState>();
    auto& state = *owned;
    state.isolatedTest = true;
    state.appPrefs.persist = state.places.persist = false;
    state.window_tabs.EnsureDefault();
    state.pane = state.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(state);
    tab.current_path = L"pulse:search:plain cr";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(2);
    (*entries)[0].name = L"Plain Craft Launcher 2.exe";
    (*entries)[0].full_path = LR"(E:\Games\Minecraft\The Legend Of 2900\PCL\Plain Craft Launcher 2.exe)";
    (*entries)[1].name = L"folder";
    (*entries)[1].full_path = LR"(D:\other folder\folder)";
    (*entries)[1].is_dir = true;
    tab.SetSnapshot(entries);
    tab.SelectOnly(0);
    Probe::Capture(state.ops);
    const auto dispatch = [&](const std::wstring& expected) {
        DispatchMenuCommand(state, app::CmdOpenTerminal);
        return Probe::TakeTerminal(state.ops, expected);
    };
    const auto blocked = [&] {
        DispatchMenuCommand(state, app::CmdOpenTerminal);
        return Probe::Empty(state.ops);
    };
    check(dispatch(LR"(E:\Games\Minecraft\The Legend Of 2900\PCL)"),
        "search EXE queues terminal with its real parent and matching -d argument");
    tab.SelectOnly(1);
    check(dispatch(LR"(D:\other folder\folder)"), "search folder opens inside the selected folder");
    tab.SelectRange(0, 1);
    check(dispatch(LR"(D:\other folder\folder)"),
        "multiple search results use the focused selected result");
    tab.SelectOnly(0);
    (*entries)[0].full_path = LR"(\\?\UNC\server\share\中文 目录\file.txt)";
    check(dispatch(LR"(\\server\share\中文 目录)"), "UNC search hit preserves its containing directory");
    (*entries)[0].full_path = L"C:\\file.txt";
    check(dispatch(L"C:\\"), "file at drive root preserves the root slash");
    (*entries)[0].full_path.clear();
    check(blocked(), "unresolved search row never launches a virtual path");
    tab.ClearSelection();
    check(blocked(), "empty search selection never launches a terminal");
    check(app::TerminalMenuPath(tab, true).empty(), "search background has no filesystem directory");
    tab.current_path = L"pulse:saved-search:fixture";
    (*entries)[0].full_path = L"D:\\saved\\hit.txt";
    tab.SelectOnly(0);
    check(dispatch(L"D:\\saved"), "saved search uses the concrete hit directory");
    (*entries)[0].link_target = L"E:\\shortcut target\\folder";
    (*entries)[0].link_target_is_dir = true;
    check(dispatch(L"E:\\shortcut target\\folder"), "folder shortcut uses the same resolved target as details panel");
    (*entries)[0].link_target = L"E:\\shortcut target\\file.txt";
    (*entries)[0].link_target_is_dir = false;
    check(dispatch(L"E:\\shortcut target"), "file shortcut uses the resolved file parent");
    (*entries)[0].link_target = L"pulse:search:virtual shortcut";
    check(blocked(), "virtual shortcut target cannot launch terminal");
    (*entries)[0].link_target.clear();
    (*entries)[0].change_record_only = true;
    check(blocked(), "historical deleted result cannot launch terminal");
    (*entries)[0].change_record_only = false;
    (*entries)[0].recycle_path = L"C:\\$Recycle.Bin\\payload";
    check(blocked(), "recycled result cannot launch terminal");
    (*entries)[0].recycle_path.clear();
    tab.current_path = L"pulse:archive:fixture.zip|";
    check(blocked() && app::TerminalMenuPath(tab, true).empty(), "archive paths cannot become terminal directories");
    tab.current_path = L"C:\\ordinary";
    (*entries)[0].full_path.clear();
    check(dispatch(L"C:\\ordinary"), "ordinary file keeps its containing directory");
    tab.SelectOnly(1);
    (*entries)[1].full_path.clear();
    check(dispatch(L"C:\\ordinary\\folder"), "ordinary folder uses the selected folder");
    check(app::TerminalMenuPath(tab, true) == L"C:\\ordinary",
        "background directory ignores an existing folder selection");
    tab.ClearSelection();
    check(dispatch(L"C:\\ordinary"), "command without selection uses the current filesystem folder");
    tab.current_path.clear();
    check(blocked(), "This PC without selection has no terminal directory");
    state.ops.OpenTerminal(L"");
    state.ops.OpenTerminal(L"pulse:search:plain cr");
    check(Probe::Empty(state.ops), "launch boundary rejects empty and virtual directories");
    check(ops::TerminalCommandLine(L"C:\\") == L"-d \"C:\\\\\"",
        "terminal drive-root argument doubles its trailing slash before the closing quote");
    check(ops::TerminalCommandLine(L"C:\\quoted\" folder\\") == L"-d \"C:\\quoted\\\" folder\\\\\"",
        "terminal arguments preserve embedded quotes and trailing slashes");
    tab.current_path = L"pulse:search:content fixture";
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    auto store = std::make_shared<index::ContentResultStore>(nullptr, 0);
    std::vector<index::ContentHit> content_hits{
        {L"C:\\content\\first.txt", L"first.txt", L"fixture"},
        {L"D:\\content target\\focused.txt", L"focused.txt", L"fixture"}
    };
    bool appended = false;
    std::thread writer([&] { appended = store->Append(content_hits); });
    writer.join();
    check(appended, "isolated disk-backed content search fixture accepts results");
    tab.content_results = store;
    tab.SelectRange(0, 1);
    DispatchMenuCommand(state, app::CmdOpenTerminal);
    check(Probe::Empty(state.ops) && state.contentSelectionAction != nullptr,
        "content search defers terminal dispatch until focused metadata resolves");
    const auto started = GetTickCount64();
    while (state.contentSelectionAction && GetTickCount64() - started < 5000) {
        CompleteContentSelection(state);
        if (state.contentSelectionAction) Sleep(1);
    }
    check(!state.contentSelectionAction && Probe::TakeTerminal(state.ops, L"D:\\content target"),
        "deferred content search command launches only the focused real directory");
    tab.content_results.reset();
    store.reset();
    Probe::Reset(state.ops);
    std::printf("terminal checks: failures=%d\n", failures);
    return failures ? 1 : 0;
}
