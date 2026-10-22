#pragma once

#include "../app/app_internal.h"
#include "../app/folder_sizes_ui.h"
#include "../common/localization.h"
#include <cstdio>

inline int RunDetailsWorkerCases(pulse::AppState& s) {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failures += !passed;
    };
    auto& name_tab = *ActiveTab(s);
    {
        wchar_t cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        const auto root = std::wstring(cwd) + L"\\bench_data\\shared-size-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto file_path = root + L"\\data.bin";
        const auto offline = root + L"\\offline";
        const bool created = CreateDirectoryW(root.c_str(), nullptr) != FALSE;
        HANDLE file = created ? CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
        DWORD written = 0;
        const bool wrote = file != INVALID_HANDLE_VALUE && WriteFile(file, "abc", 3, &written, nullptr) && written == 3;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        check(wrote, "create isolated shared folder-size application fixture");
        if (wrote) {
            name_tab.current_path = fs::NormalizePath(root);
            auto selected = std::make_shared<std::vector<fs::DirEntry>>(1);
            selected->front().name = L"data.bin"; selected->front().size = 3;
            selected->front().attrs = FILE_ATTRIBUTE_NORMAL;
            name_tab.SetSnapshot(selected); name_tab.SelectOnly(0);
            ++name_tab.applied_generation;
            s.showDetailsPanel = true;
            PrepareViewState(s);
            bool received_meta = false;
            const auto meta_deadline = GetTickCount64() + 5000;
            while (!received_meta && GetTickCount64() < meta_deadline) {
                MSG message{};
                if (PeekMessageW(&message, s.hwnd, WM_DETAILS_META, WM_DETAILS_META, PM_REMOVE)) {
                    std::unique_ptr<DetailsMetaResult> meta(reinterpret_cast<DetailsMetaResult*>(message.lParam));
                    received_meta = meta && ApplyDetailsMetaResult(s, *meta);
                } else Sleep(10);
            }
            check(received_meta && s.detailsSelValid && !s.detailsTypeName.empty() && !s.detailsOwner.empty() &&
                  !s.detailsPermissions.empty(),
                  "actual COM-initialized details worker returns handle-backed metadata with matching epochs");
            s.showDetailsPanel = false;
            ui::WindowViewModel folders;
            folders.details.path = root; folders.details.is_dir = true;
            ui::PaneSlotView slot;
            slot.rect = D2D1::RectF(240, 120, 900, 550); slot.focused = true;
            slot.pane.path = std::wstring(cwd) + L"\\bench_data";
            slot.pane.is_file_system = true; slot.pane.view_mode = ui::ViewMode::Tiles;
            ui::ListEntryView entry; entry.name = L"shared-size"; entry.path = root; entry.is_dir = true;
            slot.pane.entries.push_back(std::move(entry)); folders.pane_slots.push_back(std::move(slot));
            const auto wait_size = [&](app::FolderSizeState expected) {
                const auto deadline = GetTickCount64() + 5000;
                do {
                    PrepareFolderSizes(s, folders);
                    if (s.folderSizes.Get(root).state == expected) return true;
                    Sleep(10);
                } while (GetTickCount64() < deadline);
                return false;
            };
            const bool ready = wait_size(app::FolderSizeState::Ready);
            check(ready && s.folderSizes.Get(root).bytes == 3 && s.folderSizes.Get(root).files == 1 &&
                  !folders.details.size_pending && !folders.details.contains_text.empty() &&
                  !folders.pane_slots[0].pane.folder_size_labels.at(0).text.empty(),
                  "details and visible folder row consume one shared bytes-and-counts result");
            const bool marked = CreateDirectoryW(offline.c_str(), nullptr) &&
                SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE);
            check(marked, "create unreadable-content boundary via isolated offline subtree");
            if (marked) {
                s.folderSizes.Invalidate(root);
                const bool partial_size = wait_size(app::FolderSizeState::Partial);
                const auto label = l10n::Get(l10n::StringId::FolderSizePartial);
                check(partial_size && !folders.details.size_pending &&
                      folders.details.size_text.find(label) != std::wstring::npos &&
                      folders.details.contains_text.find(label) != std::wstring::npos &&
                      folders.pane_slots[0].pane.folder_size_labels.at(0).text.find(label) != std::wstring::npos,
                      "partial bytes and counts are explicitly labelled in details and folder rows");
                auto refreshing = s.folderSizes.Get(root);
                refreshing.state = app::FolderSizeState::Updating;
                FillDetailsFolderSize(folders.details, refreshing);
                check(folders.details.size_pending && refreshing.incomplete &&
                      folders.details.contains_text.find(label) != std::wstring::npos,
                      "an incomplete count keeps its partial label throughout a slow refresh");
            }
            s.folderSizes.Stop();
            if (GetFileAttributesW(offline.c_str()) != INVALID_FILE_ATTRIBUTES) {
                SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY); RemoveDirectoryW(offline.c_str());
            }
        }
        const bool cleaned_file = file == INVALID_HANDLE_VALUE || DeleteFileW(file_path.c_str());
        check(cleaned_file && (!created || RemoveDirectoryW(root.c_str())), "shared size application fixture cleaned");
    }
    return failures ? 1 : 0;
}
