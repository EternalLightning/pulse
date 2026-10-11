#pragma once
#include "../app/index_volume_badge.h"
#include "../app/folder_sizes_ui.h"
#include "../fs/fs_net_cache.h"
#include <filesystem>
#include <iostream>

namespace pulse::index {
struct IndexClientTestAccess {
    static void Volumes(IndexClient& client, bool failed) {
        ipc::PayloadWriter payload;
        payload.PutU32(0);
        payload.PutString(L"fixture-index");
        payload.PutU32(1);
        for (const auto* text : {L"fixture-volume", L"Fixture", L"X:\\", L"NTFS", L"unrelated caption", L"diagnostic"})
            payload.PutString(text);
        payload.PutU32(failed ? 15u : 7u);
        payload.PutU32(static_cast<uint32_t>(VolumeKind::Fixed));
        payload.PutU32(0);
        payload.PutU32(0);
        payload.PutU32(0);
        client.HandleVolumes(payload.data().data(), payload.data().size());
    }
    static bool ResetMissingForCustomPipe() {
        IndexClient client;
        client.pipe_name_ = L"\\\\.\\pipe\\PulsePresentationFixture";
        client.helper_missing_ = true;
        const bool spawned = client.SpawnHelper();
        return !spawned && !client.helper_missing_;
    }
};
}

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace pulse {
inline int RunPresentationStateCases(AppState& s) {
    using B = app::BannerType;
    using K = ui::fluent::BadgeKind;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    s.accentColor = ui::HexColor(0x0078D4);
    const auto output = std::filesystem::absolute(L"bench_data/presentation-state");
    std::filesystem::create_directories(output);
    auto capture = [&](ui::WindowViewModel& vm, const wchar_t* name, bool dark) {
        vm.dark = dark;
        s.scale = 1.0f;
        s.renderer.SetScale(1.0f);
        s.compositor.RecreateTextFormats(1.0f);
        s.compositor.Resize(1100, 720);
        const auto bounds = D2D1::RectF(0, 0, 1100, 720);
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm, bounds, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
        const bool drawn = SUCCEEDED(s.compositor.Dc()->EndDraw());
        check(drawn && s.compositor.SaveSnapshot((output / name).c_str()), "presentation fixture renders and saves screenshot");
    };
    auto* tab = ActiveTab(s);
    uint64_t generation = 9200;
    auto complete = [&](DWORD error = ERROR_SUCCESS, bool delta = false) {
        tab->pending_generation = ++generation;
        tab->search_content_active = true;
        index::ContentSearchUpdate update;
        update.progress.generation = generation;
        update.progress.done = true;
        update.progress.error = error;
        update.progress.delta = delta;
        ApplyContentSearchUpdate(s, std::move(update));
    };
    for (bool english : {false, true}) {
        tab->SetBannerTitle(B::SearchIncomplete, l10n::Get(l10n::StringId::SearchIncomplete));
        l10n::Initialize(GetModuleHandleW(nullptr), english ? L"en-US" : L"zh-CN");
        tab->current_path = app::MakeSearchPath(L"content:fixture");
        tab->banner_message = L"diagnostic";
        complete();
        check(tab->banner_type == B::None && tab->banner_title.empty() && tab->banner_message.empty(),
            "successful content result clears incomplete banner by type across languages");
        tab->SetBannerTitle(B::Other, l10n::Get(l10n::StringId::SearchIncomplete));
        complete();
        check(tab->banner_type == B::Other && !tab->banner_title.empty(),
            "matching incomplete caption does not clear a different banner type");
        tab->SetBannerTitle(B::ContentScanRunning, L"arbitrary scan caption");
        complete();
        check(tab->banner_type == B::None, "completed scan clears its typed banner independently of caption");
        tab->SetBannerTitle(B::SearchIncomplete, L"arbitrary error caption");
        complete(ERROR_SUCCESS, true);
        check(tab->banner_type == B::SearchIncomplete, "live delta does not clear scan failure banner");
        complete(ERROR_ACCESS_DENIED);
        check(tab->banner_type == B::SearchIncomplete, "failed content result produces incomplete machine type");
        tab->SetBannerTitle(B::ContentIndex, L"arbitrary background caption");
        tab->banner_message = L"background";
        tab->search_content_active = true;
        tab->search_allow_scan = false;
        s.contentStatusTick = 0;
        TickAddressSearch(s, GetTickCount64() + 750);
        check(tab->banner_type == B::None && tab->banner_title.empty(),
            "foreground query clears typed background index banner independently of caption");
        tab->SetBannerTitle(B::Other, l10n::Get(l10n::StringId::ContentIndexBuilding));
        s.contentStatusTick = 0;
        TickAddressSearch(s, GetTickCount64() + 750);
        check(tab->banner_type == B::Other, "background status leaves unrelated matching caption intact");
        tab->search_content_active = false;
        tab->pending_generation = 0;
        tab->current_path = L"\\\\presentation-fixture.invalid\\share";
        tab->SetBannerTitle(B::Offline, L"previous-language offline caption");
        tab->banner_message = L"snapshot";
        tab->net_readonly = true;
        auto probe = [&](fs::NetStatus status) {
            auto result = std::make_unique<fs::UncProbeResult>();
            result->unc = tab->current_path;
            result->probe_id = s.probe_scheduler.Begin(tab->current_path);
            result->status = status;
            ::WndProcImpl(s.hwnd, WM_NET_PROBE, 0, reinterpret_cast<LPARAM>(result.release()));
        };
        probe(fs::NetStatus::Online);
        check(!tab->net_readonly && tab->banner_type == B::None && tab->banner_message.empty(),
            "network recovery clears offline type across languages");
        tab->SetBannerTitle(B::Other, l10n::Get(l10n::StringId::Offline));
        probe(fs::NetStatus::Online);
        check(tab->banner_type == B::Other, "network recovery preserves unrelated matching offline caption");
        tab->ClearBanner();
        check(tab->banner_type == B::None && tab->banner_title.empty() && tab->banner_message.empty(),
            "banner clear resets machine type and both display fields");

        s.window_tabs.Active()->title = l10n::Get(l10n::StringId::Settings);
        tab->current_path = L"C:\\presentation-fixture\\设置";
        ui::WindowViewModel identity;
        app::FillWindowTabStrip(identity, s.window_tabs);
        check(!identity.tabs[0].is_settings, "ordinary folder with settings alias retains folder identity");
        s.window_tabs.Active()->title = L"Renamed settings page";
        tab->current_path = app::MakeSettingsPath(L"index");
        app::FillWindowTabStrip(identity, s.window_tabs);
        check(identity.tabs[0].is_settings && identity.tabs[0].title == L"Renamed settings page",
            "settings identity survives user tab rename");

        index::VolumeInfo volume;
        volume.online = volume.supported = volume.enabled = true;
        for (const auto* caption : {L"正在索引", L"Failed", L"Ready", L"unrelated text"}) {
            volume.state = caption;
            check(app::IndexVolumeBadgeKind(volume) == K::Accent, "waiting volume badge ignores localized caption");
        }
        volume.failed = true;
        volume.error.clear();
        check(app::IndexVolumeBadgeKind(volume) == K::Danger, "failure badge uses failure field even without diagnostic text");
        volume.failed = false;
        volume.progress = 100;
        check(app::IndexVolumeBadgeKind(volume) == K::Success, "indexed empty volume has success badge");
        volume.online = false;
        check(app::IndexVolumeBadgeKind(volume) == K::Neutral, "offline searchable volume has neutral badge");
        volume.online = true;
        volume.supported = false;
        check(app::IndexVolumeBadgeKind(volume) == K::Warning, "unsupported volume has warning badge");
        index::IndexClientTestAccess::Volumes(s.index, true);
        check(s.index.Volumes().size() == 1 && s.index.Volumes()[0].failed, "volume protocol carries failure bit independent of caption");
        index::IndexClientTestAccess::Volumes(s.index, false);
        check(!s.index.Volumes()[0].failed, "next volume response clears previous failure bit");
        auto settings = BuildVm(s, false);
        check(settings.settings_index_volumes.size() == 1 && settings.settings_index_volumes[0].badge_kind == K::Accent,
            "settings view receives machine-derived volume badge metadata");
        settings.settings_expanded |= 2u;
        settings.settings_scroll = 0;
        ui::fluent::Painter settings_painter(&s.compositor);
        settings_painter.SetScale(1.0f);
        const auto settings_bounds = D2D1::RectF(0, 0, 1100, 720);
        const auto settings_layout = ui::MakeSettingsLayout(settings, settings_bounds, 1.0f,
            s.renderer.TitleBarHeight(), 28.0f, &settings_painter);
        if (!settings_layout.index_volume_rows.empty()) {
            settings.settings_scroll = std::clamp(settings_layout.index_volume_rows[0].top -
                settings_layout.content.top - 100.0f, 0.0f, s.renderer.SettingsMaxScroll(settings, 1100, 720));
            const auto visible = ui::MakeSettingsLayout(settings, settings_bounds, 1.0f,
                s.renderer.TitleBarHeight(), 28.0f, &settings_painter);
            check(!visible.index_volume_rows.empty() && visible.index_volume_rows[0].top >= visible.content.top &&
                visible.index_volume_rows[0].bottom <= visible.content.bottom,
                "settings screenshot contains the complete volume badge row");
        }
        capture(settings, english ? L"settings_en.png" : L"settings_zh.png", !english);

        tab->current_path = L"pulse:changes:C:\\presentation-fixture";
        tab->virtual_title = l10n::Get(l10n::StringId::ChangeTitle) + L" · presentation-fixture";
        s.window_tabs.Active()->title.clear();
        tab->view_mode = ui::ViewMode::Details;
        tab->ClearSelection();
        tab->scroll_y = 0;
        tab->change_cursor = 0;
        tab->change_status_text.clear();
        tab->content_results.reset();
        tab->search_content_active = false;
        tab->pending_generation = 0;
        s.appPrefs.change_tracking_enabled = true;
        tab->change_request = 9300;
        s.changes.details[9300] = {tab, tab->current_path, tab->view_generation, false};
        index::ChangeResponse response;
        response.state = index::ChangeState::Available;
        index::ChangeRecord deleted;
        deleted.kind = index::ChangeKind::Deleted;
        deleted.path = L"C:\\presentation-fixture\\deleted.txt";
        index::ChangeRecord moved = deleted;
        moved.kind = index::ChangeKind::MovedOut;
        moved.old_path = L"C:\\presentation-fixture\\moved.txt";
        response.records = {deleted, moved};
        ApplyChangeDetails(s, 9300, response);
        check(tab->snapshot && tab->snapshot->size() == 2 && (*tab->snapshot)[0].change_deleted &&
            !(*tab->snapshot)[1].change_deleted && (*tab->snapshot)[1].change_record_only,
            "deleted and moved-out event producers preserve distinct machine metadata");
        if (tab->snapshot && tab->snapshot->size() == 2) {
            auto rows = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
            (*rows)[0].change_type_text = L"arbitrary translated deletion";
            (*rows)[1].change_type_text = l10n::Get(l10n::StringId::ChangeDeleted);
            tab->SetSnapshot(rows);
            auto changes = BuildVm(s, false);
            auto& pane = changes.pane_slots[0].pane;
            check(ui::MakeVisibleEntry(pane, 0).change_deleted && !ui::MakeVisibleEntry(pane, 1).change_deleted,
                "row presentation preserves deletion identity independently of translated type text");
            capture(changes, english ? L"changes_en.png" : L"changes_zh.png", !english);
        }

        ui::WindowViewModel folders;
        ui::PaneSlotView slot;
        slot.focused = true;
        slot.rect = D2D1::RectF(0, 160, 1100, 690);
        slot.pane.is_file_system = true;
        slot.pane.view_mode = ui::ViewMode::Content;
        ui::ListEntryView folder;
        folder.name = L"Manual folder";
        folder.is_dir = true;
        folder.path = L"\\\\presentation-fixture.invalid\\share\\manual";
        slot.pane.entries.push_back(folder);
        folders.pane_slots.push_back(std::move(slot));
        FillFolderSizes(s, folders);
        const auto label = folders.pane_slots[0].pane.folder_size_labels.find(0);
        check(label != folders.pane_slots[0].pane.folder_size_labels.end() && label->second.manual &&
            folders.pane.folder_size_labels[0].manual, "folder size producer and focused pane retain manual machine flag");
        if (label != folders.pane_slots[0].pane.folder_size_labels.end()) {
            label->second.text = L"arbitrary manual caption";
            folders.pane_slots[0].pane.entries.push_back(folder);
            folders.pane_slots[0].pane.entries[1].name = L"Completed folder";
            folders.pane_slots[0].pane.folder_size_labels[1] = {l10n::Get(l10n::StringId::FolderSizeCalculate), false};
            capture(folders, english ? L"folders_en.png" : L"folders_zh.png", !english);
            folders.pane_slots[0].pane.view_mode = ui::ViewMode::LargeIcons;
            capture(folders, english ? L"icons_en.png" : L"icons_zh.png", !english);
        }
        folders.pane_slots[0].pane.view_mode = ui::ViewMode::Details;
        FillFolderSizes(s, folders);
        check(folders.pane_slots[0].pane.folder_size_labels.empty() && folders.pane.folder_size_labels.empty(),
            "unsupported layout clears manual metadata together with labels");
    }
    check(index::IndexClientTestAccess::ResetMissingForCustomPipe(), "custom pipe helper attempt clears stale missing-exe flag without spawning");
    s.folderSizes.Stop();
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
    return failures ? 1 : 0;
}
}
