// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "content_hit_aliases.h"
#include "content_scope.h"
#include "document_reader.h"
#include <algorithm>

namespace pulse::index {
namespace {
std::wstring DisplayPath(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\") || path.starts_with(L"\\\\?\\unc\\")) path.replace(0, 8, L"\\\\");
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return path;
}
std::wstring Key(const std::wstring& path) { return ContentScopeKey(DisplayPath(path)); }
std::wstring Native(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\")) return path;
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}
bool Under(const std::wstring& path, const std::wstring& root, bool recursive = true) {
    const auto key = Key(path), base = Key(root);
    return ContentPathUnder(key, base) && (recursive ||
        key.find(L'\\', base.size() + (base.ends_with(L'\\') ? 0 : 1)) == std::wstring::npos);
}
std::wstring Extension(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
    return dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? L"" : ContentScopeKey(path.substr(dot));
}
bool SystemPath(const std::wstring& path) {
    const auto key = Key(path);
    if (key.size() < 3 || key[1] != L':') return false;
    const auto top = key.substr(3, key.find(L'\\', 3) == std::wstring::npos ? std::wstring::npos : key.find(L'\\', 3) - 3);
    return top == L"windows" || top == L"$recycle.bin" || top == L"system volume information" ||
        top == L"pagefile.sys" || top == L"hiberfil.sys" || top == L"swapfile.sys";
}
uint64_t Stamp(FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; }
struct Cursor {
    HANDLE source = INVALID_HANDLE_VALUE, names = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_BASIC_INFO basic{};
    std::vector<wchar_t> buffer;
    std::wstring volume;
    bool first = true;
    ~Cursor() {
        if (names != INVALID_HANDLE_VALUE) FindClose(names);
        if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    }
    bool Open(const ContentHit& hit) {
        const auto native = Native(hit.path);
        source = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (source == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(source, &identity) || identity.nNumberOfLinks < 2 ||
            ((uint64_t(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow) != hit.size || Stamp(identity.ftLastWriteTime) != hit.modified ||
            !GetFileInformationByHandleEx(source, FileBasicInfo, &basic, sizeof(basic))) return false;
        buffer.resize(32768);
        if (!GetVolumePathNameW(native.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()))) return false;
        volume = DisplayPath(buffer.data());
        DWORD length = static_cast<DWORD>(buffer.size());
        names = FindFirstFileNameW(native.c_str(), 0, &length, buffer.data());
        return names != INVALID_HANDLE_VALUE;
    }
    bool Next(std::wstring& path) {
        if (!first) {
            DWORD length = static_cast<DWORD>(buffer.size());
            if (!FindNextFileNameW(names, &length, buffer.data())) return false;
        }
        first = false;
        path = volume;
        if (!path.ends_with(L'\\')) path.push_back(L'\\');
        path.append(buffer.data() + (buffer[0] == L'\\' ? 1 : 0));
        return true;
    }
    bool SameFile(const std::wstring& path) const {
        HANDLE file = CreateFileW(Native(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info{}; FILE_BASIC_INFO current{};
        constexpr DWORD unsafe = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE |
            FILE_ATTRIBUTE_OFFLINE | 0x00040000 | 0x00400000;
        const bool same = GetFileInformationByHandle(file, &info) && GetFileInformationByHandleEx(file, FileBasicInfo, &current, sizeof(current)) &&
            !(info.dwFileAttributes & unsafe) && info.dwVolumeSerialNumber == identity.dwVolumeSerialNumber &&
            info.nFileIndexHigh == identity.nFileIndexHigh && info.nFileIndexLow == identity.nFileIndexLow &&
            info.nFileSizeHigh == identity.nFileSizeHigh && info.nFileSizeLow == identity.nFileSizeLow &&
            Stamp(info.ftLastWriteTime) == Stamp(identity.ftLastWriteTime) && current.ChangeTime.QuadPart == basic.ChangeTime.QuadPart;
        CloseHandle(file);
        return same;
    }
};
}
ContentHitAliases::ContentHitAliases(const ContentIndexConfig& config, const ContentSearchRequest& request,
    std::function<bool()> cancelled, std::function<bool(const std::wstring&)> excluded)
    : config_(config), request_(request), cancelled_(std::move(cancelled)), excluded_(std::move(excluded)), filename_(ParseQuery(request.filename_query)) {}
bool ContentHitAliases::Allowed(const std::wstring& path, const ContentHit& source) const {
    if ((!request_.candidate_paths.empty() && std::none_of(request_.candidate_paths.begin(), request_.candidate_paths.end(),
            [&](const auto& candidate) { return Key(candidate) == Key(path); })) || excluded_(path) ||
        (request_.skip_system_locations && SystemPath(path)) ||
        !ContentRequestIncludesPath(request_, path)) return false;
    const auto root = std::find_if(config_.roots.begin(), config_.roots.end(), [&](const auto& value) { return Under(path, value.path); });
    const auto original = std::find_if(config_.roots.begin(), config_.roots.end(), [&](const auto& value) { return Under(source.path, value.path); });
    const auto extension = Extension(path);
    if (root == config_.roots.end() || original == config_.roots.end() || root->encoding != original->encoding ||
        extension != Extension(source.path) || !IsIndexedContentExtension(extension)) return false;
    const bool document = IsExtractedDocumentExtension(extension);
    const auto maximum = (std::min)(document ? config_.maximum_document_bytes : config_.maximum_file_bytes,
        document ? request_.maximum_document_bytes : request_.maximum_file_bytes);
    return source.size >= request_.minimum_file_bytes && source.size <= maximum &&
        MatchContentFilename(path, source.size, source.modified, filename_);
}
bool ContentHitAliases::Emit(const ContentHit& source, const std::function<bool(ContentHit)>& publish) {
    if (cancelled_()) return false;
    if (emitted_.insert(Key(source.path)).second && !publish(source)) return false;
    if (cancelled_()) return false;
    Cursor cursor;
    try { if (!cursor.Open(source)) return true; }
    catch (...) { return !cancelled_(); }
    for (;;) {
        ContentHit alias;
        bool available = false;
        try {
            std::wstring path;
            while (!cancelled_() && cursor.Next(path)) {
                if (emitted_.contains(Key(path)) || !Allowed(path, source) || !cursor.SameFile(path)) continue;
                alias = source; alias.path = std::move(path);
                alias.name = alias.path.substr(alias.path.find_last_of(L"\\/") + 1);
                alias.file_id = 0;
                available = true; break;
            }
        } catch (...) { return !cancelled_(); }
        if (cancelled_()) return false;
        if (!available) return true;
        // Callback exceptions propagate; optional enumeration failures above do not.
        if (emitted_.insert(Key(alias.path)).second && !publish(std::move(alias))) return false;
    }
}
}
