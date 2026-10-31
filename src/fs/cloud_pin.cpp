// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "cloud_pin.h"

#include "cloud_state.h"

namespace pulse::fs {
namespace {

constexpr DWORD kSettable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                            FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE |
                            FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | kAttrPinned | kAttrUnpinned;

// \\?\ form for paths past MAX_PATH; UNC and already-prefixed paths kept right.
std::wstring Extended(const std::wstring& path) {
    if (path.size() < MAX_PATH - 12 || path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

struct Item {
    std::wstring path;
    DWORD attrs = 0;
    DWORD tag = 0;
};

bool Cancelled(const std::atomic<bool>* cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

// Non-cloud reparse points (junctions, symlinks, mount points, dedup...) are not
// ours to pin and may lead outside the sync root.
bool Foreign(const Item& item) {
    return (item.attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && !IsCloudReparseTag(item.tag);
}

bool Probe(const std::wstring& path, Item& item, const CloudPinApi& api) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = api.first(Extended(path).c_str(), FindExInfoBasic, &data,
                                         FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    item.path = path;
    item.attrs = data.dwFileAttributes;
    item.tag = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? data.dwReserved0 : 0;
    return true;
}

void Failed(CloudPinResult& result, DWORD error, const std::wstring& path) {
    ++result.failed;
    if (!result.first_error) {
        result.first_error = error ? error : ERROR_GEN_FAILURE;
        result.first_failed_path = path;
    }
}

void Touch(const Item& item, CloudPinAction action, CloudPinResult& result, const CloudPinApi& api) {
    ++result.visited;
    if (!CloudPinChanges(item.attrs, action)) return;
    if (api.set_attributes(Extended(item.path).c_str(), CloudPinAttrs(item.attrs, action)))
        ++result.changed;
    else
        Failed(result, GetLastError(), item.path);
}

bool Enter(const Item& item, CloudPinAction action) {
    if (!(item.attrs & FILE_ATTRIBUTE_DIRECTORY) || Foreign(item)) return false;
    if (action == CloudPinAction::Free && IsCloudPlaceholder(item.attrs)) return false;
    return true;
}

} // namespace

DWORD CloudPinAttrs(DWORD attrs, CloudPinAction action) noexcept {
    DWORD out = attrs & kSettable;
    if (action == CloudPinAction::Keep)
        out = (out & ~kAttrUnpinned) | kAttrPinned;
    else
        out = (out & ~kAttrPinned) | kAttrUnpinned;
    return out ? out : FILE_ATTRIBUTE_NORMAL;
}

bool CloudPinChanges(DWORD attrs, CloudPinAction action) noexcept {
    if (action == CloudPinAction::Keep)
        return (attrs & kAttrPinned) == 0 || (attrs & kAttrUnpinned) != 0;
    return (attrs & kAttrUnpinned) == 0 || (attrs & kAttrPinned) != 0;
}

CloudPinResult ApplyCloudPin(const std::vector<std::wstring>& paths, CloudPinAction action,
                             const std::atomic<bool>* cancel, const CloudPinApi& api) {
    CloudPinResult result;
    std::vector<std::wstring> folders; // depth-first, parent already written
    for (const auto& raw : paths) {
        if (Cancelled(cancel)) { result.cancelled = true; return result; }
        std::wstring path = raw;
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        Item item;
        if (!Probe(path, item, api)) { Failed(result, GetLastError(), path); continue; }
        if (Foreign(item)) continue;
        Touch(item, action, result, api);
        if (Enter(item, action)) folders.push_back(std::move(path));
        while (!folders.empty()) {
            if (Cancelled(cancel)) { result.cancelled = true; return result; }
            const std::wstring folder = std::move(folders.back());
            folders.pop_back();
            WIN32_FIND_DATAW data{};
            const HANDLE find = api.first(Extended(folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                                 FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES) {
                    const DWORD attrs = GetFileAttributesW(Extended(folder).c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                        Failed(result, attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY, folder);
                } else Failed(result, error, folder);
                continue;
            }
            do {
                if (data.cFileName[0] == L'.' &&
                    (data.cFileName[1] == 0 || (data.cFileName[1] == L'.' && data.cFileName[2] == 0)))
                    continue;
                Item child;
                child.path = folder + L"\\" + data.cFileName;
                child.attrs = data.dwFileAttributes;
                child.tag = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? data.dwReserved0 : 0;
                if (Foreign(child)) continue;
                Touch(child, action, result, api);
                if (Enter(child, action)) folders.push_back(std::move(child.path));
                if (Cancelled(cancel)) break;
            } while (api.next(find, &data));
            const DWORD error = GetLastError();
            FindClose(find);
            if (Cancelled(cancel)) { result.cancelled = true; return result; }
            if (error != ERROR_NO_MORE_FILES) Failed(result, error, folder);
        }
    }
    return result;
}

} // namespace pulse::fs
