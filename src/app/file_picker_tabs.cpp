// SPDX-License-Identifier: Apache-2.0
#include "file_picker_tabs.h"

#include "app_state.h"
#include "../common/display_path.h"
#include "../ipc/picker_transport.h"
#include "../ipc/protocol.h"

namespace pulse::app {
namespace {

constexpr DWORD kSnapshotBytes = 1024 * 1024;
constexpr wchar_t kFolderGlyph[] = L"\xE8B7";

std::wstring MappingName(DWORD process) {
    return L"Local\\Pulse.PickerTabs.1." + std::to_wstring(process);
}

bool UsableFolder(const Tab* tab) {
    return tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path) &&
        tab->archive_file.empty() && !tab->listing_failed;
}

std::vector<ui::PickerPlace> CollectPickerTabs(const AppState& state) {
    std::vector<ui::PickerPlace> places;
    state.core.windows.ForEach([&](HWND, const AppState& window) {
        if (window.exit_requested) return;
        for (const auto& layout : window.window_tabs.items) {
            if (!layout) continue;
            const auto* folder = layout->ActiveFolder();
            if (UsableFolder(folder)) {
                places.push_back({LayoutTabTitle(*layout), path::FriendlyPathText(folder->current_path), kFolderGlyph});
            } else {
                for (const auto& pane : layout->panes) {
                    folder = pane ? pane->ActiveTab() : nullptr;
                    if (!UsableFolder(folder)) continue;
                    places.push_back({layout->title.empty() ? TabTitle(folder->current_path) : layout->title,
                        path::FriendlyPathText(folder->current_path), kFolderGlyph});
                    break;
                }
            }
        }
    });
    return places;
}

} // namespace

bool PublishPickerTabs(const AppState& state) {
    const auto places = CollectPickerTabs(state);
    if (places.empty()) return false;
    ipc::PayloadWriter payload;
    for (const auto& place : places) {
        const auto bytes = (place.label.size() + place.directory.size()) * sizeof(wchar_t) + 2 * sizeof(uint32_t);
        if (payload.data().size() + bytes > kSnapshotBytes - sizeof(uint32_t)) break;
        payload.PutString(place.label);
        payload.PutString(place.directory);
    }
    if (payload.data().empty()) return false;

    const auto name = MappingName(GetCurrentProcessId());
    const auto mutex_name = name + L".Lock";
    // Keep the objects alive while picker workers open and copy the reply.
    static const ipc::picker::PipeHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, kSnapshotBytes, name.c_str()));
    static const ipc::picker::PipeHandle mutex(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (!mapping || !mutex) return false;
    const std::unique_ptr<void, decltype(&UnmapViewOfFile)> view(
        MapViewOfFile(mapping.Get(), FILE_MAP_WRITE, 0, 0, kSnapshotBytes), UnmapViewOfFile);
    if (!view) return false;
    const auto wait = WaitForSingleObject(mutex.Get(), 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return false;
    const auto size = static_cast<uint32_t>(payload.data().size());
    auto* data = static_cast<uint8_t*>(view.get());
    std::memcpy(data, &size, sizeof(size));
    std::memcpy(data + sizeof(size), payload.data().data(), size);
    ReleaseMutex(mutex.Get());
    return true;
}

std::vector<ui::PickerPlace> ReadPickerTabs(HWND source) {
    DWORD process = 0;
    if (!GetWindowThreadProcessId(source, &process)) return {};
    COPYDATASTRUCT request{kPickerTabsRequest, 0, nullptr};
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(source, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&request),
            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 150, &result) || !result) return {};

    const auto name = MappingName(process);
    const ipc::picker::PipeHandle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()));
    const ipc::picker::PipeHandle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".Lock").c_str()));
    if (!mapping || !mutex) return {};
    const std::unique_ptr<void, decltype(&UnmapViewOfFile)> view(
        MapViewOfFile(mapping.Get(), FILE_MAP_READ, 0, 0, kSnapshotBytes), UnmapViewOfFile);
    if (!view) return {};
    if (const auto wait = WaitForSingleObject(mutex.Get(), 50); wait != WAIT_OBJECT_0) {
        if (wait == WAIT_ABANDONED) ReleaseMutex(mutex.Get());
        return {};
    }
    std::unique_ptr<void, decltype(&ReleaseMutex)> lock(mutex.Get(), ReleaseMutex);
    const auto* data = static_cast<const uint8_t*>(view.get());
    uint32_t size = 0;
    std::memcpy(&size, data, sizeof(size));
    if (size > kSnapshotBytes - sizeof(size)) return {};
    const std::vector<uint8_t> payload(data + sizeof(size), data + sizeof(size) + size);
    lock.reset();

    ipc::PayloadReader reader(payload.data(), payload.size());
    std::vector<ui::PickerPlace> places;
    while (reader.remaining()) {
        ui::PickerPlace place;
        if (!reader.GetString(place.label) || !reader.GetString(place.directory) || place.directory.empty()) return {};
        place.glyph = kFolderGlyph;
        places.push_back(std::move(place));
    }
    return places;
}

} // namespace pulse::app
