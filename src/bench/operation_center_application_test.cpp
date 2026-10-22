#include "../app/app_internal.h"
#include "../ui/toolbar_layout.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../app/context_menu.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "../app/app_exit.h"

int RunOperationCenterApplicationTest() {
    using namespace pulse;
    int failures = 0, checks = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks;
        failures += !passed;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    };
    const auto output = std::filesystem::absolute(L"bench_data/operation-center-application");
    std::filesystem::create_directories(output);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = s.searchHistory.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Operation center application fixture",
        WS_OVERLAPPEDWINDOW, -20000, -20000, 1280, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetDetailsPanelVisible(true);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = L"C:\\fixture";
    InitializeOperationCenter(s);
    check(s.operationCenter != nullptr, "production operation center initializes with owner compositor");
    check(!l10n::Get(l10n::StringId::OperationCenter).empty() &&
        !l10n::Get(l10n::StringId::OperationCenterQueued).empty() &&
        !l10n::Get(l10n::StringId::OperationCenterDestination).empty(),
        "operation center title states and actions load from production resources");
    ops::OpRequest request;
    request.sources = {L"C:\\fixture\\document.pdf"};
    request.dest_dir = L"C:\\fixture\\destination";
    const auto task = s.ops.Submit(request);
    UpdateOperationCenter(s);
    auto vm = BuildVm(s, false);
    check(task && vm.operation_count == 1, "queued operation reaches production toolbar badge");
    const auto wallpaper = std::filesystem::absolute(L"assets/global-search-logo.png").wstring();
    for (const float scale : {1.0f, 1.5f}) for (const int width : {540, 960, 1440}) {
        s.scale = scale;
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        s.compositor.Resize(static_cast<UINT>(width*scale), static_cast<UINT>(760*scale));
        const auto rect = D2D1::RectF(0, 0, width*scale, 760*scale);
        const float left = s.renderer.EffectiveSidebarWidth(rect.right);
        const auto layout = ui::MakeToolbarLayout(rect.right, scale, s.renderer.TitleBarHeight(),
            s.renderer.Margin(), width*scale-left < 600*scale ? 32*scale : 84*scale, left);
        vm = BuildVm(s, false);
        check(layout.search.right+7*scale <= layout.operations.left &&
            layout.address.right+7*scale <= layout.search.left,
            "path search and operation entry have disjoint bounds");
        const auto hit = s.renderer.HitTest(vm, rect, (layout.operations.left+layout.operations.right)/2,
            (layout.operations.top+layout.operations.bottom)/2);
        check(hit.region == ui::HitTestResult::OperationCenter,
            "production hit testing reaches operation center at narrow width and DPI");
        vm.address_searching = true;
        const auto overlay = s.renderer.SearchBarRect(rect.right);
        check(overlay.right+7*scale <= layout.operations.left,
            "expanded search leaves operation entry available");
        if (width*scale-left >= 700*scale)
            check(layout.filter.right-layout.filter.left == 76*scale,
                "filter width omits unused dropdown arrow space");
        vm.address_searching = false;
        vm.details_visible = true;
        vm.details.has_selection = true;
        vm.details.is_dir = true;
        vm.details.preview_enabled = false;
        vm.details.name = L"项目资料";
        vm.details.path = L"C:\\fixture\\项目资料";
        vm.details.type_text = L"文件夹";
        vm.details.location_text = L"C:\\fixture";
        vm.details.size_text = L"3.8 GB";
        vm.hover_region = static_cast<int>(ui::HitTestResult::FilterBox);
        vm.background_image = wallpaper;
        vm.wallpaper_visibility = 80;
        vm.wallpaper_blur = 0;
        vm.window_effect = ui::WindowEffect::None;
        for (const bool dark : {false, true}) {
            vm.dark = dark;
            for (int frame = 0; frame < 12; ++frame) {
                s.compositor.Dc()->BeginDraw();
                s.renderer.Render(vm, rect, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                const auto rendered = s.compositor.Dc()->EndDraw();
                if (frame == 11) check(SUCCEEDED(rendered), "production toolbar and details render over wallpaper");
                if (frame < 11) Sleep(15);
            }
            const auto name = std::to_wstring(width)+L"-"+std::to_wstring(static_cast<int>(100*scale))+
                (dark ? L"-dark.png" : L"-light.png");
            check(s.compositor.SaveSnapshot((output/name).c_str()), "save production toolbar and details visual evidence");
        }
    }
    s.scale = 1;
    s.compositor.RecreateTextFormats(compat::WindowDpi(s.hwnd)/96.0f);
    s.operationCenter->Hide(false);
    for (const auto* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        UpdateOperationCenterTheme(s);
        UpdateOperationCenter(s);
        check(!l10n::Get(l10n::StringId::OperationCenter).empty() &&
            !l10n::Get(l10n::StringId::OperationCenterQueued).empty() &&
            !l10n::Get(l10n::StringId::OperationCenterDestination).empty() &&
            !l10n::Get(l10n::StringId::ReturnUnfinishedToTray).empty() &&
            !l10n::Get(l10n::StringId::ResumeIncompleteTransfer).empty() &&
            !l10n::Get(l10n::StringId::BatchConflictInstruction).empty() &&
            !l10n::Get(l10n::StringId::GroupBy).empty() &&
            !l10n::Get(l10n::StringId::ItemFilterSyntax).empty(),
            "operation center and file-management resources are available in both UI languages");
        check(s.operationCenter->SaveDebugSnapshot((output/(std::wstring(L"localized-queued-")+language+L".png")).c_str(), 472, 300),
            "production localized queued task renders in the operation center");
    }
    l10n::SetLanguage(L"zh-CN");
    auto* tab = ActiveTab(s);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 8; ++i) {
        fs::DirEntry entry;
        entry.name = i % 2 ? L"report-" + std::to_wstring(i) + L".pdf"
            : L"photo-" + std::to_wstring(i) + L".png";
        entry.size = static_cast<uint64_t>(i + 1) * 1024 * 1024;
        entry.full_path = tab->current_path + L"\\" + entry.name;
        entries->push_back(entry);
    }
    tab->snapshot = entries;
    tab->view_mode = ui::ViewMode::Details;
    tab->sort_column = ui::SortColumn::Name;
    tab->selected_index = 3;
    tab->selected = {3};
    s.places.tags = {{L"design", L"设计", 0xF59E0B, {}}, {L"ready", L"已审核", 0x22C55E, {}}};
    for (const auto& entry : *entries) s.places.tags[0].paths.push_back(entry.full_path);
    s.places.tags[1].paths.push_back((*entries)[3].full_path);
    s.places.TagsReordered();
    DispatchMenuCommand(s, app::CmdGroupType);
    vm = BuildVm(s, false);
    check(tab->group_mode == ui::GroupMode::Type && vm.pane.date_groups && vm.pane.date_groups->size() == 2,
        "grouping command reaches production view with independent filename sorting");
    check(tab->sort_column == ui::SortColumn::Name && vm.pane.SourceIndex(vm.pane.ViewIndex(3)) == 3,
        "grouping preserves sort choice and selected source identity");
    const auto remembered = s.appPrefs.folder_views.FindGroup(tab->current_path);
    check(remembered && remembered->mode == ui::GroupMode::Type,
        "production grouping command remembers per-directory mode");
    tab->filter_text = L"tag:设计 tag:已审核 ext:pdf size:>1MB";
    vm = BuildVm(s, false);
    check(vm.pane.EntryCount() == 1 && vm.pane.SourceIndex(0) == 3,
        "production tag AND metadata filtering composes with grouping");
    tab->filter_text = L"tag:设计 -tag:已审核 ext:pdf";
    vm = BuildVm(s, false);
    check(vm.pane.EntryCount() == 3, "negative tag condition excludes reviewed files");
    tab->filter_text = L"size:broken";
    vm = BuildVm(s, false);
    check(vm.pane.EntryCount() == 0 && !vm.pane.filter_error.empty(),
        "invalid filter produces an explained empty state");
    tab->filter_text.clear();
    tab->net_readonly = true;
    vm = BuildVm(s, false);
    check(vm.toolbar_commands_enabled && !(*vm.toolbar_commands_enabled)[0] &&
        (*vm.toolbar_commands_enabled)[1] && !(*vm.toolbar_commands_enabled)[4],
        "production toolbar shares catalog readonly rules while allowing copy");
    tab->net_readonly = false;
    tab->details_column_dividers = {300, 180, 140};
    RememberFolderColumns(s, *tab);
    check(s.appPrefs.folder_views.FindColumns(tab->current_path) == tab->details_column_dividers,
        "manual widths are remembered for the active directory");
    s.renderer.SetScale(1);
    s.compositor.RecreateTextFormats(1);
    s.compositor.Resize(1440, 760);
    for (const bool dark : {false, true}) {
        s.darkMode = dark;
        vm = BuildVm(s, false);
        vm.dark = dark;
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm, D2D1::RectF(0, 0, 1440, 760), ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
        check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "production grouped rows render in both themes");
        check(s.compositor.SaveSnapshot((output/(dark ? L"grouped-dark.png" : L"grouped-light.png")).c_str()),
            "save production grouping visual evidence");
    }
    s.tray.Collect({L"C:\\fixture\\ready.txt", L"C:\\fixture\\missing.txt"}, false);
    const auto queries = s.tray.BeginMetadataQueries(8);
    std::vector<app::TrayMetadataResult> facts;
    for (const auto& query : queries) facts.push_back({query,
        query.path.ends_with(L"missing.txt") ? app::TrayMetadataState::Missing : app::TrayMetadataState::Ready,
        FILE_ATTRIBUTE_NORMAL, 16});
    s.tray.ApplyMetadataResults(facts);
    const auto before = s.trayTransfers.size();
    ReleaseTrayBatch(s, 0);
    check(s.trayTransfers.size() == before + 1 && s.tray.batches().size() == 1 &&
        s.tray.batches()[0].items.size() == 1 && s.tray.batches()[0].items[0].path.ends_with(L"missing.txt"),
        "release tracks submitted task and keeps unsubmitted missing items in tray");
    s.operationCenter.reset();
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    std::printf("Operation center application: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

namespace pulse::ui {
struct OperationCenterTestPeer {
    static const auto& Tasks(const OperationCenter& center) { return center.tasks_; }
    static void Pause(OperationCenter& center, uint64_t task) { center.callbacks_.pause(task); }
    static void Resume(OperationCenter& center, uint64_t task) { center.callbacks_.resume(task); }
    static void Cancel(OperationCenter& center, uint64_t task) { center.callbacks_.cancel(task); }
};
}

int RunOperationCenterMultiwindowTest() {
    using namespace pulse;
    int failures = 0, checks = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks; failures += !passed;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    };
    auto viewer = std::make_unique<AppState>();
    auto source = std::make_unique<AppState>(viewer->core);
    for (auto* state : {viewer.get(), source.get()}) {
        state->isolatedTest = true;
        state->appPrefs.persist = state->searchHistory.persist = state->places.persist = false;
        state->appPrefs.keep_running_on_close = false;
        state->appPrefs.global_search_enabled = false;
        state->hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Operation routing fixture",
            WS_OVERLAPPEDWINDOW, -20000, -20000, 1280, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!state->hwnd) return 2;
        SetWindowLongPtrW(state->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->core.windows.Add(state->hwnd, state);
        state->window_tabs.EnsureDefault();
        state->pane = state->window_tabs.Active()->FocusedPane();
        ShowWindow(state->hwnd, SW_SHOWNOACTIVATE);
    }
    if (!viewer->compositor.Init(viewer->hwnd)) return 2;
    viewer->compositor.RecreateTextFormats(compat::WindowDpi(viewer->hwnd) / 96.0f);
    check(viewer->compositor.TextFormat() && viewer->compositor.SmallFormat(),
        "fixture initializes production text formats before rendering task cards");
    viewer->renderer.SetCompositor(&viewer->compositor);
    InitializeOperationCenter(*viewer);
    if (!viewer->operationCenter) return 2;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"operation-routing-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"destination");
    const auto file = root / L"large.bin";
    { std::ofstream stream(file, std::ios::binary); const std::string block(1024 * 1024, 'x');
      for (int i = 0; i < 32; ++i) stream.write(block.data(), block.size()); }
    { std::ofstream stream(root / L"destination" / L"large.bin"); stream << "existing destination"; }
    ops::OpRequest request; request.type = ops::OpType::Copy;
    request.sources = {file.wstring()}; request.dest_dir = (root / L"destination").wstring();
    const auto local_task = viewer->ops.Submit(request);
    source->ops.SetUiWindow(source->hwnd);
    source->ops.Start([hwnd = source->hwnd] { PostMessageW(hwnd, WM_OPS_NOTIFY, 0, 0); });
    const auto foreign_task = source->ops.Submit(request);
    const auto wait = [&](const auto& predicate) {
        const auto deadline = GetTickCount64() + 15000;
        do {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_OPS_NOTIFY) SyncOperationCenters(*source);
                else { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if (predicate()) return true;
            Sleep(5);
        } while (GetTickCount64() < deadline);
        return false;
    };
    check(local_task == foreign_task && local_task != 0, "fixture reproduces duplicate per-window task IDs");
    check(wait([&] { return source->ops.PendingConflict().has_value(); }), "real 32 MiB copy waits at destination conflict");
    RequestAppWindowClose(*source);
    check(!IsWindowVisible(source->hwnd) && IsWindow(source->hwnd) && source->ops.HasPendingFileOperations(),
        "closing the copy window keeps its task running in a hidden registered window");
    SyncOperationCenters(*source);
    auto tasks = CollectOperationCenterTasks(*viewer);
    check(tasks.size() == 2 && tasks[0].task_id != tasks[1].task_id,
        "operation center aggregates hidden and visible owners with distinct display IDs");
    uint64_t display_id = 0;
    for (const auto& task : tasks) {
        auto id = task.task_id;
        if (ResolveOperationCenterTask(*viewer, id) == source.get()) display_id = task.task_id;
    }
    const auto stable = CollectOperationCenterTasks(*viewer);
    auto routed = display_id;
    check(display_id && ResolveOperationCenterTask(*viewer, routed) == source.get() && routed == foreign_task &&
        std::any_of(stable.begin(), stable.end(), [&](const auto& task) { return task.task_id == display_id; }),
        "display ID remains stable and resolves to the hidden task's actual owner and ID");
    check(BuildVm(*viewer, false).operation_count == 2 &&
        ui::OperationCenterTestPeer::Tasks(*viewer->operationCenter).size() == 2,
        "visible toolbar badge and operation cards include the background copy");
    ui::OperationCenterTestPeer::Pause(*viewer->operationCenter, display_id);
    if (const auto conflict = source->ops.PendingConflict())
        source->ops.ResolveConflict(conflict->token, ops::ConflictChoice::Replace, true);
    check(wait([&] { return source->ops.Status().phase == ops::OpPhase::Paused; }),
        "pause from visible operation center reaches hidden copy");
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    SyncOperationCenters(*source);
    check(viewer->operationCenter->SaveDebugSnapshot(
        std::filesystem::absolute(L"bench_data/operation-center-multiwindow.png").c_str(), 472, 600),
        "render visible operation cards for hidden paused copy and local queued task");
    ui::OperationCenterTestPeer::Resume(*viewer->operationCenter, display_id);
    ui::OperationCenterTestPeer::Cancel(*viewer->operationCenter, display_id);
    check(wait([&] { return !source->ops.HasPendingFileOperations(); }) &&
        source->ops.Status().phase == ops::OpPhase::Cancelled && viewer->ops.HasPendingFileOperations(),
        "cancel targets hidden copy without touching same-numbered local task");
    check(wait([&] { const auto& cards = ui::OperationCenterTestPeer::Tasks(*viewer->operationCenter);
        return std::any_of(cards.begin(), cards.end(), [&](const auto& task) {
            return task.task_id == display_id && task.phase == ops::OpPhase::Cancelled; }); }),
        "background task's terminal state is synchronized into visible cards");
    const auto completed_before = source->ops.Status().completed_ops;
    const auto complete_dir = root / L"completed";
    std::filesystem::create_directories(complete_dir);
    request.dest_dir = complete_dir.wstring();
    source->ops.Submit(request);
    check(wait([&] { return source->ops.Status().completed_ops > completed_before &&
        !source->ops.HasPendingFileOperations(); }) && source->ops.Status().phase == ops::OpPhase::Completed &&
        std::filesystem::exists(complete_dir / L"large.bin") &&
        std::filesystem::file_size(complete_dir / L"large.bin") == std::filesystem::file_size(file),
        "hidden window completes a real 32 MiB copy without interrupting the visible window");
    check(wait([&] { const auto& cards = ui::OperationCenterTestPeer::Tasks(*viewer->operationCenter);
        return std::any_of(cards.begin(), cards.end(), [](const auto& task) {
            return task.phase == ops::OpPhase::Completed; }); }),
        "completed background copy is synchronized to visible recent history");
    check(BuildVm(*viewer, false).operation_count == 1,
        "completed background tasks no longer inflate the visible active-task badge");
    source->core.windows.Remove(source->hwnd);
    routed = display_id;
    check(ResolveOperationCenterTask(*viewer, routed) == nullptr, "stale callback cannot access an unregistered task owner");
    SyncOperationCenters(*viewer);
    check(ui::OperationCenterTestPeer::Tasks(*viewer->operationCenter).size() == 1,
        "removed owner no longer contributes operation cards");
    source->ops.Stop(); viewer->ops.Stop();
    viewer->operationCenter.reset(); viewer->renderer.SetCompositor(nullptr); viewer->compositor.Shutdown();
    for (auto* state : {source.get(), viewer.get()}) {
        state->core.windows.Remove(state->hwnd);
        SetWindowLongPtrW(state->hwnd, GWLP_USERDATA, 0); DestroyWindow(state->hwnd); state->hwnd = nullptr;
    }
    // Cancelled transfer recovery plans retain directory guards until their owner is released.
    source.reset();
    viewer.reset();
    bool cleaned = false;
    DWORD cleanup_error = ERROR_SUCCESS;
    const auto cleanup_deadline = GetTickCount64() + 3000;
    do {
        // Delete-pending files can briefly retain their parent directory.
        // Retry only this fixture's exact paths, never recursively remove a directory.
        DeleteFileW((complete_dir / L"large.bin").c_str()); RemoveDirectoryW(complete_dir.c_str());
        DeleteFileW(file.c_str()); DeleteFileW((root / L"destination" / L"large.bin").c_str());
        RemoveDirectoryW((root / L"destination").c_str());
        if (RemoveDirectoryW(root.c_str())) { cleaned = true; break; }
        cleanup_error = GetLastError();
        Sleep(10);
    } while (GetTickCount64() < cleanup_deadline);
    if (!cleaned) std::printf("[INFO] fixture cleanup error=%lu\n", cleanup_error);
    check(cleaned, "isolated copy fixture cleaned");
    std::printf("Operation center multiwindow: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
