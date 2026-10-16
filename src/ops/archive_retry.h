#pragma once

#include "archive.h"
#include "destination_guard.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include <map>
#include <set>

namespace pulse::ops {

// Worker-only journal. It grants authority only for an extractor-reported file
// whose identity and final metadata still match. Unknown crash outputs stay put.
class ArchiveRetry {
public:
    static constexpr wchar_t kName[] = L".pulse-extract-state";
    ~ArchiveRetry() {
        if (file_ != INVALID_HANDLE_VALUE) {
            if (valid_ && items_.empty()) {
                FILE_DISPOSITION_INFO remove{TRUE};
                SetFileInformationByHandle(file_, FileDispositionInfo, &remove, sizeof(remove));
            }
            CloseHandle(file_);
        }
    }

    bool Open(const std::filesystem::path& target, const std::wstring& source, std::wstring& error) {
        target_ = target;
        source_ = std::filesystem::absolute(source).lexically_normal().wstring();
        DestinationGuard::Handle directory(CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!directory || directory.get() == INVALID_HANDLE_VALUE) {
            if (directory.get() == INVALID_HANDLE_VALUE) directory.release();
            return Fail(error);
        }
        target_id_ = Identity(directory.get());
        if (target_id_.empty()) return Fail(error);
        file_ = CreateFileW((target / kName).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool created = file_ != INVALID_HANDLE_VALUE;
        if (!created && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS))
            file_ = CreateFileW((target / kName).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return Fail(error);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file_, &info) || info.nNumberOfLinks != 1 ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return Fail(error);
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart > 32 * 1024 * 1024) return Fail(error);
        if (!size.QuadPart) {
            valid_ = created && Save(error);
            return valid_ || Fail(error);
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(file_, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) || got != bytes.size()) return Fail(error);
        // Only complete, flushed lines can grant retry authority. A torn final
        // append retains the previous conservative snapshot.
        auto end = std::find(bytes.rbegin(), bytes.rend(), static_cast<uint8_t>('\n'));
        if (end == bytes.rend()) return Fail(error);
        const size_t last = bytes.size() - static_cast<size_t>(end - bytes.rbegin());
        size_t begin = last - 1;
        while (begin && bytes[begin - 1] != '\n') --begin;
        std::vector<uint8_t> line(bytes.begin() + begin, bytes.begin() + last);
        std::wstring text;
        if (!DecodeUtf8Bytes(line, text)) return Fail(error);
        json::Document doc(text);
        if (!doc.Is(json::Kind::Object) || json::ExtractString(doc, L"format") != L"PulseExtraction1" ||
            json::ExtractString(doc, L"target") != target_id_ ||
            !path::EqualInsensitive(json::ExtractString(doc, L"source"), source_)) return Fail(error);
        const auto* items = doc.Find(L"items");
        if (!items || items->kind != json::Kind::Array) return Fail(error);
        for (const auto& value : items->children) {
            Item item;
            const auto name = json::String(value.Find(L"name"));
            if (value.kind != json::Kind::Object || !IsSafeArchiveMember(name) || path::EqualInsensitive(name, kName)) return Fail(error);
            item.proof = json::String(value.Find(L"proof"));
            if (!items_.emplace(name, std::move(item)).second) return Fail(error);
        }
        LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(last);
        if (!SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) return Fail(error);
        valid_ = true;
        return true;
    }

    bool Recover(const std::vector<ArchiveEntry>& selected, DestinationGuard& guard, std::wstring& error) {
        // Validate the entire retry before removing any confirmed residual.
        std::vector<DestinationGuard::Handle> handles;
        for (const auto& entry : selected) {
            if (entry.is_dir) continue;
            const auto found = items_.find(entry.path);
            if (found == items_.end()) continue;
            const auto output = target_ / entry.path;
            if (!guard.Capture(output.wstring(), error) ||
                !guard.PinDirectories(output.parent_path().wstring(), error) ||
                std::any_of(guard.Impacts().begin(), guard.Impacts().end(), [](const auto& impact) {
                    return impact.kind != DestinationLinkKind::HardLink;
                })) return Fail(error);
            HANDLE raw = CreateFileW(output.c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (raw == INVALID_HANDLE_VALUE) {
                const DWORD code = GetLastError();
                if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) continue;
                return Fail(error);
            }
            DestinationGuard::Handle handle(raw);
            if (found->second.proof.empty() || Proof(raw) != found->second.proof) {
                error = L"上次解压文件已变化或归属无法确认，未覆盖：" + output.wstring();
                return false;
            }
            handles.push_back(std::move(handle));
        }
        for (const auto& handle : handles) {
            FILE_BASIC_INFO basic{};
            if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic))) return Fail(error);
            if (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) {
                FILE_BASIC_INFO writable{};
                writable.FileAttributes = basic.FileAttributes & ~FILE_ATTRIBUTE_READONLY;
                if (!writable.FileAttributes) writable.FileAttributes = FILE_ATTRIBUTE_NORMAL;
                if (!SetFileInformationByHandle(handle.get(), FileBasicInfo, &writable, sizeof(writable))) return Fail(error);
            }
            FILE_DISPOSITION_INFO remove{TRUE};
            if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &remove, sizeof(remove))) return Fail(error);
        }
        handles.clear();
        for (const auto& entry : selected) if (!entry.is_dir) guard.ForgetLeaf((target_ / entry.path).wstring());
        return true;
    }

    bool Begin(const std::vector<ArchiveEntry>& pending, std::wstring& error) {
        active_.clear();
        for (const auto& entry : pending) if (!entry.is_dir) {
            active_.insert(entry.path);
            items_[entry.path].proof.clear();
        }
        return Save(error);
    }

    void Feed(std::string_view chunk) {
        for (char c : chunk) {
            if (c == '\r' || c == '\n') {
                if (line_.starts_with("- ")) {
                    std::vector<uint8_t> bytes(line_.begin() + 2, line_.end());
                    std::wstring name;
                    if (DecodeUtf8Bytes(bytes, name)) {
                        std::replace(name.begin(), name.end(), L'\\', L'/');
                        if (active_.contains(name)) reported_.insert(std::move(name));
                    }
                }
                line_.clear();
            } else if (line_.size() < 131072) line_ += c;
        }
        for (auto it = reported_.begin(); it != reported_.end();) {
            auto& item = items_[*it];
            if (item.handle) { it = reported_.erase(it); continue; }
            HANDLE raw = CreateFileW((target_ / *it).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (raw != INVALID_HANDLE_VALUE) { item.handle.reset(raw); it = reported_.erase(it); }
            else ++it;
        }
    }

    bool Finish(bool success, std::wstring& error) {
        for (const auto& name : active_) {
            auto found = items_.find(name);
            if (success) items_.erase(found);
            else if (found->second.handle) found->second.proof = Proof(found->second.handle.get());
        }
        if (!Save(error)) return false;
        if (items_.empty()) {
            FILE_DISPOSITION_INFO remove{TRUE};
            if (!SetFileInformationByHandle(file_, FileDispositionInfo, &remove, sizeof(remove))) return Fail(error);
        }
        return true;
    }

private:
    struct Item { std::wstring proof; DestinationGuard::Handle handle; };
    static std::wstring Identity(HANDLE handle) {
        FileIdentity identity;
        if (!ReadFileIdentity(handle, identity) || !identity.Valid()) return {};
        std::wstring text = std::to_wstring(identity.extended) + L":" + std::to_wstring(identity.value.VolumeSerialNumber);
        for (BYTE byte : identity.value.FileId.Identifier) text += L":" + std::to_wstring(byte);
        return text;
    }
    static std::wstring Proof(HANDLE handle) {
        FileIdentity identity;
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        if (!ReadFileIdentity(handle, identity) || !identity.Valid() ||
            !GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) return {};
        std::wstring proof = std::to_wstring(identity.extended) + L":" + std::to_wstring(identity.value.VolumeSerialNumber);
        for (BYTE byte : identity.value.FileId.Identifier) proof += L":" + std::to_wstring(byte);
        return proof + L":" + std::to_wstring(info.nFileSizeHigh) + L":" + std::to_wstring(info.nFileSizeLow) +
            L":" + std::to_wstring(basic.LastWriteTime.QuadPart) + L":" + std::to_wstring(basic.ChangeTime.QuadPart);
    }
    bool Save(std::wstring& error) {
        std::wstring text = L"{\"format\":\"PulseExtraction1\",\"target\":\"" + target_id_ + L"\",\"source\":\"";
        json::Escape(source_, text);
        text += L"\",\"items\":[";
        bool first = true;
        for (const auto& [name, item] : items_) {
            if (!first) text += L',';
            first = false;
            text += L"{\"name\":\""; json::Escape(name, text);
            text += L"\",\"proof\":\""; json::Escape(item.proof, text); text += L"\"}";
        }
        text += L"]}\n";
        std::vector<uint8_t> bytes;
        if (!EncodeUtf8Bytes(text, bytes) || bytes.size() > 16 * 1024 * 1024) return Fail(error);
        DWORD written = 0;
        return (WriteFile(file_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(file_)) || Fail(error);
    }
    static bool Fail(std::wstring& error) {
        error = L"无法安全读取或保存未完成解压记录，操作已停止。";
        return false;
    }
    HANDLE file_ = INVALID_HANDLE_VALUE;
    bool valid_ = false;
    std::filesystem::path target_;
    std::wstring source_, target_id_;
    std::map<std::wstring, Item> items_;
    std::set<std::wstring> active_, reported_;
    std::string line_;
};
}
