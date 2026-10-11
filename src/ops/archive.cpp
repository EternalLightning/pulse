#include "archive.h"
#include "destination_guard.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <cwctype>
#include <cwchar>
#include <utility>

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
    const fs::path& exe = Runtime(), size_t max_output_bytes = 64 * 1024 * 1024) {
    ProcessResult result;
    if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; result.output = L"已取消解压。"; return result; }
    if (!fs::exists(exe)) { result.output = L"找不到压缩包运行库 7zip/7z.exe。"; return result; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle read, write, input;
    if (!CreatePipe(&read.value, &write.value, &security, 0)) { result.output = L"无法创建压缩包输出管道。"; return result; }
    if (!SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) { result.output = L"无法配置压缩包输出管道。"; return result; }
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
    HANDLE inherited[] = {input.value, write.value};
    if (!UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) {
        result.output = L"无法配置压缩包进程句柄。"; return result;
    }
    std::wstring command = Quote(exe.wstring());
    for (const auto& arg : args) command += L" " + Quote(arg);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
    startup.lpAttributeList = attributes.value;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process)) {
        result.output = L"无法启动压缩包运行库。"; return result;
    }
    Handle process_handle{process.hProcess}, thread{process.hThread};
    CloseHandle(write.value); write.value = INVALID_HANDLE_VALUE;
    std::string output;
    char buffer[8192]; DWORD count = 0;
    bool cancelled = false, output_limit = false;
    for (;;) {
        if (cancel && cancel->load()) { TerminateProcess(process.hProcess, 2); cancelled = true; break; }
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
            if (!PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) || !available) break;
        }
    }
    WaitForSingleObject(process.hProcess, INFINITE);
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
struct Cleanup {
    fs::path path;
    DestinationGuard& guard;
    ~Cleanup() { if (!path.empty()) RemoveStageTree(path, guard); }
};
#ifdef PULSE_ARCHIVE_PROCESS_TEST
thread_local const ArchivePublicationHook* publication_hook = nullptr;
thread_local bool force_publication_copy = false;
void PublicationPoint(ArchivePublicationPoint point, const fs::path& output, const fs::path& source = {}) {
    if (publication_hook && *publication_hook) (*publication_hook)(point, output.wstring(), source.wstring());
}
#endif
bool CopyAndPublish(const fs::path& source, const fs::path& output, DestinationGuard& guard,
    const std::atomic_bool* cancel, std::wstring& error, bool& cancelled) {
    Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    if (input.value == INVALID_HANDLE_VALUE) { error = L"无法读取临时解压文件。"; return false; }
    DestinationGuard::Handle owned;
    fs::path temporary;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary = output.parent_path() / (L".pulse-archive-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        if (!CapturePlain(guard, temporary, error)) return false;
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { owned.reset(file); break; }
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) {
            error = L"无法创建解压发布文件 (" + std::to_wstring(code) + L")。"; return false;
        }
    }
    if (!owned) { error = L"无法创建解压发布文件。"; return false; }
    struct Dispose {
        HANDLE handle;
        bool published = false;
        ~Dispose() {
            if (!published) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
            }
        }
    } dispose{owned.get()};
    if (!guard.AdoptCreatedLeaf(temporary.wstring(), owned.get(), error)) return false;
    std::vector<BYTE> buffer(1024 * 1024);
    for (;;) {
        if (cancel && cancel->load()) { cancelled = true; error = L"已取消解压。"; return false; }
        DWORD read = 0;
        if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            error = L"无法读取临时解压文件。"; return false;
        }
        if (!read) break;
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(owned.get(), buffer.data() + offset, read - offset, &written, nullptr) || !written) {
                error = L"无法写入解压发布文件。"; return false;
            }
            offset += written;
        }
#ifdef PULSE_ARCHIVE_PROCESS_TEST
        PublicationPoint(ArchivePublicationPoint::CopyProgress, output, source);
#endif
    }
    FILETIME created{}, accessed{}, modified{};
    if (!GetFileTime(input.value, &created, &accessed, &modified) ||
        !SetFileTime(owned.get(), &created, &accessed, &modified) || !FlushFileBuffers(owned.get())) {
        error = L"无法完成解压发布文件。"; return false;
    }
    if (cancel && cancel->load()) { cancelled = true; error = L"已取消解压。"; return false; }
    if (!guard.RenameLeaf(owned.get(), output.wstring())) {
        const DWORD code = GetLastError();
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return true;
        error = L"无法写入解压文件 (" + std::to_wstring(code) + L")。"; return false;
    }
    dispose.published = true;
    return guard.AdoptCreatedLeaf(output.wstring(), owned.get(), error);
}
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
    const std::vector<std::wstring>& args, size_t output_limit, const std::atomic_bool* cancel) {
    auto result = Run(args, cancel, fs::path(executable), output_limit);
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
        else if (has_path && (line == L"Encrypted = +" || line == L"Encrypted = -")) {
            has_encrypted = true;
            entry.encrypted = line == L"Encrypted = +";
            if (entry.encrypted) result.password_state = ArchivePasswordState::Required;
        }
    }
    flush();
    if (unsafe) {
        result.outcome = ArchiveOutcome::Failed;
        result.entries.clear(); result.error = L"压缩包包含不安全路径或链接，已拒绝处理。";
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
    const std::atomic_bool* cancel) {
    ArchiveOperationResult result;
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
#ifdef PULSE_ARCHIVE_PROCESS_TEST
        destination_guard.SharedDirectoryLeases()->before_relative_rename = [](const std::wstring& output) {
            PublicationPoint(ArchivePublicationPoint::BeforeRelativeRename, fs::path(output));
        };
#endif
        if (!CapturePlain(destination_guard, target, error)) return result;
#ifdef PULSE_ARCHIVE_PROCESS_TEST
        PublicationPoint(ArchivePublicationPoint::DestinationCaptured, target);
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
        wchar_t temp[MAX_PATH]{};
        DWORD temp_length = GetTempPathW(MAX_PATH, temp);
        if (!temp_length || temp_length >= MAX_PATH) { error = L"无法访问临时目录。"; return result; }
        DestinationGuard stage_guard(GetFileInformationByHandleEx, destination_guard.SharedDirectoryLeases());
        const fs::path temporary_root = fs::absolute(temp).lexically_normal();
        if (!CapturePlain(stage_guard, temporary_root, error) ||
            !stage_guard.PinDirectories(temporary_root.wstring(), error)) return result;
        fs::path stage;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            stage = temporary_root / (L"PulseArchive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (!CapturePlain(stage_guard, stage, error)) return result;
            auto created = DestinationGuard::CreateDirectoryLeaf(stage.wstring(), false, error);
            if (created) {
                error.clear();
                if (!stage_guard.AdoptCreatedLeaf(stage.wstring(), created.get(), error) ||
                    !stage_guard.PinDirectories(stage.wstring(), error)) return result;
                break;
            }
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) return result;
            stage.clear();
        }
        if (stage.empty()) { error = L"无法创建临时解压目录。"; return result; }
        Cleanup cleanup{stage, stage_guard};
        fs::path contents = stage / L"contents";
        fs::path selection = stage / L"selection.txt";
        if (!CapturePlain(stage_guard, contents, error) || !stage_guard.EnsureDirectories(contents.wstring(), error)) return result;
        for (const auto& entry : selected) {
            if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result; }
            const fs::path member = contents / fs::path(entry.path);
            const fs::path directory = entry.is_dir ? member : member.parent_path();
            if (!CapturePlain(stage_guard, directory, error) ||
                !stage_guard.EnsureDirectories(directory.wstring(), error)) return result;
        }
        Handle selection_lock{CreateFileW(selection.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (selection_lock.value == INVALID_HANDLE_VALUE) { error = L"无法创建临时解压列表。"; return result; }
        if (!stage_guard.AdoptCreatedLeaf(selection.wstring(), selection_lock.value, error)) return result;
        for (const auto& entry : selected) if (!entry.is_dir) {
            const auto line = Encode(entry.path) + '\n';
            DWORD written = 0;
            if (line.size() > MAXDWORD || !WriteFile(selection_lock.value, line.data(),
                static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size()) {
                error = L"无法写入临时解压列表。"; return result;
            }
        }
        CloseHandle(selection_lock.value);
        selection_lock.value = CreateFileW(selection.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (selection_lock.value == INVALID_HANDLE_VALUE || !stage_guard.Check(selection.wstring(), error)) {
            error = L"临时解压列表身份无法确认。"; return result;
        }
        const bool files = std::any_of(selected.begin(), selected.end(), [](const ArchiveEntry& entry) { return !entry.is_dir; });
        if (files) {
            auto run = Run({L"x", L"-y", L"-aos", L"-spd", L"-bd", L"-sccUTF-8", L"-scsUTF-8", L"-p" + password,
                L"-o" + contents.wstring(), L"-i@" + selection.wstring(), L"--", path}, cancel);
            result.exit_code = run.exit_code;
            if (run.outcome != ArchiveOutcome::Success) {
                result.outcome = run.outcome; error = std::move(run.output); return result;
            }
        }
        for (const auto& entry : selected) {
            if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result; }
            fs::path output = target / fs::path(entry.path);
            if (!CapturePlain(destination_guard, output, error)) return result;
            if (entry.is_dir) {
                if (!destination_guard.EnsureDirectories(output.wstring(), error)) return result;
                continue;
            }
            fs::path source = contents / fs::path(entry.path);
            if (!CapturePlain(stage_guard, source, error)) return result;
#ifdef PULSE_ARCHIVE_PROCESS_TEST
            PublicationPoint(ArchivePublicationPoint::SourceCaptured, output, source);
#endif
            if (!stage_guard.PinDirectories(source.parent_path().wstring(), error) ||
                !destination_guard.EnsureDirectories(output.parent_path().wstring(), error)) return result;
            auto source_handle = stage_guard.PinLeaf(source.wstring(), error);
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!source_handle || !GetFileInformationByHandleEx(source_handle.get(), FileAttributeTagInfo,
                &attributes, sizeof(attributes)) ||
                (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                error = L"解压输出不是安全的普通文件。"; return result;
            }
#ifdef PULSE_ARCHIVE_PROCESS_TEST
            PublicationPoint(ArchivePublicationPoint::BeforePublish, output, source);
#endif
            if (cancel && cancel->load()) { result.outcome = ArchiveOutcome::Cancelled; error = L"已取消解压。"; return result; }
            bool renamed = false;
#ifdef PULSE_ARCHIVE_PROCESS_TEST
            if (!force_publication_copy)
#endif
                renamed = destination_guard.RenameLeaf(source_handle.get(), output.wstring());
            if (renamed) {
                if (!destination_guard.AdoptCreatedLeaf(output.wstring(), source_handle.get(), error)) return result;
                continue;
            }
            DWORD code = GetLastError();
#ifdef PULSE_ARCHIVE_PROCESS_TEST
            if (force_publication_copy) code = ERROR_NOT_SAME_DEVICE;
#endif
            if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) continue;
            if (code != ERROR_NOT_SAME_DEVICE) { error = L"无法写入解压文件 (" + std::to_wstring(code) + L")。"; return result; }
            bool cancelled = false;
            if (!CopyAndPublish(source, output, destination_guard, cancel, error, cancelled)) {
                if (cancelled) result.outcome = ArchiveOutcome::Cancelled;
                return result;
            }
        }
        result.outcome = ArchiveOutcome::Success;
        return result;
    } catch (const fs::filesystem_error& exception) { error = L"解压文件系统错误：" + Decode(exception.what()); return result; }
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
ArchiveOperationResult ExtractArchiveForTest(const std::wstring& path, const std::wstring& destination,
    const ArchivePublicationHook& hook, const std::atomic_bool* cancel, bool force_copy) {
    struct Restore {
        const ArchivePublicationHook* previous = publication_hook;
        bool previous_copy = force_publication_copy;
        ~Restore() { publication_hook = previous; force_publication_copy = previous_copy; }
    } restore;
    publication_hook = &hook;
    force_publication_copy = force_copy;
    return ExtractArchive(path, destination, {}, L"", cancel);
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
