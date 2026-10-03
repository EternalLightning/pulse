#include "file_lock_owner.h"
#include <restartmanager.h>
#include <winsvc.h>
#include <algorithm>
#include <filesystem>
#include <set>
#include <cwctype>

#pragma comment(lib, "rstrtmgr.lib")
#pragma comment(lib, "advapi32.lib")

namespace pulse::ops {
namespace {
class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE Get() const { return value_; }
private:
    HANDLE value_;
};
bool Cancelled(const LockCancelled& check) { return check && check(); }
bool Equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring Leaf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return path.substr(slash == std::wstring::npos ? 0 : slash + 1);
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}
std::wstring Image(HANDLE process) {
    std::wstring value(32768, L'\0');
    DWORD length = static_cast<DWORD>(value.size());
    if (!QueryFullProcessImageNameW(process, 0, value.data(), &length)) return {};
    value.resize(length);
    return value;
}
bool ServicePids(std::set<DWORD>& pids) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!manager) return false;
    DWORD bytes = 0, count = 0, resume = 0;
    EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
        nullptr, 0, &bytes, &count, &resume, nullptr);
    bool ok = false;
    if (GetLastError() == ERROR_MORE_DATA && bytes <= 4 * 1024 * 1024) {
        std::vector<BYTE> data(bytes);
        resume = 0;
        ok = EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE,
            data.data(), static_cast<DWORD>(data.size()), &bytes, &count, &resume, nullptr) != FALSE;
        if (ok) {
            const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(data.data());
            for (DWORD i = 0; i < count; ++i) pids.insert(services[i].ServiceStatusProcess.dwProcessId);
        }
    }
    CloseServiceHandle(manager);
    return ok;
}
bool Protected(HANDLE process, DWORD pid, const std::wstring& image,
               const std::wstring& protected_directory, const std::set<DWORD>& services) {
    if (pid <= 4 || pid == GetCurrentProcessId() || image.empty() || services.contains(pid)) return true;
    const auto name = Leaf(image);
    for (const auto* reserved : {L"explorer.exe", L"Pulse.exe", L"Pulse.Shell.exe", L"Pulse.Index.exe",
            L"Pulse.Preview.exe", L"Pulse.Network.exe", L"csrss.exe", L"smss.exe", L"wininit.exe",
            L"winlogon.exe", L"services.exe", L"lsass.exe", L"svchost.exe"})
        if (Equal(name, reserved)) return true;
    if (!protected_directory.empty() && Equal(Parent(image), protected_directory)) return true;
    BOOL critical = TRUE;
    if (!IsProcessCritical(process, &critical) || critical) return true;
    return false;
}
void CollectFiles(const std::wstring& root, std::vector<std::wstring>& files,
                  const LockCancelled& cancelled, bool& complete,
                  size_t& visited, ULONGLONG deadline) {
    constexpr size_t limit = 256;
    const auto attrs = GetFileAttributesW(root.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (files.size() < limit) files.push_back(root);
        else complete = false;
        return;
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(root,
        std::filesystem::directory_options::skip_permission_denied, error), end;
    while (!error && it != end) {
        if (Cancelled(cancelled)) return;
        if (files.size() >= limit || ++visited > 4096 || GetTickCount64() >= deadline) {
            complete = false;
            break;
        }
        const auto path = it->path().wstring();
        const auto child_attrs = GetFileAttributesW(path.c_str());
        if (child_attrs != INVALID_FILE_ATTRIBUTES) {
            if (child_attrs & FILE_ATTRIBUTE_REPARSE_POINT) it.disable_recursion_pending();
            else if (!(child_attrs & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(path);
        }
        it.increment(error);
    }
    if (error) complete = false;
}
std::vector<RM_PROCESS_INFO> Owners(const std::vector<std::wstring>& files, std::wstring* diagnostics) {
    DWORD session = 0;
    WCHAR session_key[CCH_RM_SESSION_KEY + 1]{};
    const auto started = RmStartSession(&session, 0, session_key);
    if (diagnostics) *diagnostics += L"RmStartSession=" + std::to_wstring(started) + L"\n";
    if (started != ERROR_SUCCESS) return {};
    struct SessionGuard { DWORD session; ~SessionGuard() { RmEndSession(session); } } guard{session};
    std::vector<LPCWSTR> names;
    for (const auto& file : files) names.push_back(file.c_str());
    const auto registered = RmRegisterResources(session, static_cast<UINT>(names.size()), names.data(), 0, nullptr, 0, nullptr);
    if (diagnostics) *diagnostics += L"RmRegisterResources=" + std::to_wstring(registered) + L", files=" + std::to_wstring(names.size()) + L"\n";
    if (registered != ERROR_SUCCESS) return {};
    std::vector<RM_PROCESS_INFO> owners;
    for (int retry = 0; retry < 4; ++retry) {
        UINT required = 0, count = static_cast<UINT>(owners.size());
        DWORD reasons = 0;
        const auto result = RmGetList(session, &required, &count,
            owners.empty() ? nullptr : owners.data(), &reasons);
        if (diagnostics) *diagnostics += L"RmGetList=" + std::to_wstring(result) + L", required=" +
            std::to_wstring(required) + L", count=" + std::to_wstring(count) + L", reasons=" + std::to_wstring(reasons) + L"\n";
        if (result == ERROR_SUCCESS) { owners.resize(count); return owners; }
        if (result != ERROR_MORE_DATA || required > 4096) break;
        owners.resize(required);
    }
    return {};
}
std::wstring FailedName(const std::wstring& error) {
    const auto separator = error.rfind(L" | ");
    auto tail = separator == std::wstring::npos ? error : error.substr(separator + 3);
    const auto diagnostics = tail.find(L"; delete diagnostics:");
    if (diagnostics != std::wstring::npos) tail.resize(diagnostics);
    while (!tail.empty() && iswspace(tail.back())) tail.pop_back();
    const auto first = tail.find_first_not_of(L" \r\n\t");
    return first == std::wstring::npos ? std::wstring{} : tail.substr(first);
}
}

bool IsLockFailure(HRESULT error) noexcept {
    for (const auto code : {ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION, ERROR_ACCESS_DENIED,
            ERROR_USER_MAPPED_FILE, ERROR_DIR_NOT_EMPTY})
        if (error == HRESULT_FROM_WIN32(code)) return true;
    return error == static_cast<HRESULT>(0x80270021u) || error == static_cast<HRESULT>(0x80270022u)
        || error == static_cast<HRESULT>(0x80270027u) || error == static_cast<HRESULT>(0x80270028u);
}

LockReport ProbeFileLocks(HRESULT error, const std::wstring& failed_item,
    const std::vector<std::wstring>& candidates, const std::wstring& protected_directory,
    const LockCancelled& cancelled, std::wstring* diagnostics) {
    if (diagnostics) diagnostics->clear();
    LockReport report;
    if (!IsLockFailure(error) || Cancelled(cancelled)) return report;
    const auto failed = FailedName(failed_item);
    std::vector<std::wstring> roots;
    const bool absolute = std::filesystem::path(failed).is_absolute();
    if (absolute) roots.push_back(failed);
    else for (const auto& candidate : candidates)
        if (Equal(Leaf(candidate), failed)) roots.push_back(candidate);
    bool complete = true;
    std::vector<std::wstring> files;
    if (roots.empty()) roots = candidates;
    size_t visited = 0;
    const auto deadline = GetTickCount64() + 1000;
    for (const auto& root : roots) {
        if (Cancelled(cancelled)) return {};
        if (files.size() >= 256 || visited >= 4096 || GetTickCount64() >= deadline) {
            complete = false;
            break;
        }
        ++visited;
        CollectFiles(root, files, cancelled, complete, visited, deadline);
    }
    if (diagnostics) *diagnostics += L"collected=" + std::to_wstring(files.size()) + L", complete=" + std::to_wstring(complete) + L"\n";
    if (!absolute && !failed.empty()) {
        std::vector<std::wstring> matching;
        for (const auto& file : files) if (Equal(Leaf(file), failed)) matching.push_back(file);
        if (!matching.empty()) files = std::move(matching);
    }
    if (files.empty() || Cancelled(cancelled)) return {};
    std::set<DWORD> services;
    const bool services_known = ServicePids(services);
    auto owners = Owners(files, diagnostics);
    if (Cancelled(cancelled)) return {};
    report.path = absolute ? failed : files.size() == 1 ? files.front() : roots.front();
    for (const auto& found : owners) {
        if (std::any_of(report.owners.begin(), report.owners.end(), [&](const auto& owner) {
                return owner.pid == found.Process.dwProcessId;
            })) continue;
        LockOwner owner;
        owner.pid = found.Process.dwProcessId;
        owner.start_time = found.Process.ProcessStartTime;
        owner.app_name = found.strAppName;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner.pid));
        if (process.Get()) {
            owner.image_path = Image(process.Get());
            owner.can_terminate = complete && services_known &&
                found.ApplicationType != RmCritical && found.ApplicationType != RmService &&
                found.ApplicationType != RmExplorer &&
                !Protected(process.Get(), owner.pid, owner.image_path, protected_directory, services);
        }
        report.owners.push_back(std::move(owner));
    }
    if (report.owners.empty()) report.path.clear();
    return report;
}

EndLockResult EndLockOwner(const LockOwner& owner, const std::wstring& protected_directory,
    const LockCancelled& cancelled, DWORD timeout_ms, DWORD* error) {
    if (error) *error = ERROR_SUCCESS;
    if (Cancelled(cancelled)) return EndLockResult::Cancelled;
    if (!owner.can_terminate) return EndLockResult::Refused;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
        FALSE, owner.pid));
    if (!process.Get()) {
        const auto code = GetLastError();
        if (error) *error = code;
        return code == ERROR_INVALID_PARAMETER ? EndLockResult::AlreadyGone : EndLockResult::Failed;
    }
    if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0) return EndLockResult::AlreadyGone;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process.Get(), &created, &exited, &kernel, &user)) return EndLockResult::Refused;
    if (CompareFileTime(&created, &owner.start_time) != 0) return EndLockResult::AlreadyGone;
    std::set<DWORD> services;
    const auto image = Image(process.Get());
    if (!Equal(image, owner.image_path) || !ServicePids(services) ||
        Protected(process.Get(), owner.pid, image, protected_directory, services)) return EndLockResult::Refused;
    if (Cancelled(cancelled)) return EndLockResult::Cancelled;
    if (!TerminateProcess(process.Get(), ERROR_PROCESS_ABORTED)) {
        if (error) *error = GetLastError();
        return EndLockResult::Failed;
    }
    const auto deadline = GetTickCount64() + std::min<DWORD>(timeout_ms, 5000);
    do {
        if (WaitForSingleObject(process.Get(), 50) == WAIT_OBJECT_0) return EndLockResult::Ended;
        if (Cancelled(cancelled)) return EndLockResult::Cancelled;
    } while (GetTickCount64() < deadline);
    if (error) *error = ERROR_TIMEOUT;
    return EndLockResult::Failed;
}

std::wstring LockOwnerDescription(const LockOwner& owner) {
    const auto image = Leaf(owner.image_path);
    auto description = owner.app_name.empty() ? image : owner.app_name;
    if (!image.empty() && !Equal(description, image)) description += L" (" + image + L")";
    return description + L" [PID " + std::to_wstring(owner.pid) + L"]";
}
std::wstring LockProtectedDirectory() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    return Parent(path);
}
}
