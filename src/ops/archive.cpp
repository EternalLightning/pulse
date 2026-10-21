#include "archive.h"
#include "destination_guard.h"
#include "archive_retry.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <cwctype>
#include <cwchar>
#include <utility>
#include <limits>

namespace pulse::ops {
namespace {
namespace fs = std::filesystem;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
std::wstring Decode(const std::string& bytes) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), result.data(), size);
    return result;
}
std::string Encode(const std::wstring& value) {
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
fs::path Runtime() {
    std::wstring executable(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    executable.resize(length);
    fs::path runtime = fs::path(executable).parent_path() / L"7zip" / L"7z.exe";
    if (fs::exists(runtime)) return runtime;
#ifdef PULSE_ARCHIVE_RUNTIME_DIR
    runtime = fs::path(PULSE_ARCHIVE_RUNTIME_DIR) / L"7z.exe";
#endif
    return runtime;
}
struct ProcessAttributes {
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~ProcessAttributes() {
        if (value) {
            DeleteProcThreadAttributeList(value);
            HeapFree(GetProcessHeap(), 0, value);
        }
    }
};
struct ProcessResult {
    ArchiveOutcome outcome = ArchiveOutcome::Failed;
    std::optional<DWORD> exit_code;
    std::wstring output;
};
ProcessResult Run(const std::vector<std::wstring>& args, const std::atomic_bool* cancel,
    const fs::path& exe = Runtime(), size_t max_output_bytes = 64 * 1024 * 1024,
    const std::function<void(std::string_view)>& progress = {}) {
    ProcessResult result;
    if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; result.output = L"已取消解压。"; return result; }
    if (!fs::exists(exe)) { result.output = L"找不到压缩包运行库 7zip/7z.exe。"; return result; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle read, write, input;
    // Large listings must not stall the writer on a tiny pipe between cancellation polls.
    constexpr DWORD pipe_buffer_bytes = 256 * 1024;
    if (!CreatePipe(&read.value, &write.value, &security, pipe_buffer_bytes)) { result.output = L"无法创建压缩包输出管道。"; return result; }
    if (!SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) { result.output = L"无法配置压缩包输出管道。"; return result; }
    Handle progress_read, progress_write;
    if (progress && (!CreatePipe(&progress_read.value, &progress_write.value, &security, pipe_buffer_bytes) ||
        !SetHandleInformation(progress_read.value, HANDLE_FLAG_INHERIT, 0))) {
        result.output = L"无法创建解压进度管道。"; return result;
    }
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) { result.output = L"无法创建压缩包输入句柄。"; return result; }
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    ProcessAttributes attributes;
    auto storage = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, attribute_size));
    if (!storage) { result.output = L"无法配置压缩包进程句柄。"; return result; }
    if (!InitializeProcThreadAttributeList(storage, 1, 0, &attribute_size)) {
        HeapFree(GetProcessHeap(), 0, storage);
        result.output = L"无法配置压缩包进程句柄。"; return result;
    }
    attributes.value = storage;
    HANDLE inherited[] = {input.value, write.value, progress_write.value};
    if (!UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(HANDLE) * (progress ? 3 : 2), nullptr, nullptr)) {
        result.output = L"无法配置压缩包进程句柄。"; return result;
    }
    std::wstring command = Quote(exe.wstring());
    for (const auto& arg : args) command += L" " + Quote(arg);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdError = write.value;
    startup.StartupInfo.hStdOutput = progress ? progress_write.value : write.value;
    startup.lpAttributeList = attributes.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process)) {
        result.output = L"无法启动压缩包运行库。"; return result;
    }
    Handle process_handle{process.hProcess}, thread{process.hThread};
    CloseHandle(write.value); write.value = INVALID_HANDLE_VALUE;
    if (progress) { CloseHandle(progress_write.value); progress_write.value = INVALID_HANDLE_VALUE; }
    std::string output;
    char buffer[64 * 1024]; DWORD count = 0;
    bool cancelled = false, output_limit = false;
    for (;;) {
        if (cancel && cancel->load()) { TerminateProcess(process.hProcess, 2); cancelled = true; break; }
        DWORD progress_available = 0;
        if (progress) {
            if (PeekNamedPipe(progress_read.value, nullptr, 0, nullptr, &progress_available, nullptr) && progress_available &&
                ReadFile(progress_read.value, buffer, (std::min<DWORD>)(progress_available, sizeof(buffer)), &count, nullptr))
                progress(std::string_view(buffer, count));
            else progress({});
        }
        DWORD available = 0;
        if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            if (!ReadFile(read.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) break;
            if (count > max_output_bytes - output.size()) {
                TerminateProcess(process.hProcess, 2);
                output_limit = true;
                break;
            }
            output.append(buffer, count);
        } else if (WaitForSingleObject(process.hProcess, 20) == WAIT_OBJECT_0) {
            // The process can write its final bytes while the wait is in progress.
            PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr);
            if (progress) PeekNamedPipe(progress_read.value, nullptr, 0, nullptr, &progress_available, nullptr);
            if (!available && !progress_available) break;
        }
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    // Cancellation can leave the extractor's final verbose member record in
    // stdout. Drain it after exit before sealing retry identities.
    if (progress) {
        DWORD available = 0;
        while (PeekNamedPipe(progress_read.value, nullptr, 0, nullptr, &available, nullptr) && available &&
            ReadFile(progress_read.value, buffer, (std::min<DWORD>)(available, sizeof(buffer)), &count, nullptr) && count)
            progress(std::string_view(buffer, count));
    }
    DWORD code = 0;
    if (GetExitCodeProcess(process.hProcess, &code)) result.exit_code = code;
    if (cancelled || output_limit) {
        result.outcome = cancelled ? ArchiveOutcome::Cancelled : ArchiveOutcome::OutputLimit;
        result.output = cancelled ? L"已取消解压。" : L"压缩包输出超过安全大小限制。";
    } else {
        result.outcome = result.exit_code == 0u ? ArchiveOutcome::Success : ArchiveOutcome::Failed;
        result.output = Decode(output);
    }
    if (result.outcome != ArchiveOutcome::Success && result.output.empty()) result.output = L"压缩包处理失败。";
    return result;
}
std::wstring Normalize(std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return path;
}
bool SafeName(const std::wstring& path) {
    if (path.empty() || path.front() == L'/' || path.find_first_of(L":\r\n") != std::wstring::npos) return false;
    std::wistringstream parts(path); std::wstring part;
    while (std::getline(parts, part, L'/')) {
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        if (std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'*' || c == L'?'; })) return false;
        std::wstring stem = part.substr(0, part.find(L'.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT") &&
                ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'¹' || stem[3] == L'²' || stem[3] == L'³'))) return false;
    }
    return true;
}
bool CapturePlain(DestinationGuard& guard, const fs::path& path, std::wstring& error) {
    if (!guard.Capture(path.wstring(), error)) return false;
    if (std::any_of(guard.Impacts().begin(), guard.Impacts().end(), [](const DestinationLinkImpact& impact) {
        return impact.kind != DestinationLinkKind::HardLink;
    })) { error = L"路径包含链接，无法安全解压。"; return false; }
    return true;
}
// Each directory stays pinned while its children are enumerated and deleted.
// A changed identity is left behind; a reparse leaf is never traversed.
bool RemoveStageTree(const fs::path& path, DestinationGuard& guard) {
    std::wstring ignored;
    if (!guard.Capture(path.wstring(), ignored) ||
        !guard.PinDirectories(path.parent_path().wstring(), ignored)) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (!guard.PinDirectories(path.wstring(), ignored) || !guard.Check(path.wstring(), ignored)) return false;
        std::error_code ec;
        fs::directory_iterator children(path, ec);
        if (ec) return false;
        bool removed = true;
        for (; children != fs::directory_iterator{}; children.increment(ec)) {
            if (ec) { removed = false; break; }
            removed = RemoveStageTree(children->path(), guard) && removed;
        }
        if (ec) removed = false;
        guard.ReleaseDirectory(path.wstring());
        if (!removed) return false;
    }
    return guard.DeleteOwnedLeaf(path.wstring(), ignored);
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
thread_local const ArchiveExtractionHook* extraction_hook = nullptr;
void ExtractionPoint(ArchiveExtractionPoint point, const fs::path& destination, const fs::path& selection = {}) {
    if (extraction_hook && *extraction_hook) (*extraction_hook)(point, destination.wstring(), selection.wstring());
}
#endif
struct SelectionFile {
    DestinationGuard guard;
    fs::path directory, path;
    Handle lock;

    explicit SelectionFile(const std::shared_ptr<DestinationGuard::DirectoryLeases>& leases)
        : guard(GetFileInformationByHandleEx, leases) {}
    ~SelectionFile() {
        if (lock.value != INVALID_HANDLE_VALUE) { CloseHandle(lock.value); lock.value = INVALID_HANDLE_VALUE; }
        if (!directory.empty()) RemoveStageTree(directory, guard);
    }

    bool Create(const std::vector<std::wstring>& members, std::wstring& error) {
        wchar_t temp[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH) { error = L"无法访问临时目录。"; return false; }
        const fs::path root = fs::absolute(temp).lexically_normal();
        if (!CapturePlain(guard, root, error) || !guard.PinDirectories(root.wstring(), error)) return false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = root / (L"PulseArchiveList-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (!CapturePlain(guard, candidate, error)) return false;
            auto created = DestinationGuard::CreateDirectoryLeaf(candidate.wstring(), false, error);
            if (created) {
                error.clear();
                if (!guard.AdoptCreatedLeaf(candidate.wstring(), created.get(), error) ||
                    !guard.PinDirectories(candidate.wstring(), error)) return false;
                directory = candidate;
                break;
            }
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) return false;
        }
        if (directory.empty()) { error = L"无法创建临时解压列表目录。"; return false; }
        path = directory / L"selection.txt";
        lock.value = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock.value == INVALID_HANDLE_VALUE) { error = L"无法创建临时解压列表。"; return false; }
        if (!guard.AdoptCreatedLeaf(path.wstring(), lock.value, error)) return false;
        for (const auto& member : members) {
            const auto line = Encode(member) + '\n';
            DWORD written = 0;
            if (line.size() > MAXDWORD || !WriteFile(lock.value, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
                written != line.size()) { error = L"无法写入临时解压列表。"; return false; }
        }
        CloseHandle(lock.value);
        lock.value = INVALID_HANDLE_VALUE;
#ifdef PULSE_ARCHIVE_PROCESS_TEST
        ExtractionPoint(ArchiveExtractionPoint::SelectionWritten, directory, path);
#endif
        lock.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock.value == INVALID_HANDLE_VALUE || !guard.Check(path.wstring(), error)) {
            error = L"临时解压列表身份无法确认。"; return false;
        }
        // Check content under the read lock: identity alone cannot detect an
        // in-place edit during the write-to-read handle transition.
        for (const auto& member : members) {
            const auto expected = Encode(member) + '\n';
            std::string actual(expected.size(), '\0');
            DWORD read = 0;
            if (!ReadFile(lock.value, actual.data(), static_cast<DWORD>(actual.size()), &read, nullptr) ||
                read != actual.size() || actual != expected) {
                error = L"临时解压列表内容发生变化。"; return false;
            }
        }
        char extra = 0;
        DWORD read = 0;
        if (!ReadFile(lock.value, &extra, 1, &read, nullptr) || read) {
            error = L"临时解压列表内容发生变化。"; return false;
        }
        return true;
    }
};
ArchiveResult ParseListing(ProcessResult run);
std::wstring ListingDiagnostic(const std::wstring& output) {
    // A failed ZIP scan can emit thousands of SLT records instead of a diagnostic.
    if (output.find(L"Path = ") != std::wstring::npos &&
        output.find(L"\nFolder = ") != std::wstring::npos)
        return L"压缩包目录信息不完整或损坏，无法读取完整内容。";
    constexpr size_t limit = 1024;
    return output.size() <= limit ? output : output.substr(0, limit) + L"\n…";
}
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
ArchiveProcessTestResult RunArchiveProcessForTest(const std::wstring& executable,
    const std::vector<std::wstring>& args, size_t output_limit, const std::atomic_bool* cancel,
    const std::function<void(std::string_view)>& progress) {
    auto result = Run(args, cancel, fs::path(executable), output_limit, progress);
    return {result.outcome, result.exit_code, std::move(result.output)};
}
ArchiveResult ParseArchiveListingForTest(const std::wstring& output, DWORD exit_code) {
    return ParseListing({exit_code == 0u ? ArchiveOutcome::Success : ArchiveOutcome::Failed, exit_code, output});
}
#endif
bool IsArchivePath(const std::wstring& path) {
    std::wstring extension = fs::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return extension == L".zip" || extension == L".7z" || extension == L".rar" ||
        extension == L".tar" || extension == L".tgz" || extension == L".gz" ||
        extension == L".bz2" || extension == L".xz" || extension == L".cab" ||
        extension == L".tbz2" || extension == L".txz" || extension == L".zst";
}
bool IsSafeArchiveMember(const std::wstring& path) {
    std::wstring normalized = Normalize(path);
    if (!normalized.empty() && normalized.back() == L'/') normalized.pop_back();
    return SafeName(normalized);
}
ArchiveResult ListArchive(const std::wstring& path, const std::wstring& password, const std::atomic_bool* cancel) {
    auto result = ParseListing(Run({L"l", L"-slt", L"-ba", L"-sccUTF-8", L"-bd", L"-p" + password, L"--", path}, cancel));
    if (cancel && cancel->load()) {
        result.outcome = ArchiveOutcome::Cancelled;
        result.password_state = ArchivePasswordState::Unknown;
        result.entries.clear(); result.error = L"已取消解压。";
    }
    return result;
}
namespace {
ArchiveResult ParseListing(ProcessResult run) {
    ArchiveResult result;
    result.outcome = run.outcome;
    result.exit_code = run.exit_code;
    const bool recovering = run.outcome == ArchiveOutcome::Failed &&
        (run.exit_code == 1u || run.exit_code == 2u) &&
        run.output.find(L"Descriptor_ERROR") != std::wstring::npos;
    if (run.outcome != ArchiveOutcome::Success && !recovering) {
        result.error = ListingDiagnostic(run.output);
        return result;
    }
    result.password_state = ArchivePasswordState::NotRequired;
    std::wistringstream lines(run.output); std::wstring line;
    ArchiveEntry entry; bool has_path = false, unsafe = false;
    bool has_folder = false, has_size = false, has_encrypted = false;
    bool local_record = false, descriptor_error = false, damaged_local_record = false;
    auto flush = [&]() {
        if (!has_path) return;
        entry.path = Normalize(entry.path);
        while (!entry.path.empty() && entry.path.back() == L'/') entry.path.pop_back();
        if (!SafeName(entry.path)) unsafe = true;
        damaged_local_record |= local_record && descriptor_error;
        if (entry.is_dir) entry.path += L'/';
        if (!recovering || (has_folder && (entry.is_dir || has_size) && has_encrypted &&
                            local_record && !descriptor_error))
            result.entries.push_back(std::move(entry));
        entry = {}; has_path = false;
        has_folder = has_size = has_encrypted = local_record = descriptor_error = false;
    };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) { flush(); continue; }
        if (line.starts_with(L"Path = ")) { flush(); entry.path = line.substr(7); has_path = true; }
        else if (line.starts_with(L"Size = ")) {
            const auto value = line.substr(7);
            try {
                size_t used = 0;
                if (!value.empty() && value.front() >= L'0' && value.front() <= L'9') {
                    entry.size = std::stoull(value, &used);
                    has_size = used == value.size();
                }
            } catch (...) { has_size = false; }
            if (!has_size && !recovering) unsafe = true;
        }
        else if (line.starts_with(L"Modified = ")) {
            SYSTEMTIME local_time{};
            FILETIME local_file_time{};
            if (swscanf_s(line.c_str() + 11, L"%hu-%hu-%hu %hu:%hu:%hu", &local_time.wYear,
                &local_time.wMonth, &local_time.wDay, &local_time.wHour, &local_time.wMinute,
                &local_time.wSecond) == 6 && SystemTimeToFileTime(&local_time, &local_file_time)) {
                LocalFileTimeToFileTime(&local_file_time, &entry.mtime);
            }
        }
        else if (line == L"Folder = +" || line == L"Folder = -") {
            has_folder = true;
            entry.is_dir = line == L"Folder = +";
        }
        else if (line.starts_with(L"Characteristics = ")) {
            const auto value = line.substr(18);
            local_record = value == L"Local" || value.starts_with(L"Local ");
            const auto marker = value.find(L"Descriptor_ERROR");
            descriptor_error = marker != std::wstring::npos &&
                (marker == 0 || value[marker - 1] == L' ') &&
                (marker + 16 == value.size() || value[marker + 16] == L' ');
        }
        else if (line.starts_with(L"Attributes = ")) {
            const auto value = line.substr(13);
            if (value.find(L'D') != std::wstring::npos || value.starts_with(L"d") || value.find(L" d") != std::wstring::npos) entry.is_dir = true;
            if (value.find(L'L') != std::wstring::npos || value.starts_with(L"l") || value.find(L" l") != std::wstring::npos) unsafe = true;
        } else if ((line.starts_with(L"Symbolic Link = ") && line.size() > 16) ||
            (line.starts_with(L"Hard Link = ") && line.size() > 12) ||
            (line.starts_with(L"Copy Link = ") && line.size() > 12)) unsafe = true;
        // Anti-items can delete target paths instead of extracting ordinary data.
        else if (line == L"Anti = +") unsafe = true;
        else if (has_path && (line == L"Encrypted = +" || line == L"Encrypted = -")) {
            has_encrypted = true;
            entry.encrypted = line == L"Encrypted = +";
            if (entry.encrypted) result.password_state = ArchivePasswordState::Required;
        }
    }
    flush();
    if (unsafe) {
        result.outcome = ArchiveOutcome::Failed;
        result.entries.clear(); result.error = L"压缩包包含不安全路径、链接或删除记录，已拒绝处理。";
    } else if (recovering) {
        if (damaged_local_record && !result.entries.empty()) {
            result.outcome = ArchiveOutcome::PartialListing;
            result.error = L"压缩包不完整，已显示可读取的内容；部分文件可能缺失或损坏。";
        } else {
            result.entries.clear();
            result.password_state = ArchivePasswordState::Unknown;
            result.error = ListingDiagnostic(run.output);
        }
    }
    return result;
}
}
ArchiveOperationResult ExtractArchive(const std::wstring& path, const std::wstring& destination,
    const std::vector<std::wstring>& selected_paths, const std::wstring& password,
    const std::atomic_bool* cancel, const ArchiveProgressCallback& progress) {
    ArchiveOperationResult result;
    ArchiveProgressTracker tracker;
    ULONGLONG last_progress = 0;
    const auto report = [&](bool force = false) {
        const auto now = GetTickCount64();
        if (progress && (force || now - last_progress >= 100)) {
            progress(tracker.Status());
            last_progress = now;
        }
    };
    report(true);
    auto& error = result.error;
    if (cancel && cancel->load()) {
        result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result;
    }
    // Keep the checked archive immutable until extraction finishes.
    Handle archive_lock{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (archive_lock.value == INVALID_HANDLE_VALUE) { error = L"无法读取压缩包，文件可能正在被修改。"; return result; }
    auto listing = ListArchive(path, password, cancel);
    result = static_cast<ArchiveOperationResult&>(listing);
    if (listing.outcome != ArchiveOutcome::Success) {
        if (listing.outcome == ArchiveOutcome::PartialListing) {
            result.outcome = ArchiveOutcome::Failed;
            result.error = L"压缩包不完整，无法安全解压。请先使用压缩软件修复或重新获取完整文件。";
        }
        return result;
    }
    result.outcome = ArchiveOutcome::Failed;
    try {
        fs::path target = fs::absolute(destination).lexically_normal();
        DestinationGuard destination_guard;
        if (!CapturePlain(destination_guard, target, error)) return result;
#ifdef PULSE_ARCHIVE_PROCESS_TEST
        ExtractionPoint(ArchiveExtractionPoint::DestinationCaptured, target);
#endif
        if (!destination_guard.PinDirectories(target.wstring(), error)) return result;
        std::vector<ArchiveEntry> selected;
        for (const auto& entry : listing.entries) {
            bool include = selected_paths.empty();
            for (auto wanted : selected_paths) {
                wanted = Normalize(wanted);
                if (entry.path == wanted || (!wanted.empty() && wanted.back() == L'/' && entry.path.starts_with(wanted))) include = true;
            }
            if (include) selected.push_back(entry);
        }
        if (selected.empty() && !selected_paths.empty()) { error = L"压缩包中找不到所选项目。"; return result; }
        for (const auto& entry : listing.entries) {
            const auto first = fs::path(entry.path).begin()->wstring();
            if (pulse::path::EqualInsensitive(first, ArchiveRetry::kName)) {
                error = L"压缩包包含与解压恢复记录冲突的名称。"; return result;
            }
        }
        if (!destination_guard.EnsureDirectories(target.wstring(), error)) return result;
        ArchiveRetry retry;
        if (!retry.Open(target, path, error) || !retry.Recover(selected, destination_guard, error)) return result;
        std::vector<ArchiveEntry> pending;
        uint64_t total_bytes = 0;
        // Check every selected endpoint before creating directories or launching 7-Zip.
        for (const auto& entry : selected) {
            if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result; }
            const auto output = target / fs::path(entry.path);
            if (!CapturePlain(destination_guard, output, error)) return result;
            const DWORD attributes = GetFileAttributesW(output.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                if (entry.is_dir && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    error = L"解压目录与已有文件同名。"; return result;
                }
                if (!entry.is_dir) continue;
            } else {
                const DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                    error = L"无法检查解压目标 (" + std::to_wstring(code) + L")。"; return result;
                }
            }
            if (!entry.is_dir) {
                if (entry.size > (std::numeric_limits<uint64_t>::max)() - total_bytes) {
                    error = L"压缩包文件总大小超过支持范围。"; return result;
                }
                total_bytes += entry.size;
            }
            pending.push_back(entry);
        }
        if (!destination_guard.EnsureDirectories(target.wstring(), error)) return result;
        for (const auto& entry : pending) {
            if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result; }
            const auto output = target / fs::path(entry.path);
            const auto directory = entry.is_dir ? output : output.parent_path();
            if (!destination_guard.EnsureDirectories(directory.wstring(), error)) return result;
        }
        const bool files = std::any_of(pending.begin(), pending.end(), [](const auto& entry) { return !entry.is_dir; });
        tracker.Begin(ArchivePhase::Extracting, total_bytes, pending.size(), GetTickCount64());
        report(true);
        if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result; }
        if (!files) {
            if (!retry.Finish(true, error)) return result;
            result.outcome = ArchiveOutcome::Success;
            tracker.Begin(ArchivePhase::Completed, total_bytes, pending.size(), GetTickCount64());
            tracker.Advance(total_bytes, pending.size(), {}, GetTickCount64());
            report(true);
            return result;
        }
        std::vector<std::wstring> args{L"x", L"-y", L"-aos", L"-spd", L"-bb1", L"-bsp1", L"-bso1", L"-bse2", L"-sccUTF-8", L"-scsUTF-8",
            L"-p" + password, L"-o" + target.wstring()};
        SelectionFile selection_file(destination_guard.SharedDirectoryLeases());
        if (!selected_paths.empty()) {
            std::vector<std::wstring> members;
            size_t command_size = Quote(Runtime().wstring()).size() + 1 + Quote(path).size() + Quote(L"--").size() + 2;
            for (const auto& arg : args) command_size += Quote(arg).size() + 1;
            for (const auto& entry : pending) if (!entry.is_dir) {
                members.push_back(entry.path);
                command_size += Quote(L"-i!" + entry.path).size() + 1;
            }
            // CreateProcessW limits the complete command line, including its null,
            // to 32767 UTF-16 code units. Most selections need no list file.
            if (command_size <= 32767) {
                for (const auto& member : members) args.push_back(L"-i!" + member);
            } else {
                if (!selection_file.Create(members, error)) return result;
                args.push_back(L"-i@" + selection_file.path.wstring());
            }
        }
        args.push_back(L"--");
        args.push_back(path);
#ifdef PULSE_ARCHIVE_PROCESS_TEST
        ExtractionPoint(ArchiveExtractionPoint::BeforeExtract, target, selection_file.path);
#endif
        // Keep directory leases throughout the child process. -aos skips existing
        // files for extract-all and collisions observed after our preflight.
        for (const auto& entry : pending) {
            const auto output = target / fs::path(entry.path);
            const auto directory = entry.is_dir ? output : output.parent_path();
            if (!destination_guard.Check(directory.wstring(), error)) return result;
        }
        if (!retry.Begin(pending, error)) return result;
        auto run = Run(args, cancel, Runtime(), 64 * 1024 * 1024,
            [&](std::string_view chunk) {
                retry.Feed(chunk);
#ifdef PULSE_ARCHIVE_PROCESS_TEST
                ExtractionPoint(ArchiveExtractionPoint::ProcessProgress, target, selection_file.path);
#endif
                tracker.Feed(chunk, GetTickCount64());
                report();
            });
        result.exit_code = run.exit_code;
        if (run.outcome != ArchiveOutcome::Success) {
            result.outcome = run.outcome; error = std::move(run.output);
            std::wstring journal_error;
            if (!retry.Finish(false, journal_error)) error += L"\n" + journal_error;
            return result;
        }
        if (cancel && cancel->load()) {
            result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。";
            std::wstring journal_error;
            if (!retry.Finish(false, journal_error)) error += L"\n" + journal_error;
            return result;
        }
        for (const auto& entry : pending) {
            const auto output = target / fs::path(entry.path);
            const auto directory = entry.is_dir ? output : output.parent_path();
            if (!destination_guard.Check(directory.wstring(), error)) return result;
            const DWORD attributes = GetFileAttributesW(output.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                (entry.is_dir != ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0))) {
                error = L"解压输出类型异常。"; return result;
            }
        }
        if (!retry.Finish(true, error)) return result;
        result.outcome = ArchiveOutcome::Success;
        tracker.Begin(ArchivePhase::Completed, total_bytes, pending.size(), GetTickCount64());
        tracker.Advance(total_bytes, pending.size(), {}, GetTickCount64());
        report(true);
        return result;
    } catch (const fs::filesystem_error& exception) { error = L"解压文件系统错误：" + Decode(exception.what()); return result; }
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
ArchiveOperationResult ExtractArchiveForTest(const std::wstring& path, const std::wstring& destination,
    const ArchiveExtractionHook& hook, const std::atomic_bool* cancel, const ArchiveProgressCallback& progress,
    const std::vector<std::wstring>& selected_paths) {
    struct Restore {
        const ArchiveExtractionHook* previous = extraction_hook;
        ~Restore() { extraction_hook = previous; }
    } restore;
    extraction_hook = &hook;
    return ExtractArchive(path, destination, selected_paths, L"", cancel, progress);
}
#endif
bool ExtractArchive(const std::wstring& path, const std::wstring& destination,
    const std::vector<std::wstring>& selected_paths, const std::wstring& password, std::wstring& error,
    const std::atomic_bool* cancel) {
    auto result = ExtractArchive(path, destination, selected_paths, password, cancel);
    error = std::move(result.error);
    return result.outcome == ArchiveOutcome::Success;
}
}
