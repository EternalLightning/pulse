// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../app/content_results_ui.h"
#include "../common/localization.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>

int ShotModeMain(pulse::AppState& state, HWND hwnd);
LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
namespace pulse::ui {
struct FluentMenuTestPeer {
    static void Choose(FluentMenu& menu, int command) {
        menu.result_ = command;
        menu.Dismiss();
    }
    static void Filter(FluentMenu& menu, const std::wstring& query) {
        if (menu.filter_fn_) (void)menu.filter_fn_(query);
    }
};
}
namespace {
using namespace pulse;
int failures = 0;
AppState* modal_state = nullptr;
std::function<void()> modal_action;
ULONGLONG modal_deadline = 0;
bool modal_ran = false;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    failures += !passed;
}
std::unique_ptr<AppState> State() {
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.appPrefs.change_tracking_enabled = false;
    s.shot.active = true;
    s.shot_high_contrast = true;
    s.accentColor = ui::HexColor(0x0078D4);
    s.hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"App core lifecycle fixture",
        WS_POPUP, 0, 0, 960, 640, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return {};
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(s.scale);
    s.renderer.SetDetailsPanelVisible(false);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = L"pulse:changes:C:\\fixture";
    tab.loading = false;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
    return owned;
}
void Dispose(std::unique_ptr<AppState>& owned) {
    if (!owned) return;
    owned->menu.reset();
    owned->renderer.SetCompositor(nullptr);
    owned->compositor.Shutdown();
    if (IsWindow(owned->hwnd)) DestroyWindow(owned->hwnd);
    owned->hwnd = nullptr;
    owned.reset();
}
void CALLBACK DriveModal(HWND, UINT, UINT_PTR timer, DWORD) {
    if (!modal_state || !modal_state->menu || !modal_state->menu->IsOpen()) {
        if (GetTickCount64() < modal_deadline) return;
        KillTimer(nullptr, timer);
        Check(false, "isolated popup opens before its five-second deadline");
        if (modal_state && modal_state->menu) modal_state->menu->Dismiss();
        return;
    }
    KillTimer(nullptr, timer);
    modal_ran = true;
    modal_action();
}
void PopupAction(AppState& s, std::function<void()> action, const std::function<void()>& open) {
    modal_state = &s;
    modal_action = std::move(action);
    modal_ran = false;
    modal_deadline = GetTickCount64() + 5000;
    const auto timer = SetTimer(nullptr, 0, 40, DriveModal);
    Check(timer != 0, "arm isolated modal-loop callback");
    if (timer) open();
    KillTimer(nullptr, timer);
    Check(modal_ran, "production TrackPopup dispatches the lifecycle callback");
    modal_action = {};
    modal_state = nullptr;
}
bool StoreBarrier(const std::shared_ptr<index::ContentResultStore>& store) {
    auto signal = std::make_shared<std::promise<int>>();
    auto ready = signal->get_future();
    store->FindIdentity(11, [signal](int index) { signal->set_value(index); });
    return ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
}
void ContentChecks() {
    auto owned = State();
    Check(!!owned, "initialize content-selection fixture");
    if (!owned) return;
    auto& s = *owned;
    auto& tab = *ActiveTab(s);
    tab.current_path = L"pulse:search:content:needle";
    auto store = std::make_shared<index::ContentResultStore>(nullptr, 0);
    tab.content_results = store;
    index::ContentHit a{L"C:\\fixture\\a.txt", L"a.txt", L"needle", 1, 0, 1}; a.file_id = 11;
    index::ContentHit b{L"C:\\fixture\\b.md", L"b.md", L"needle", 2, 0, 1}; b.file_id = 12;
    const bool seeded = store->Append({a, b});
    Check(seeded, "seed real disk-spooled content results");
    if (!seeded) { Dispose(owned); return; }
    SelectContentPattern(s, L"*.txt");
    Check(StoreBarrier(store), "Match finishes while its UI completion remains unconsumed");
    CompleteContentSelection(s);
    Check(tab.SelectedCount() == 1 && tab.IsSelected(0), "current Match applies through the production UI path without deadlock");

    tab.ClearSelection();
    SelectContentPattern(s, L"*.txt");
    Check(StoreBarrier(store), "hold a completed Match before sorting");
    store->SetSort(index::ContentResultSort::Name, true);
    Check(StoreBarrier(store), "publish the reordered visible table before consuming Match");
    CompleteContentSelection(s);
    Check(!s.contentSelectionAction && tab.SelectedCount() == 0,
        "old Match row numbers cannot select the file now occupying that row after sorting");

    SelectContentPattern(s, L"*.txt");
    Check(StoreBarrier(store), "hold a completed Match before a live insert");
    index::ContentHit c{L"C:\\fixture\\z.md", L"z.md", L"needle", 3, 0, 1}; c.file_id = 13;
    const bool inserted = store->ApplyChanges({c}, index::ContentResultSort::Name, true);
    Check(inserted, "insert ahead of the completed Match row");
    if (!inserted) { Dispose(owned); return; }
    CompleteContentSelection(s);
    Check(!s.contentSelectionAction && tab.SelectedCount() == 0,
        "old Match row numbers cannot select a live-inserted file");
    Dispose(owned);
}
void ChangeMenuChecks() {
    for (int scenario = 0; scenario != 4; ++scenario) {
        auto owned = State();
        Check(!!owned, "initialize change-filter popup fixture");
        if (!owned) return;
        auto& s = *owned;
        auto* original = ActiveTab(s);
        const auto original_id = original->network_identity;
        original->change_days = 7;
        auto& other = s.window_tabs.NewTab(L"pulse:home");
        other.ActiveFolder()->loading = false;
        other.ActiveFolder()->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        s.window_tabs.SwitchTab(0);
        BindCurrentLayout(s);
        PopupAction(s, [&] {
            if (scenario == 0) {
                std::array<BYTE, 256> previous{}, keys{};
                Check(GetKeyboardState(previous.data()) != FALSE, "capture thread-local keyboard state");
                keys = previous;
                keys[VK_CONTROL] |= 0x80;
                keys[VK_SHIFT] = keys[VK_MENU] = 0;
                Check(SetKeyboardState(keys.data()) != FALSE, "set isolated Ctrl state without sending user input");
                HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, L'W', 0);
                SetKeyboardState(previous.data());
            } else if (scenario == 1) {
                s.pane->NewTab(L"pulse:changes:C:\\replacement");
                ActiveTab(s)->change_days = 9;
            } else if (scenario == 2) {
                original->current_path = L"pulse:changes:C:\\navigated";
                ++original->view_generation;
            } else {
                s.window_tabs.SwitchTab(1);
                BindCurrentLayout(s);
            }
            ui::FluentMenuTestPeer::Choose(*s.menu, 2);
        }, [&] {
            ui::HitTestResult hit;
            hit.region = ui::HitTestResult::ChangeTimeFilter;
            HandleChangeClick(s, hit, POINT{80, 80});
        });
        if (scenario == 0) {
            bool exists = false;
            ForEachPane(s, [&](app::Pane& pane) { exists |= pane.view.network_identity == original_id; });
            Check(!exists && s.window_tabs.items.size() == 1,
                "Ctrl+W closes the original tab inside the real popup loop and selection returns safely");
        } else if (scenario == 1) {
            Check(ActiveTab(s)->network_identity != original_id && ActiveTab(s)->change_days == 9,
                "same-address replacement Tab cannot receive the old menu choice");
        } else if (scenario == 2) {
            Check(original->change_days == 7, "navigation invalidates a pending change-filter choice");
        } else {
            Check(original->change_days == 3 && ActiveTab(s)->network_identity != original_id,
                "switching tabs preserves the original live tab as the filter target");
        }
        Dispose(owned);
    }
}
void BadgeChecks() {
    for (const bool remove : {false, true}) {
        auto owned = State();
        Check(!!owned, "initialize badge-editor popup fixture");
        if (!owned) return;
        auto& s = *owned;
        const std::wstring path = L"C:\\fixture\\desktop";
        app::SidebarEntry entry;
        entry.path = path;
        s.sidebar.quick_access.push_back(entry);
        const auto color = TagColorPalette(s)[1];
        PopupAction(s, [&] {
            s.sidebar = app::SidebarModel{};
            if (!remove) s.sidebar.quick_access.push_back(entry);
            ui::FluentMenuTestPeer::Filter(*s.menu, L"after rebuild");
            if (!remove) ui::FluentMenuTestPeer::Choose(*s.menu, app::CmdTabColorBase + 1);
        }, [&] {
            // Deliberately pass a reference owned by the model being replaced.
            ShowStarredBadgeEditor(s, s.sidebar.quick_access.front().path, POINT{80, 80});
        });
        const auto* saved = s.places.FindQuickAccessBadge(path);
        if (remove) {
            Check(!saved && !s.menu->IsOpen(), "removed badge target dismisses without writing a retired entry or catalog item");
        } else {
            Check(saved && saved->badge == L"after rebuil" && saved->badge_rgb == color &&
                s.sidebar.quick_access.front().badge == saved->badge && s.sidebar.quick_access.front().badge_rgb == color,
                "filter callback and returned color choice update the rebuilt sidebar entry");
        }
        Dispose(owned);
    }
}
void SidebarEscapeChecks() {
    for (int kind = 0; kind < 4; ++kind) {
        for (const bool active : {false, true}) {
            auto owned = State();
            Check(!!owned, "IP02 initialize isolated sidebar gesture fixture");
            if (!owned) return;
            auto& s = *owned;
            auto* tab = ActiveTab(s);
            tab->search_content_active = true;
            s.sidebarOrder = app::DefaultSidebarOrder();
            s.places.quick_access_paths = {L"C:\\fixture\\one", L"C:\\fixture\\two"};
            s.places.quick_access_order = s.places.quick_access_paths;
            s.places.starred_items = {
                {L"C:\\fixture\\one", model::PlaceItemKind::Folder},
                {L"C:\\fixture\\two", model::PlaceItemKind::Folder}};
            s.places.tags = {{L"one", L"one"}, {L"two", L"two"}};
            const auto original_order = s.sidebarOrder;
            const auto original_places = s.places.ToJson();
            const auto original_revision = s.places.TagRevision();
            if (kind == 0) {
                s.groupDragPending = !active;
                s.groupDragActive = active;
                s.groupDragId = s.sidebarOrder.front();
                s.groupDragToIndex = static_cast<int>(s.sidebarOrder.size()) - 1;
                s.groupGapVisible = true;
            } else if (kind == 1) {
                s.pinDragPending = !active;
                s.pinDragActive = active;
                s.pinDragPath = s.places.quick_access_paths.front();
                s.pinDragToIndex = 1;
                s.pinGapVisible = true;
            } else if (kind == 2) {
                s.starDragPending = !active;
                s.starDragActive = active;
                s.starDragPath = s.places.starred_items.front().path;
                s.starDragTarget = 1;
                s.dropSidebar = 0;
            } else {
                s.tagDragPending = !active;
                s.tagDragActive = active;
                s.tagDragTag = 0;
                s.tagDragPath = L"pulse:tag:one";
                s.tagOrder = {1, 0};
                s.tagGapVisible = true;
                s.tagOffsets[L"one"] = 0.5f;
            }
            SetCapture(s.hwnd);
            HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            Check(!s.groupDragPending && !s.groupDragActive &&
                !s.pinDragPending && !s.pinDragActive &&
                !s.starDragPending && !s.starDragActive &&
                !s.tagDragPending && !s.tagDragActive && GetCapture() != s.hwnd,
                "IP02 Escape clears each pending/active sidebar gesture and capture");
            HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, 0);
            Check(s.sidebarOrder == original_order && s.places.ToJson() == original_places &&
                s.places.TagRevision() == original_revision && tab->search_content_active,
                "IP02 release after Escape preserves order, saved state and unrelated content search");
            if (kind == 3) Check(s.tagOrder.empty() && s.tagOffsets.empty() && !s.tagGapVisible,
                "IP02 canceled tag reorder drops temporary order and animation state");
            Dispose(owned);
        }
    }
    auto owned = State();
    Check(!!owned, "IP02 initialize ordinary release fixture");
    if (!owned) return;
    auto& s = *owned;
    s.sidebarOrder = app::DefaultSidebarOrder();
    const int moved = s.sidebarOrder.front();
    s.groupDragActive = true;
    s.groupDragId = moved;
    s.groupDragToIndex = static_cast<int>(s.sidebarOrder.size()) - 1;
    HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, 0);
    Check(s.sidebarOrder.back() == moved && !s.groupDragActive,
        "IP02 ordinary release still commits a sidebar group reorder");
    Dispose(owned);
}

void RowClickChecks() {
    for (const bool ctrl : {false, true}) {
        for (const int count : {0, 3}) {
            auto owned = State();
            Check(!!owned, "IP04 initialize cross-tab row-click fixture");
            if (!owned) return;
            auto& s = *owned;
            auto& a = *ActiveTab(s);
            a.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(3));
            a.SelectOnly(0);
            a.ToggleSelect(1);
            HandleListRowClick(s, 1, ctrl, false);
            s.dragPending = true;
            auto& second = s.window_tabs.NewTab(L"pulse:changes:fixture-B");
            s.pane = second.FocusedPane();
            auto& b = *ActiveTab(s);
            b.loading = false;
            b.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(static_cast<size_t>(count)));
            if (count) b.SelectOnly(0);
            const int selected_index = b.selected_index;
            const auto selected = b.selected;
            const auto revision = b.selection_revision;
            HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, 0);
            Check(b.selected_index == selected_index && b.selected == selected &&
                b.selection_revision == revision && !s.dragPending && s.clickCollapseIndex < 0,
                "IP04 old collapse/Ctrl-toggle row cannot change a new tab with valid or absent same-number row");
            Dispose(owned);
        }
    }
    for (const bool changed_view : {false, true}) {
        auto owned = State();
        Check(!!owned, "IP04 initialize replaced-context row-click fixture");
        if (!owned) return;
        auto& s = *owned;
        auto& tab = *ActiveTab(s);
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(3));
        tab.SelectOnly(0);
        tab.ToggleSelect(1);
        HandleListRowClick(s, 1, false, false);
        s.dragPending = true;
        if (changed_view) ++tab.view_generation;
        else BindCurrentLayout(s);
        const auto revision = tab.selection_revision;
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, 0);
        Check(tab.SelectedCount() == 2 && tab.selection_revision == revision && !s.dragPending,
            "IP04 rebinding layout or replacing the view cancels pending row selection and drag");
        Dispose(owned);
    }
    for (const bool ctrl : {false, true}) {
        auto owned = State();
        Check(!!owned, "IP04 initialize ordinary row-click fixture");
        if (!owned) return;
        auto& s = *owned;
        auto& tab = *ActiveTab(s);
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(3));
        tab.SelectOnly(0);
        tab.ToggleSelect(1);
        HandleListRowClick(s, 1, ctrl, false);
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, 0);
        Check(tab.SelectedCount() == 1 && tab.IsSelected(ctrl ? 0 : 1),
            "IP04 same-view release preserves ordinary collapse and Ctrl-toggle behavior");
        Dispose(owned);
    }
}
void AltNavigationChecks() {
    BYTE original[256]{};
    Check(GetKeyboardState(original) != FALSE, "IP03 capture thread keyboard state");
    BYTE keys[256]{};
    keys[VK_MENU] = keys[VK_LMENU] = 0x80;
    Check(SetKeyboardState(keys) != FALSE, "IP03 stage Alt on the fixture UI thread");
    for (const UINT key : {VK_UP, VK_LEFT, VK_RIGHT}) {
        auto owned = State();
        Check(!!owned, "IP03 initialize system-key navigation fixture");
        if (!owned) break;
        auto& s = *owned;
        auto* tab = ActiveTab(s);
        tab->current_path = app::MakeHomePath();
        if (key == VK_RIGHT) tab->forward_stack.push({app::MakeRecentPath()});
        else tab->back_stack.push({app::MakeRecentPath()});
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        WndProcImpl(s.hwnd, WM_SYSKEYDOWN, key, LPARAM{1} << 29);
        Check(tab->current_path == app::MakeRecentPath(),
            "IP03 WM_SYSKEYDOWN reaches Alt+Up/Left/Right navigation");
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
        Dispose(owned);
    }
    SetKeyboardState(original);
}

void AttributeCompletionChecks() {
    auto owned = State();
    Check(!!owned, "IP01 initialize stale attribute completion fixture");
    if (!owned) return;
    auto& s = *owned;
    s.details_attribute_mailbox = std::make_shared<app::WindowResultMailbox<app::DetailsAttributeResult>>(
        s.hwnd, WM_DETAILS_ATTRIBUTE_RESULT);
    app::DetailsAttributeResult result;
    result.pane = s.pane;
    result.pane_lifetime = s.pane->lifetime_token;
    result.tab_identity = ActiveTab(s)->network_identity;
    result.view_generation = ActiveTab(s)->view_generation;
    result.path = L"C:\\fixture\\attributes.txt";
    s.details_attribute_pending[result.path] = 1;
    Check(s.details_attribute_mailbox->Publish(result), "IP01 publish owned attribute completion");
    auto& second = s.window_tabs.NewTab(L"pulse:changes:attribute-other-tab");
    s.pane = second.FocusedPane();
    auto* tab = ActiveTab(s);
    tab->pending_generation = 123;
    MSG message{};
    Check(PeekMessageW(&message, s.hwnd, WM_DETAILS_ATTRIBUTE_RESULT,
        WM_DETAILS_ATTRIBUTE_RESULT, PM_REMOVE) != FALSE, "IP01 receive mailbox notification");
    ReceiveDetailsAttributeResult(s, message.wParam);
    Check(tab->pending_generation == 123 && s.details_attribute_pending.empty(),
        "IP01 completion after switching tabs clears pending work without refreshing the other view");
    s.details_attribute_mailbox->Close();
    Check(!s.details_attribute_mailbox->Publish(std::move(result)),
        "IP01 closed window mailbox rejects late attribute completion");
    Dispose(owned);
}
void ShotChecks() {
    struct HiddenCapture {
        std::wstring previous;
        HiddenCapture() {
            const DWORD size = GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr, 0);
            previous.resize(size);
            if (size) GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", previous.data(), size);
            SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
        }
        ~HiddenCapture() {
            SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", previous.empty() ? nullptr : previous.c_str());
        }
    } hidden_capture;
    const auto output = std::filesystem::absolute(L"bench_data/audit-fixes-20261030/shot-timing");
    std::filesystem::create_directories(output);
    std::ofstream writable(output / L"write-check.txt", std::ios::binary);
    writable << "fixture output write check";
    writable.close();
    Check(!writable.fail(), "screenshot fixture output is writable");
    if (writable.fail()) return;
    for (const int length : {255, 256, 400}) {
        auto owned = State();
        Check(!!owned, "initialize screenshot timing fixture");
        if (!owned) return;
        auto& s = *owned;
        s.timing.first_frame_ms = 12.5;
        s.timing.first_frame_recorded = true;
        s.timing.sort_done_ms = 25.0;
        const char* format = "path=%ls\nfirst_frame_ms=%.2f\nenum_sort_done_ms=%.2f\n";
        const int overhead = std::snprintf(nullptr, 0, format, L"", 12.5, 25.0);
        ActiveTab(s)->current_path.assign(static_cast<size_t>(length - overhead), L'a');
        s.shot.output = (output / (std::to_wstring(length) + L".png")).wstring();
        s.shot.start = std::chrono::steady_clock::now();
        std::string expected(static_cast<size_t>(length) + 1, '\0');
        std::snprintf(expected.data(), expected.size(), format, ActiveTab(s)->current_path.c_str(), 12.5, 25.0);
        expected.resize(std::min(static_cast<size_t>(length), size_t{255}));
        const int shot_result = ShotModeMain(s, s.hwnd);
        std::printf("[INFO] %d-byte timing: ShotModeMain returned %d\n", length, shot_result);
        Check(shot_result == 0, "production shot mode renders and writes timing data");
        s.hwnd = nullptr;
        auto timing = std::filesystem::path(s.shot.output);
        timing.replace_extension(L".timing.txt");
        std::ifstream file(timing, std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Check(file.is_open() && actual == expected,
            "255/256/400-byte formatted output writes only initialized buffer bytes");
        Dispose(owned);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("[INFO] fixture main entered, pid=%lu, argc=%d\n", GetCurrentProcessId(), argc);
    const bool shot_only = argc == 2 && std::wstring_view(argv[1]) == L"--shot-timing";
    const bool input_only = argc == 2 && std::wstring_view(argv[1]) == L"--input-only";
    if (argc != 1 && !shot_only && !input_only) { Check(false, "supported fixture arguments"); return 1; }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try {
        OleInitialize(nullptr);
        l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
        if (input_only) {
            SidebarEscapeChecks();
            RowClickChecks();
            AltNavigationChecks();
            AttributeCompletionChecks();
        } else if (!shot_only) {
            ContentChecks();
            ChangeMenuChecks();
            BadgeChecks();
        }
        if (!input_only) ShotChecks();
        OleUninitialize();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[FAIL] fixture exception: %s\n", error.what());
        ++failures;
    }
    std::printf("app core lifecycle: %d failures\n", failures);
    return failures ? 1 : 0;
}
