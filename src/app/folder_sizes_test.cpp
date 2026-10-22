#ifdef PULSE_WITH_SELFTEST
#include "folder_sizes.h"
#include "folder_sizes_ui.h"
#include "app_state.h"
#include "../ui/ui_renderer_internal.h"
#include "../common/windows_compat.h"
#include "../common/text_format.h"
#include <filesystem>
#include <fstream>
#include <functional>

namespace pulse::ui {
struct FolderSizesUiTest {
    static bool Run(const std::filesystem::path& root, std::ofstream& log) {
        HWND hwnd = CreateWindowExW(0, L"STATIC", L"Folder sizes", WS_POPUP,
            0, 0, 920, 550, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        bool ok = hwnd != nullptr;
        {
            Compositor compositor;
            if (!hwnd || !compositor.Init(hwnd)) { if (hwnd) DestroyWindow(hwnd); return false; }
            MainRenderer renderer;
            renderer.SetCompositor(&compositor);
            for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) for (bool dark : {false, true}) for (int width : {920, 440}) for (auto mode : {ViewMode::Tiles, ViewMode::MediumIcons, ViewMode::LargeIcons, ViewMode::ExtraLargeIcons, ViewMode::Content}) {
                compositor.Resize(static_cast<UINT>(width * scale), static_cast<UINT>(550 * scale));
                compositor.RecreateTextFormats(scale);
                renderer.SetScale(scale);
                const auto theme = MakeTheme(dark, HexColor(0x0078D4));
                renderer.UpdateBrushes(theme);
                renderer.text_background_ = theme.bg;
                renderer.icon_cache_.SetDeviceContext(compositor.Dc());
                renderer.painter_.BeginFrame(theme, false);
                WindowViewModel vm;
                vm.dark = dark;
                PaneViewModel pane;
                pane.path = L"C:\\资料"; pane.header_text = L"资料";
                pane.view_mode = mode;
                pane.selected_index = 1; pane.selected_count = 1;
                const wchar_t* names[] = {L"项目资料", L"设计素材", L"照片归档", L"工作文档", L"视频素材",
                    L"下载整理", L"共享资料", L"历史备份", L"空文件夹"};
                const wchar_t* sizes[] = {L"12.8 GB · 索引估算", L"4.62 GB", L"86.3 GB", L"328 MB", L"24.6 GB · 更新中",
                    L"计算中…", L"点击统计", L"8.4 GB · 部分统计", L"0 B"};
                for (int i = 0; i < 9; ++i) {
                    ListEntryView e;
                    e.name = names[i]; e.is_dir = true; e.path = pane.path + L"\\" + names[i];
                    pane.entries.push_back(std::move(e)); pane.folder_size_labels[i] = {sizes[i], i == 6};
                }
                pane.folder_size_actions.insert(6);
                pane.folder_size_actions.insert(0);
                ViewLayout geometry(mode, {0, 0, static_cast<float>(width)*scale, 550*scale}, 9, 0, 0, scale);
                const auto name_bounds = geometry.NameRect(0);
                const auto size_bounds = geometry.FolderSizeRect(0);
                const bool fits = size_bounds.top >= name_bounds.bottom && size_bounds.bottom <= geometry.ItemRect(0).bottom &&
                    size_bounds.bottom-size_bounds.top >= 21*scale;
                log << (fits ? "[PASS] " : "[FAIL] ") << "capacity line fits below name within cell\n";
                ok &= fits;
                auto* dc = compositor.Dc();
                dc->BeginDraw(); dc->Clear(theme.bg);
                const auto bounds = D2D1::RectF(8 * scale, 8 * scale, (width - 8.0f) * scale, 542 * scale);
                renderer.DrawSinglePane(vm, pane, bounds,
                    0, true, false, theme);
                const bool drawn = SUCCEEDED(dc->EndDraw());
                const auto shot = root / (std::wstring(ViewModeName(mode)) + L"-" + (width == 440 ? std::wstring(L"narrow-") : std::wstring()) +
                    std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark.png" : L"-light.png"));
                const bool saved = drawn && compositor.SaveSnapshot(shot.c_str());
                log << (saved ? "[PASS] " : "[FAIL] ") << "size states render mode=" << ViewModeIndex(mode) << " scale=" << scale << " dark=" << dark << std::endl;
                ok &= saved;
                if (width == 920) {
                    // Exercise production hit-testing with a real pane slot, away from chrome.
                    const auto hit_bounds = D2D1::RectF(240 * scale, 120 * scale, 900 * scale, 540 * scale);
                    PaneSlotView slot; slot.rect = hit_bounds; slot.pane = pane; slot.focused = true;
                    vm.pane_slots = {slot};
                    const auto list = renderer.PaneListRect(pane, hit_bounds);
                    ViewLayout positions(mode, list, pane.EntryCount(), 0, 0, scale);
                    const int action = mode == ViewMode::Tiles ? 6 : 0;
                    const auto name = positions.NameRect(action);
                    const auto hit = renderer.HitTest(vm, D2D1::RectF(0, 0, 920 * scale, 550 * scale),
                        name.left + 4 * scale, name.top + 32 * scale);
                    const bool clickable = hit.region == HitTestResult::RowFolderSize && hit.index == action;
                    log << (clickable ? "[PASS] " : "[FAIL] ") << "manual size label hit-tests independently of folder open\n";
                    ok &= clickable;
                }
            }
        }
        DestroyWindow(hwnd);
        return ok;
    }
};
}
namespace pulse::app {
bool RunFolderSizeIndexClientTest(const std::filesystem::path& fixture, std::ofstream& log);
bool RunFolderSizesTest() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;
    compat::EnableDpiAwareness();
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::current_path() / L"bench_data" / L"folder-sizes";
    const auto fixture = root / (L"fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(fixture / L"nested" / L"child");
    std::filesystem::create_directory(fixture / L"empty");
    std::ofstream log(root / L"results.log");
    bool ok = true;
    auto check = [&](bool passed, const char* label) {
        log << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl; ok &= passed;
    };
    auto file = [&](const std::filesystem::path& p, size_t bytes) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << std::string(bytes, 'x');
    };
    auto wait = [&](const std::function<bool()>& predicate) {
        const auto deadline = GetTickCount64() + 6000;
        while (GetTickCount64() < deadline) { if (predicate()) return true; Sleep(10); }
        return predicate();
    };
    file(fixture / L"nested" / L"a.bin", 1234);
    file(fixture / L"nested" / L"child" / L"b.bin", 5678);
    const auto nested = (fixture / L"nested").wstring();
    const auto empty = (fixture / L"empty").wstring();
    const auto missing = (fixture / L"missing").wstring();
    const auto cache = (fixture / L"cache.json").wstring();
    {
        FolderSizes service;
        service.SetIndexEnabled(false);
        service.SetCachePath([cache] { return cache; });
        service.Sync({{nested}, {empty}, {missing}, {L"\\\\not-a-server\\share", false}}, {});
        check(service.Get(L"\\\\not-a-server\\share").state == FolderSizeState::Manual,
              "network folder remains manual without issuing a scan");
        check(wait([&] { return service.Get(nested).state == FolderSizeState::Ready; }) && service.Get(nested).bytes == 6912,
              "nested logical file sizes are summed exactly");
        check(wait([&] { return service.Get(empty).state == FolderSizeState::Ready; }) &&
            service.Get(empty).has_value && service.Get(empty).bytes == 0 && format::ByteSize(0) == L"0 B",
              "empty directory is a known zero, not unknown");
        check(wait([&] { return service.Get(missing).state == FolderSizeState::Unavailable; }) && !service.Get(missing).has_value,
              "unreadable or missing root never becomes a false zero");
        file(fixture / L"nested" / L"child" / L"b.bin", 1000);
        service.Invalidate((fixture / L"nested" / L"child" / L"b.bin").wstring());
        const auto stale = service.Get(nested);
        check(stale.has_value && stale.bytes == 6912 && stale.state == FolderSizeState::Updating,
              "old value remains visible while invalidated data updates");
        check(wait([&] { const auto v = service.Get(nested); return v.state == FolderSizeState::Ready && v.bytes == 2234; }),
              "changed descendant refreshes its parent total");
        for (int i = 0; i < 30; ++i) service.Sync({{i % 2 ? nested : empty}}, {});
        service.Sync({{empty}}, {});
        check(wait([&] { return service.Get(empty).state == FolderSizeState::Ready; }) && service.Get(empty).bytes == 0,
              "rapid scope changes cannot publish a previous directory total into the new one");
        service.Stop();
    }
    {
        FolderSizes loaded;
        loaded.SetIndexEnabled(false);
        loaded.SetCachePath([cache] { return cache; });
        loaded.Sync({{nested, false}}, {});
        check(wait([&] { return loaded.Get(nested).has_value; }) && loaded.Get(nested).bytes == 2234 &&
            loaded.Get(nested).state == FolderSizeState::Cached,
              "disk cache survives restart and is explicitly stale before validation");
        loaded.Calculate(nested);
        check(wait([&] { return loaded.Get(nested).state == FolderSizeState::Ready; }),
              "manual calculate starts work and replaces cached status");
        loaded.Stop();
    }
    const auto offline = fixture / L"nested" / L"offline";
    std::filesystem::create_directory(offline); file(offline / L"skipped.bin", 9000);
    if (SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE)) {
        FolderSizes partial; partial.SetIndexEnabled(false); partial.Sync({{nested}}, {});
        check(wait([&] { return partial.Get(nested).state == FolderSizeState::Partial; }) && partial.Get(nested).bytes == 2234,
              "offline subtree is skipped without discarding readable bytes or reporting a complete total");
        partial.Stop(); SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
    } else log << "[SKIP] filesystem cannot create offline-directory fixture" << std::endl;
    {
        FolderSizes watched;
        watched.SetIndexEnabled(false);
        watched.Sync({{nested}}, {nested});
        check(wait([&] { return watched.Get(nested).state == FolderSizeState::Ready; }),
              "recursive watcher and initial scan settle together");
        file(fixture / L"nested" / L"child" / L"b.bin", 3333);
        check(wait([&] { const auto v = watched.Get(nested); return v.state == FolderSizeState::Ready && v.bytes == 13567; }),
              "real descendant write triggers automatic recalculation without UI refresh");
        watched.Stop();
    }
    {
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->appPrefs.persist = false;
        state->settings.BindUi(state->appPrefs, state->ctxMenuPrefs, state->index, state->networkIndex, {});
        ui::WindowViewModel vm;
        ui::PaneSlotView slot;
        slot.rect = D2D1::RectF(240, 120, 900, 550); slot.focused = true;
        slot.pane.path = fixture.wstring(); slot.pane.is_file_system = true;
        slot.pane.view_mode = ui::ViewMode::Tiles;
        ui::ListEntryView e; e.name = L"empty"; e.path = empty; e.is_dir = true;
        slot.pane.entries.push_back(e);
        vm.pane_slots.push_back(slot);
        PrepareFolderSizes(*state, vm);
        check(wait([&] { FillFolderSizes(*state, vm); return vm.pane_slots[0].pane.folder_size_labels[0].text == L"0 B"; }),
              "real view-model wiring requests and displays the last visible row, including a single-item folder");
        for (auto mode : {ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons, ui::ViewMode::ExtraLargeIcons, ui::ViewMode::Content}) {
            vm.pane_slots[0].pane.view_mode = mode;
            PrepareFolderSizes(*state, vm);
            check(vm.pane_slots[0].pane.folder_size_labels[0].text == L"0 B", "grid/content view receives actual folder total");
        }
        vm.pane_slots[0].pane.view_mode = ui::ViewMode::Details;
        PrepareFolderSizes(*state, vm);
        check(vm.pane_slots[0].pane.folder_size_labels.empty(), "unsupported layout removes size labels and requests");
        state->folderSizes.Stop();
    }
    check(RunFolderSizeIndexClientTest(fixture, log), "folder size index IPC integration");
    check(ui::FolderSizesUiTest::Run(root, log), "actual tile renderer shows all size states");
    std::filesystem::remove_all(fixture);
    CoUninitialize();
    return ok;
}
}
#endif
