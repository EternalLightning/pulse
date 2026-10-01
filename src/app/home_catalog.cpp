#include "home_catalog.h"
#include "places.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include <shlobj.h>
#include <shellapi.h>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
int KnownFolderIcon(const KNOWNFOLDERID& id) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    int index = -1;
    if (SUCCEEDED(SHGetKnownFolderIDList(id, KF_FLAG_DONT_VERIFY, nullptr, &pidl))) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_SYSICONINDEX))
            index = info.iIcon;
    }
    CoTaskMemFree(pidl);
    return index;
}
void ReadDriveMetadata(ui::HomeCardView& card) {
    using I = l10n::StringId;
    wchar_t name[MAX_PATH + 1]{};
    if (GetVolumeInformationW(card.path.c_str(), name, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && name[0])
        card.label = std::wstring(name) + L" (" + card.path.substr(0, 2) + L")";
    else if (card.group == 1) card.label = l10n::Get(I::LocalDisk) + L" (" + card.path.substr(0, 2) + L")";
    else {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(card.path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) && info.szDisplayName[0])
            card.label = info.szDisplayName;
    }
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
                card.system_icon_index = KnownFolderIcon(*library.id);
                cards.push_back(std::move(card));
            }
            CoTaskMemFree(path);
        }
        ui::HomeCardView recycle;
        recycle.label = l10n::Get(I::RecycleBin);
        recycle.path = MakeRecyclePath();
        recycle.glyph = L"\xE75C";
        recycle.system_icon_index = KnownFolderIcon(FOLDERID_RecycleBinFolder);
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
        {
            std::lock_guard lock(state->mutex);
            state->cards = cards;
            state->updated = true;
        }
        for (auto& card : cards) {
            if (card.group != 2) continue;
            ReadDriveMetadata(card);
            std::lock_guard lock(state->mutex);
            state->cards = cards;
            state->updated = true;
        }
        if (SUCCEEDED(com)) CoUninitialize();
        if (error_mode_changed) SetThreadErrorMode(previous_error_mode, nullptr);
        std::lock_guard lock(state->mutex);
        state->cards = std::move(cards);
        state->loading = false;
        state->updated = true;
    }).detach();
}
bool HomeCatalog::ConsumeUpdate() {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->updated, false);
}
bool HomeCatalog::Loading() const {
    std::lock_guard lock(state_->mutex);
    return state_->loading;
}
void HomeCatalog::Fill(ui::PaneViewModel& pane, const PlacesCatalog* places) const {
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
