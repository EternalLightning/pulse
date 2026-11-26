// SPDX-License-Identifier: Apache-2.0
#include "app_file_picker.h"
#include "app_state.h"
#include "file_picker_places.h"
#include "link_resolve.h"
#include "../common/display_path.h"
#include "../common/known_folder_labels.h"
#include "../fs/fs_enum.h"

#include <chrono>
#include <filesystem>
#include <future>
#include <thread>

namespace pulse {

void app::SetFilePickerFolderViews(ui::FilePickerSpec& spec, const AppPrefs& prefs) {
    spec.folder_sort = model::FolderSortModeFromInt(prefs.folder_sort_mode);
    spec.folder_view = [&views = prefs.folder_views](const std::wstring& path) {
        const auto* known = path::FindKnownFolderLabel(path, path::KnownFolderLabels());
        const auto sort = views.FindSort(path).value_or(
            DefaultFolderSort(known && known->label == l10n::StringId::Downloads));
        const auto group = views.FindGroup(path).value_or(FolderGroupPrefs{});
        return ui::PickerFolderView{
            views.Find(path).value_or(ui::ViewMode::Details), sort.column, sort.direction,
            group.mode, group.direction, views.FindColumns(path).value_or(std::array<float, 4>{}),
            views.FindColumnLayout(path).value_or(model::DetailsColumnPrefs{})};
    };
}

bool PickPulsePath(AppState& state, HWND owner, ui::FilePickerSpec spec, std::wstring& path,
                   const std::function<bool(const std::wstring&)>& validate) {
    if (spec.mode == ui::PickerMode::Save) return false;
    spec.modal_owner = true;
    spec.visibility = {state.appPrefs.show_hidden_files, state.appPrefs.show_protected_os_files};
    spec.verify_copies = state.appPrefs.verify_copies;
    spec.zebra_rows = state.appPrefs.list_zebra_rows;
    app::SetFilePickerFolderViews(spec, state.appPrefs);
    spec.load_places = app::LoadFilePickerPlaces;
    spec.should_close = [owner] { return !IsWindow(owner); };

    // Typed paths can reference another drive or network location.
    std::future<std::wstring> pending;
    std::wstring selected_path;
    spec.begin_submit = [&](const ui::FilePickerResult& choice) {
        std::packaged_task<std::wstring()> check([mode = spec.mode, chosen = choice.paths.front(), validate]() mutable {
            if (mode == ui::PickerMode::Open &&
                _wcsicmp(std::filesystem::path(chosen).extension().c_str(), L".lnk") == 0) {
                fs::DirEntry shortcut;
                if (!app::ResolveLink(fs::NormalizePath(chosen), shortcut)) return std::wstring{};
                chosen = path::FriendlyPathText(shortcut.link_target);
            }
            const DWORD attrs = GetFileAttributesW(fs::NormalizePath(chosen).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES ||
                ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) != (mode == ui::PickerMode::Folder) ||
                (validate && !validate(chosen))) return std::wstring{};
            return chosen;
        });
        pending = check.get_future();
        try { std::thread(std::move(check)).detach(); }
        catch (const std::system_error&) { pending = {}; return false; }
        return true;
    };
    spec.poll_submit = [&]() -> std::optional<ui::PickerSubmitReply> {
        if (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return std::nullopt;
        try { selected_path = pending.get(); }
        catch (...) { return ui::PickerSubmitReply{false, true, {}}; }
        return ui::PickerSubmitReply{!selected_path.empty(), true, {}};
    };
    ui::FilePickerResult result;
    if (!ui::ShowFilePicker(owner, spec, state.darkMode, state.accentColor, result)) return false;
    path = std::move(selected_path);
    return true;
}

} // namespace pulse
