#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <unordered_set>
namespace pulse::ops {
inline std::wstring ListingPattern(std::wstring folder) {
    if (folder.empty()) return {};
    if (folder.back() != L'\\') folder += L'\\';
    folder += L'*';
    if (folder.size() < MAX_PATH || folder.starts_with(L"\\\\?\\")) return folder;
    if (folder.find(L'/') != std::wstring::npos || folder.find(L"\\.\\") != std::wstring::npos ||
        folder.find(L"\\..\\") != std::wstring::npos)
        return {};
    if (folder.size() > 3 && folder[1] == L':' && folder[2] == L'\\') return L"\\\\?\\" + folder;
    if (folder.size() > 3 && folder[0] == L'\\' && folder[1] == L'\\' && folder[2] != L'.' && folder[2] != L'?' &&
        folder[2] != L'\\')
        return L"\\\\?\\UNC\\" + folder.substr(2);
    return {};
}

// Which names a destination folder already has, from one listing, for a
// transfer of many items into it. Over a network each query of a name is a
// round trip; the listing returns hundreds of names per trip. Only absence
// is answered, and only for names made of ASCII and of characters without
// case, which every file system compares alike. Names are folded with the
// file-system upper-case rules (LCMapStringEx without linguistic casing), so
// a listed name that the file system takes for an ASCII spelling still
// matches it. A name that may be an 8.3 alias (a '~'), or that Win32
// would trim (a trailing dot or space), is left to the caller, and so is any
// name the listing shows: the caller confirms it, which also keeps
// case-sensitive folders right.
class TransferTargetListing {
public:
    TransferTargetListing(const std::wstring& folder, size_t items) {
        if (items < 16) return;
        const std::wstring pattern = ListingPattern(folder);
        if (pattern.empty()) return;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoStandard, &data, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return;
        // A huge folder receiving a few items: past this many names, asking
        // about each item is cheaper.
        size_t budget = (std::max)(size_t{4096}, items * 64);
        bool complete = false;
        for (;;) {
            names_.insert(Map(data.cFileName, LCMAP_UPPERCASE));
            // Custom DOS aliases need not contain '~'. Include aliases so an
            // existing destination cannot become an absence hint through one.
            if (data.cAlternateFileName[0]) names_.insert(Map(data.cAlternateFileName, LCMAP_UPPERCASE));
            if (--budget == 0) break;
            if (!FindNextFileW(find, &data)) {
                complete = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        }
        FindClose(find);
        if (!complete) names_.clear();
        valid_ = complete;
    }

    // True: absent in this listing snapshot. Every mutation still checks the
    // destination guard and live collision state. False: ask the file system.
    bool SurelyAbsent(const std::wstring& name) const {
        if (!valid_ || name.empty() || name.back() == L'.' || name.back() == L' ' ||
            name.find_first_of(L"~:") != std::wstring::npos)
            return false;
        const std::wstring upper = Map(name, LCMAP_UPPERCASE);
        const std::wstring lower = Map(name, LCMAP_LOWERCASE);
        if (upper.size() != name.size() || lower.size() != name.size()) return false;
        for (size_t i = 0; i < name.size(); ++i)
            if (name[i] >= 0x80 && (upper[i] != name[i] || lower[i] != name[i])) return false;
        return !names_.contains(upper);
    }

    // `name` has since arrived in the folder (this transfer put it there).
    void Added(const std::wstring& name) {
        if (valid_) names_.insert(Map(name, LCMAP_UPPERCASE));
    }

private:
    static std::wstring Map(const std::wstring& name, DWORD flags) {
        if (name.empty()) return {};
        std::wstring out(name.size(), L'\0');
        const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, name.c_str(), static_cast<int>(name.size()),
                                       out.data(), static_cast<int>(out.size()), nullptr, nullptr, 0);
        // Unmappable: a key no query can equal, and a name no query trusts.
        if (size <= 0) return std::wstring(1, L'\0') + name;
        out.resize(static_cast<size_t>(size));
        return out;
    }

    bool valid_ = false;
    std::unordered_set<std::wstring> names_;
};

}  // namespace pulse::ops
