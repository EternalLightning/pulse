// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ops/image_open.cpp"
#include "../ipc/shell_launch_protocol.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && wcscmp(argv[1], L"--viewer-launcher") == 0) return 0;
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    {
        using namespace pulse;
        using namespace pulse::ops;
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(ARRAYSIZE(temp), temp);
        const auto root = std::filesystem::path(temp) /
            (L"Pulse-image-order-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        const bool created = std::filesystem::create_directory(root, error);
        check(created, "Create an isolated image order fixture");
        if (created) {
            for (const auto* name : {L"a.jpg", L"b.jpg", L"c.jpg", L"hidden.jpg"}) {
                std::ofstream(root / name).put('x');
            }
            const auto selected_path = (root / L"b.jpg").wstring();
            PIDLIST_ABSOLUTE raw = nullptr;
            const HRESULT parsed = SHParseDisplayName(selected_path.c_str(), nullptr, &raw, 0, nullptr);
            Pidl target(raw);
            const auto verify = [&](ImageOpenContext context, const std::vector<std::wstring>& expected,
                                    REFPROPERTYKEY key, SORTDIRECTION direction, const char* label) {
                ipc::ShellLaunchRequest request;
                request.paths = {selected_path};
                request.image_context = std::move(context);
                const auto payload = ipc::WriteShellLaunch(request);
                ipc::ShellLaunchRequest decoded;
                bool valid = SUCCEEDED(parsed) && ipc::ReadShellLaunch(payload.data(), payload.size(), decoded);
                auto contents = std::make_shared<FolderContents>();
                valid = valid && PrepareContents(target.get(), *contents);
                contents->context = decoded.image_context;
                const auto site = Make<ImageOpenSite>(nullptr, contents);
                int count = -1;
                valid = valid && SUCCEEDED(site->ItemCount(SVGIO_ALLVIEW, &count)) &&
                    count == static_cast<int>(expected.size());
                ComPtr<IEnumIDList> enumerator;
                valid = valid && SUCCEEDED(site->Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER,
                    IID_PPV_ARGS(&enumerator)));
                for (const auto& name : expected) {
                    PITEMID_CHILD child_raw = nullptr;
                    if (!enumerator || enumerator->Next(1, &child_raw, nullptr) != S_OK) { valid = false; break; }
                    Pidl child(child_raw);
                    ComPtr<IShellItem> item;
                    PWSTR filename = nullptr;
                    const HRESULT item_hr = SHCreateItemWithParent(contents->folder.get(), nullptr,
                        child.get(), IID_PPV_ARGS(&item));
                    const HRESULT name_hr = SUCCEEDED(item_hr) ? item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &filename) : item_hr;
                    valid = valid && SUCCEEDED(name_hr) && filename && name == filename;
                    CoTaskMemFree(filename);
                }
                SORTCOLUMN sort{};
                int focus = -1;
                site->GetFocusedItem(&focus);
                valid = valid && SUCCEEDED(site->GetSortColumns(&sort, 1)) &&
                    IsEqualPropertyKey(sort.propkey, key) && sort.direction == direction &&
                    focus >= 0 && focus < count && contents->MatchesSelected(contents->items[focus].pidl.get());
                UINT columns = 0;
                std::vector<PROPERTYKEY> keys(2);
                valid = valid && SUCCEEDED(site->GetColumnCount(CM_ENUM_VISIBLE, &columns)) &&
                    SUCCEEDED(site->GetColumns(CM_ENUM_VISIBLE, keys.data(), columns));
                bool includes_sort = false;
                for (UINT i = 0; i < columns; ++i) includes_sort |= IsEqualPropertyKey(keys[i], sort.propkey);
                check(valid && includes_sort, label);
            };
            ImageOpenContext descending;
            descending.paths = {(root / L"c.jpg").wstring(), selected_path, (root / L"a.jpg").wstring()};
            descending.direction = model::SortDirection::Desc;
            verify(descending, {L"c.jpg", L"b.jpg", L"a.jpg"}, PKEY_ItemNameDisplay, SORT_DESCENDING,
                "Name descending survives IPC and enumerates neighbors in visible order");
            ImageOpenContext modified;
            modified.paths = {(root / L"a.jpg").wstring(), (root / L"c.jpg").wstring(), selected_path};
            modified.column = model::SortColumn::Mtime;
            modified.direction = model::SortDirection::Desc;
            verify(modified, {L"a.jpg", L"c.jpg", L"b.jpg"}, PKEY_DateModified, SORT_DESCENDING,
                "Date ordering remains distinct from name order and reports DateModified descending");
            ImageOpenContext filtered;
            filtered.paths = {(root / L"c.jpg").wstring(), selected_path};
            filtered.direction = model::SortDirection::Desc;
            verify(filtered, {L"c.jpg", L"b.jpg"}, PKEY_ItemNameDisplay, SORT_DESCENDING,
                "Filtered neighbors do not reintroduce other existing files");
            verify({}, {L"a.jpg", L"b.jpg", L"c.jpg", L"hidden.jpg"}, PKEY_ItemNameDisplay, SORT_ASCENDING,
                "An external open without view context retains Shell name order fallback");
            ipc::ShellLaunchRequest invalid;
            invalid.paths = {selected_path};
            invalid.image_context.paths = {std::wstring(L"bad\0name", 8)};
            auto payload = ipc::WriteShellLaunch(invalid);
            ipc::ShellLaunchRequest decoded;
            check(!ipc::ReadShellLaunch(payload.data(), payload.size(), decoded), "Reject embedded NUL in image context paths");
            invalid.image_context = descending;
            invalid.image_context.column = static_cast<model::SortColumn>(99);
            payload = ipc::WriteShellLaunch(invalid);
            check(!ipc::ReadShellLaunch(payload.data(), payload.size(), decoded), "Reject unknown image context sort columns");
            invalid.image_context = descending;
            invalid.verb = L"properties";
            payload = ipc::WriteShellLaunch(invalid);
            check(!ipc::ReadShellLaunch(payload.data(), payload.size(), decoded), "Reject view context on non-open shell requests");
            std::filesystem::remove_all(root, error);
            check(!error, "Remove the isolated image order fixture");
        }
    }
    {
        using namespace pulse::ops;
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        PIDLIST_ABSOLUTE raw = nullptr;
        HRESULT hr = SHParseDisplayName(executable, nullptr, &raw, 0, nullptr);
        Pidl target(raw);
        if (SUCCEEDED(hr)) {
            auto lazy = std::make_shared<FolderContents>();
            check(PrepareContents(target.get(), *lazy), "Prepare activation without enumerating siblings");
            const auto lazy_site = Make<ImageOpenSite>(nullptr, lazy);
            int selected_count = 0;
            ComPtr<IShellItemArray> selection;
            const HRESULT selection_hr = lazy_site->GetSelection(FALSE, &selection);
            lazy_site->ItemCount(SVGIO_SELECTION, &selected_count);
            check(SUCCEEDED(selection_hr) && selected_count == 1 && lazy->items.empty() &&
                lazy->read_result == E_PENDING, "Selection requests do not load the folder view");
            int count = 0;
            const HRESULT loaded = lazy_site->ItemCount(SVGIO_ALLVIEW, &count);
            int focus = -1;
            lazy_site->GetFocusedItem(&focus);
            PITEMID_CHILD focused_raw = nullptr;
            const HRESULT focused_hr = lazy_site->Item(focus, &focused_raw);
            Pidl focused_item(focused_raw);
            check(SUCCEEDED(loaded) && count > 0 && SUCCEEDED(focused_hr) &&
                lazy->MatchesSelected(focused_item.get()), "Lazy folder view preserves the selected image index");
            const auto* snapshot = lazy->items.data();
            int repeated_count = 0;
            check(SUCCEEDED(lazy_site->ItemCount(SVGIO_ALLVIEW, &repeated_count)) &&
                repeated_count == count && lazy->items.data() == snapshot,
                "Repeated folder requests reuse the same snapshot");
        }
        auto contents = std::make_shared<FolderContents>();
        if (SUCCEEDED(hr)) hr = ReadContents(target.get(), *contents, nullptr);
        check(SUCCEEDED(hr), "Load an isolated Shell folder model");
        if (SUCCEEDED(hr)) {
            contents->focused = 0;
            const auto site = Make<ImageOpenSite>(nullptr, contents);
            ComPtr<IFolderView2> view;
            hr = site->QueryService(SID_SFolderView, IID_PPV_ARGS(&view));
            check(SUCCEEDED(hr), "Activation service exposes IFolderView2");
            ComPtr<IPersistIDList> identity;
            hr = view.As(&identity);
            PIDLIST_ABSOLUTE folder_raw = nullptr;
            if (SUCCEEDED(hr)) hr = identity->GetIDList(&folder_raw);
            Pidl folder(folder_raw);
            check(SUCCEEDED(hr) && folder && folder.get() != contents->folder.get() &&
                ILIsEqual(folder.get(), contents->folder.get()), "Folder identity returns an owned parent PIDL");
            ComPtr<IColumnManager> columns;
            hr = view.As(&columns);
            UINT count = 0;
            PROPERTYKEY key{};
            if (SUCCEEDED(hr)) hr = columns->GetColumnCount(CM_ENUM_VISIBLE, &count);
            if (SUCCEEDED(hr)) hr = columns->GetColumns(CM_ENUM_VISIBLE, &key, count);
            SORTCOLUMN sort{};
            int sort_count = 0;
            if (SUCCEEDED(hr)) hr = view->GetSortColumnCount(&sort_count);
            if (SUCCEEDED(hr)) hr = view->GetSortColumns(&sort, sort_count);
            check(SUCCEEDED(hr) && count == 1 && sort_count == 1 &&
                IsEqualPropertyKey(key, sort.propkey), "Visible columns agree with the activation sort order");
            CM_COLUMNINFO info{};
            info.cbSize = sizeof(info);
            info.dwMask = CM_MASK_STATE | CM_MASK_WIDTH | CM_MASK_NAME;
            hr = columns ? columns->GetColumnInfo(key, &info) : E_NOINTERFACE;
            check(SUCCEEDED(hr) && (info.dwState & CM_STATE_VISIBLE) && info.uWidth != 0 &&
                info.wszName[0], "Shell can serialize the visible column metadata");
            int selected = -1;
            hr = view->GetSelectedItem(0, &selected);
            check(hr == S_OK && selected == 0, "First image remains selected when enumeration starts at zero");
            selected = 99;
            hr = view->GetSelectedItem(1, &selected);
            check(hr == S_FALSE && selected == -1, "Selection enumeration terminates after the selected item");
            UINT invalid_count = 99;
            check(columns && columns->GetColumnCount(static_cast<CM_ENUM_FLAGS>(0), &invalid_count) == E_INVALIDARG &&
                invalid_count == 0 && columns->GetColumns(CM_ENUM_VISIBLE, &key, 0) == E_INVALIDARG &&
                identity->GetIDList(nullptr) == E_POINTER, "Reject malformed Shell serialization requests");
        }
    }
    {
        using namespace pulse::ops;
        auto lifetime = std::make_shared<int>(0);
        std::weak_ptr<void> observed = lifetime;
        auto contents = std::make_shared<FolderContents>();
        contents->lifetime = lifetime;
        auto site = Make<ImageOpenSite>(nullptr, contents);
        ComPtr<IFolderView2> view;
        const HRESULT result = site ? site->QueryService(SID_SFolderView, IID_PPV_ARGS(&view)) : E_OUTOFMEMORY;
        lifetime.reset();
        contents.reset();
        site.Reset();
        check(SUCCEEDED(result) && !observed.expired(), "Shell view retains the launch lifetime after handoff");
        view.Reset();
        check(observed.expired(), "Releasing the final Shell view releases the launch lifetime");
    }
    {
        using namespace pulse::ops;
        ImageOpenSession empty;
        check(!empty.ViewerRunning(), "An unused image session does not retain a launch worker");
        const std::wstring name = L"Pulse-image-lifetime-" + std::to_wstring(GetCurrentProcessId()) + L".jpg";
        ViewerWindowTracker tracker(name);
        check(tracker.CaptureBeforeLaunch(), "Capture windows before image activation");
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --viewer-launcher";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        const bool launched = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
        bool exited = false;
        if (launched) {
            CloseHandle(child.hThread);
            exited = WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0;
            if (!exited) { TerminateProcess(child.hProcess, ERROR_CANCELLED); WaitForSingleObject(child.hProcess, 5000); }
            tracker.SetLauncher(child.hProcess);
        }
        check(launched && exited && tracker.Active(), "A finished activation launcher does not retire the media session");
        HWND viewer = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", name.c_str(),
            WS_POPUP | WS_VISIBLE, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(viewer && tracker.Active(), "Track the actual viewer window after the launcher exits");
        if (viewer) {
            SetWindowTextW(viewer, L"Neighbor-video.mp4");
            check(tracker.Active(), "Switching from an image to a video preserves the viewer session");
            SetWindowTextW(viewer, L"Another-image.jpg");
            ShowWindow(viewer, SW_HIDE);
            check(tracker.Active(), "Changing images and hiding the viewer do not retire its context");
            DestroyWindow(viewer);
            check(!tracker.Active(), "Closing the viewer releases its image session");
        }
    }
    {
        using namespace pulse::ops;
        const std::wstring name = L"Pulse-existing-viewer-" + std::to_wstring(GetCurrentProcessId()) + L".jpg";
        HWND previous = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", name.c_str(),
            WS_POPUP | WS_VISIBLE, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ViewerWindowTracker tracker(name);
        const bool captured = previous && tracker.CaptureBeforeLaunch();
        tracker.SetLauncher(OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId()));
        tracker.Active();
        if (previous) DestroyWindow(previous);
        check(captured && tracker.Active(), "An unchanged background window with the same filename cannot own a new session");
        HWND current = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", name.c_str(),
            WS_POPUP | WS_VISIBLE, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(current && tracker.Active(), "A new matching viewer can own the pending session");
        if (current) DestroyWindow(current);
        check(!tracker.Active(), "The replacement viewer is reclaimed independently of the old window");
    }
    OleUninitialize();
    return failures ? 1 : 0;
}
