#include "../ops/image_open.cpp"
#include <cstdio>

int wmain() {
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
        if (!passed) ++failures;
    };
    {
        using namespace pulse::ops;
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        PIDLIST_ABSOLUTE raw = nullptr;
        HRESULT hr = SHParseDisplayName(executable, nullptr, &raw, 0, nullptr);
        Pidl target(raw);
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
    OleUninitialize();
    return failures ? 1 : 0;
}
