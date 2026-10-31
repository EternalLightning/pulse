// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

#include <windows.h>
#include <sddl.h>

#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::sysctx::exchange {

enum class Phase { Ready, Running, Complete };
enum class Confirmation { Complete, Rejected, Unknown };
inline constexpr DWORD kUnknownExit = 3;

inline bool MatchesExit(DWORD exit_code, bool all_ok) {
    return exit_code <= 1 && ((exit_code == 0) == all_ok);
}

inline bool ValidToken(std::wstring_view token) {
    if (token.size() != 32) return false;
    for (const auto c : token)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

inline std::wstring Header(std::wstring_view token, Phase phase) {
    return L"PULSE-SYSCTX-EXCHANGE 1\n" + std::wstring(token) + L"\n" +
        (phase == Phase::Ready ? L"ready\n" : phase == Phase::Running ? L"running\n" : L"complete\n");
}

inline bool Write(HANDLE file, std::wstring_view token, Phase phase, std::wstring_view body = {}) {
    if (!ValidToken(token) || (phase != Phase::Complete && !body.empty())) return false;
    const std::wstring text = Header(token, phase) + std::wstring(body);
    if (text.size() > 16384) return false;
    std::string bytes;
    bytes.reserve(text.size());
    for (const auto c : text) {
        if (c > 127) return false;
        bytes.push_back(static_cast<char>(c));
    }
    LARGE_INTEGER zero{};
    DWORD written = 0;
    return SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && SetEndOfFile(file) && FlushFileBuffers(file);
}

inline bool Read(HANDLE file, std::wstring_view token, Phase& phase, std::wstring& body) {
    body.clear();
    LARGE_INTEGER size{}, zero{};
    if (!ValidToken(token) || !GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16384 ||
        !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
        return false;
    std::wstring text;
    for (const unsigned char c : bytes) {
        if (c > 127) return false;
        text.push_back(c);
    }
    for (const auto candidate : {Phase::Ready, Phase::Running, Phase::Complete}) {
        const auto prefix = Header(token, candidate);
        if (text.compare(0, prefix.size(), prefix) != 0) continue;
        if (candidate != Phase::Complete && text.size() != prefix.size()) return false;
        phase = candidate;
        body = text.substr(prefix.size());
        return true;
    }
    return false;
}

inline Confirmation Confirm(HANDLE file, std::wstring_view token, DWORD exit_code, std::wstring& body) {
    Phase phase{};
    if (!Read(file, token, phase, body)) return Confirmation::Unknown;
    if (exit_code == 2 && phase == Phase::Ready) return Confirmation::Rejected;
    if (exit_code <= 1 && phase == Phase::Complete) return Confirmation::Complete;
    return Confirmation::Unknown;
}

struct Report {
    std::wstring body;
    bool all_ok = false;
};

// The durable Running marker precedes any mutation. A missing final report
// means an unknown outcome, including a crash between an Apply and this write.
template<class Execute, class Writer>
int Run(HANDLE file, std::wstring_view token, Execute execute, Writer write) {
    Phase phase{};
    std::wstring body;
    if (!Read(file, token, phase, body) || phase != Phase::Ready) return 2;
    if (!write(file, token, Phase::Running, std::wstring_view{})) return 2;
    const Report report = execute();
    if (!write(file, token, Phase::Complete, std::wstring_view(report.body))) return kUnknownExit;
    return report.all_ok ? 0 : 1;
}

inline void DeleteOwned(HANDLE file) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
}

class Channel {
public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel() {
        for (const HANDLE file : {batch_file, result_file}) {
            if (file == INVALID_HANDLE_VALUE) continue;
            if (!preserve) DeleteOwned(file);
            CloseHandle(file);
        }
        if (directory_file != INVALID_HANDLE_VALUE) {
            if (!preserve) DeleteOwned(directory_file); // a helper backup keeps it nonempty
            CloseHandle(directory_file);
        }
    }

    bool Create(const std::wstring& parent, std::wstring_view id, const std::string& batch) {
        if (!ValidToken(id) || batch.empty() || batch.size() > 256 * 1024 ||
            directory_file != INVALID_HANDLE_VALUE) return false;
        token = id;
        directory = parent + L"\\pulse_sysctx_" + token;
        HANDLE process_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process_token)) return false;
        DWORD bytes = 0;
        GetTokenInformation(process_token, TokenUser, nullptr, 0, &bytes);
        std::vector<BYTE> user(bytes);
        const bool got_user = bytes != 0 && GetTokenInformation(process_token, TokenUser, user.data(), bytes, &bytes);
        CloseHandle(process_token);
        if (!got_user) return false;
        wchar_t* sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return false;
        const std::wstring acl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return false;
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        const bool created = CreateDirectoryW(directory.c_str(), &security) != FALSE;
        LocalFree(descriptor);
        if (!created) return false;
        directory_file = CreateFileW(directory.c_str(), DELETE | FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (directory_file == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION directory_info{};
        if (!GetFileInformationByHandle(directory_file, &directory_info) ||
            !(directory_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (directory_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        batch_path = directory + L"\\batch.txt";
        result_path = directory + L"\\result.txt";
        batch_file = CreateFileW(batch_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (batch_file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        if (!WriteFile(batch_file, batch.data(), static_cast<DWORD>(batch.size()), &written, nullptr) ||
            written != batch.size() || !FlushFileBuffers(batch_file)) return false;
        result_file = CreateFileW(result_path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        return result_file != INVALID_HANDLE_VALUE && Write(result_file, token, Phase::Ready);
    }

    std::wstring directory, batch_path, result_path, token;
    HANDLE batch_file = INVALID_HANDLE_VALUE;
    HANDLE result_file = INVALID_HANDLE_VALUE;
    bool preserve = false;

private:
    HANDLE directory_file = INVALID_HANDLE_VALUE;
};

// Never creates, replaces, truncates, or deletes a caller-supplied path.
inline HANDLE OpenForHelper(const std::wstring& path, std::wstring_view token) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return file;
    BY_HANDLE_FILE_INFORMATION info{};
    Phase phase{};
    std::wstring body;
    if (!GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1 || !Read(file, token, phase, body) || phase != Phase::Ready) {
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

} // namespace pulse::app::sysctx::exchange
