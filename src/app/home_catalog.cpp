#include "home_catalog.h"
#include "app_model.h"
#include "places.h"
#include "../model/drive_labels.h"
#include "../common/drive_display_name.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ipc/shell_query_client.h"
#include <shlobj.h>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
void ReadDriveMetadata(ui::HomeCardView& card) {
    using I = l10n::StringId;
    wchar_t name[MAX_PATH + 1]{};
    if (card.group == 2) {
        if (const auto label = path::ReadMappedDriveLabel(card.path))
            card.label = model::DriveDisplayName(card.path, *label);
        else {
            ipc::ShellQueryRequest request;
            request.kind = ipc::ShellQueryKind::DisplayName;
            request.path = card.path;
            ipc::ShellQueryResult result;
            if (ipc::QueryShell(request, result) && SUCCEEDED(result.error) && !result.text.empty())
                card.label = std::move(result.text);
        }
    } else if (GetVolumeInformationW(card.path.c_str(), name, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && name[0])
        card.label = std::wstring(name) + L" (" + card.path.substr(0, 2) + L")";
    else card.label = l10n::Get(I::LocalDisk) + L" (" + card.path.substr(0, 2) + L")";
    ULARGE_INTEGER available{}, total{}, free{};
    if (GetDiskFreeSpaceExW(card.path.c_str(), &available, &total, &free) && total.QuadPart) {
        card.used_ratio = static_cast<float>(static_cast<double>(total.QuadPart - std::min(total.QuadPart, free.QuadPart)) / static_cast<double>(total.QuadPart));
        wchar_t capacity[160]{};
        swprintf_s(capacity, l10n::Get(I::HomeDriveSpaceFormat).c_str(),
            format::ByteSize(free.QuadPart).c_str(), format::ByteSize(total.QuadPart).c_str());
        card.detail = capacity;
    } else card.detail = l10n::Get(I::HomeCapacityUnavailable);
}
}
void HomeCatalog::Refresh() {
    auto state = state_;
    {
        std::lock_guard lock(state->mutex);
        if (state->loading) return;
        state->loading = true;
    }
    std::thread([state] {
        using I = l10n::StringId;
        DWORD previous_error_mode = 0;
        const bool error_mode_changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_error_mode) != FALSE;
        std::vector<ui::HomeCardView> cards;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const struct { const KNOWNFOLDERID* id; I label; const wchar_t* glyph; } libraries[] = {
            {&FOLDERID_Desktop, I::Desktop, L"\xE7F4"},
            {&FOLDERID_Pictures, I::KnownFolderPictures, L"\xEB9F"},
            {&FOLDERID_Videos, I::KnownFolderVideos, L"\xE714"},
            {&FOLDERID_Downloads, I::Downloads, L"\xE896"}
        };
        for (const auto& library : libraries) {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(*library.id, KF_FLAG_DONT_VERIFY, nullptr, &path))) {
                ui::HomeCardView card;
                card.label = l10n::Get(library.label);
                card.path = path;
                card.glyph = library.glyph;
                card.shell_icon_path = card.path;
                cards.push_back(std::move(card));
            }
            CoTaskMemFree(path);
        }
        ui::HomeCardView recycle;
        recycle.label = l10n::Get(I::RecycleBin);
        recycle.path = MakeRecyclePath();
        recycle.glyph = L"\xE75C";
        recycle.shell_icon_path = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
        cards.push_back(std::move(recycle));
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) {
            if (!(drives & (1u << i))) continue;
            const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            const UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_CDROM && type != DRIVE_REMOTE) continue;
            ui::HomeCardView card;
            card.path = root;
            card.group = type == DRIVE_REMOTE ? 2 : 1;
            card.glyph = L"\xE7F1";
            card.label = std::wstring(root, 2);
            if (type != DRIVE_REMOTE) ReadDriveMetadata(card);
            else card.detail = l10n::Get(I::HomeLoading);
            cards.push_back(std::move(card));
        }
        // Publish local content before contacting network providers, which may be slow or offline.
        const auto publish = [&](bool complete) {
            std::lock_guard lock(state->mutex);
            state->cards = cards;
            for (auto& card : state->cards) {
                for (const auto& [root, label] : state->label_updates) {
                    if (path::EqualInsensitive(path::StripExtendedPathPrefix(card.path), root))
                        card.label = model::DriveDisplayName(root, label);
                }
            }
            if (complete) {
                state->loading = false;
                state->label_updates.clear();
            }
            state->updated = true;
        };
        publish(false);
        for (auto& card : cards) {
            if (card.group != 2) continue;
            ReadDriveMetadata(card);
            publish(false);
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (error_mode_changed) SetThreadErrorMode(previous_error_mode, nullptr);
        publish(true);
    }).detach();
}
void HomeCatalog::UpdateDriveLabel(const std::wstring& root, const std::wstring& label) {
    const auto plain = path::StripExtendedPathPrefix(root).substr(0, 2) + L"\\";
    std::lock_guard lock(state_->mutex);
    if (state_->loading) state_->label_updates[plain] = label;
    for (auto& card : state_->cards) {
        if (path::EqualInsensitive(path::StripExtendedPathPrefix(card.path), plain))
            card.label = model::DriveDisplayName(root, label);
    }
    state_->updated = true;
}
bool HomeCatalog::ConsumeUpdate() {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->updated, false);
}
bool HomeCatalog::Loading() const {
    std::lock_guard lock(state_->mutex);
    return state_->loading;
}
void HomeCatalog::FillDriveLabels(SidebarModel& sidebar) const {
    std::lock_guard lock(state_->mutex);
    for (auto& drive : sidebar.drives) {
        for (const auto& card : state_->cards) {
            if (card.group != 0 && path::EqualInsensitive(
                    path::StripExtendedPathPrefix(drive.path), card.path)) {
                drive.label = card.label;
                break;
            }
        }
    }
}
void HomeCatalog::Fill(ui::PaneViewModel& pane, const model::PlacesCatalog* places) const {
    if (!pane.is_home) return;
    std::lock_guard lock(state_->mutex);
    pane.home_cards = state_->cards;
    pane.home_loading = state_->loading;
    if (places) {
        for (const auto& place : places->networks) {
            if (place.unc.empty()) continue;
            const bool exists = std::any_of(pane.home_cards.begin(), pane.home_cards.end(),
                [&](const auto& card) { return _wcsicmp(card.path.c_str(), place.unc.c_str()) == 0; });
            if (!exists) pane.home_cards.push_back({place.name.empty() ? place.unc : place.name,
                place.unc, place.unc, L"\xE968", 2});
        }
    }
}
}
