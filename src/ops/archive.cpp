#include "archive.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
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
struct ProcessResult { DWORD code = 2; std::wstring output; };
ProcessResult Run(const std::vector<std::wstring>& args, const std::atomic_bool* cancel,
    const fs::path& exe = Runtime(), size_t max_output_bytes = 64 * 1024 * 1024) {
    ProcessResult result;
    if (cancel && cancel->load()) { result.output = L"已取消解压。"; return result; }
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
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
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
    GetExitCodeProcess(process.hProcess, &result.code);
    if (cancelled || output_limit) {
        result.code = 2;
        result.output = cancelled ? L"已取消解压。" : L"压缩包输出超过安全大小限制。";
    } else result.output = Decode(output);
    if (result.code != 0 && result.output.empty()) result.output = L"压缩包处理失败。";
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
bool NoReparse(const fs::path& path) {
    fs::path current;
    for (const auto& part : path) {
        current /= part;
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}
bool PasswordError(const std::wstring& output) {
    return output.find(L"password") != std::wstring::npos || output.find(L"Password") != std::wstring::npos || output.find(L"encrypted") != std::wstring::npos;
}
struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } };
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
ArchiveProcessTestResult RunArchiveProcessForTest(const std::wstring& executable,
    const std::vector<std::wstring>& args, size_t output_limit, const std::atomic_bool* cancel) {
    auto result = Run(args, cancel, fs::path(executable), output_limit);
    return {result.code, std::move(result.output)};
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
    ArchiveResult result;
    auto run = Run({L"l", L"-slt", L"-ba", L"-sccUTF-8", L"-bd", L"-p" + password, L"--", path}, cancel);
    if (run.code != 0) { result.error = run.output; result.password_required = PasswordError(run.output); return result; }
    std::wistringstream lines(run.output); std::wstring line;
    ArchiveEntry entry; bool has_path = false, unsafe = false;
    auto flush = [&]() {
        if (!has_path) return;
        entry.path = Normalize(entry.path);
        while (!entry.path.empty() && entry.path.back() == L'/') entry.path.pop_back();
        if (!SafeName(entry.path)) unsafe = true;
        if (entry.is_dir) entry.path += L'/';
        result.entries.push_back(entry); entry = {}; has_path = false;
    };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) { flush(); continue; }
        if (line.starts_with(L"Path = ")) { flush(); entry.path = line.substr(7); has_path = true; }
        else if (line.starts_with(L"Size = ")) { try { entry.size = std::stoull(line.substr(7)); } catch (...) { unsafe = true; } }
        else if (line.starts_with(L"Modified = ")) {
            SYSTEMTIME local_time{};
            FILETIME local_file_time{};
            if (swscanf_s(line.c_str() + 11, L"%hu-%hu-%hu %hu:%hu:%hu", &local_time.wYear,
                &local_time.wMonth, &local_time.wDay, &local_time.wHour, &local_time.wMinute,
                &local_time.wSecond) == 6 && SystemTimeToFileTime(&local_time, &local_file_time)) {
                LocalFileTimeToFileTime(&local_file_time, &entry.mtime);
            }
        }
        else if (line == L"Folder = +") entry.is_dir = true;
        else if (line.starts_with(L"Attributes = ")) {
            const auto value = line.substr(13);
            if (value.find(L'D') != std::wstring::npos || value.starts_with(L"d") || value.find(L" d") != std::wstring::npos) entry.is_dir = true;
            if (value.find(L'L') != std::wstring::npos || value.starts_with(L"l") || value.find(L" l") != std::wstring::npos) unsafe = true;
        } else if ((line.starts_with(L"Symbolic Link = ") && line.size() > 16) ||
            (line.starts_with(L"Hard Link = ") && line.size() > 12) ||
            (line.starts_with(L"Copy Link = ") && line.size() > 12)) unsafe = true;
        else if (line == L"Encrypted = +") { entry.encrypted = true; if (password.empty()) result.password_required = true; }
    }
    flush();
    if (unsafe) { result.entries.clear(); result.error = L"压缩包包含不安全路径或链接，已拒绝处理。"; }
    return result;
}
bool ExtractArchive(const std::wstring& path, const std::wstring& destination,
    const std::vector<std::wstring>& selected_paths, const std::wstring& password, std::wstring& error,
    const std::atomic_bool* cancel) {
    error.clear();
    // Keep the checked archive immutable until extraction finishes.
    Handle archive_lock{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (archive_lock.value == INVALID_HANDLE_VALUE) { error = L"无法读取压缩包，文件可能正在被修改。"; return false; }
    auto listing = ListArchive(path, password, cancel);
    if (!listing.error.empty()) { error = listing.error; return false; }
    try {
        fs::path target = fs::absolute(destination).lexically_normal();
        if (!NoReparse(target)) { error = L"目标路径包含链接，无法安全解压。"; return false; }
        std::vector<ArchiveEntry> selected;
        for (const auto& entry : listing.entries) {
            bool include = selected_paths.empty();
            for (auto wanted : selected_paths) {
                wanted = Normalize(wanted);
                if (entry.path == wanted || (!wanted.empty() && wanted.back() == L'/' && entry.path.starts_with(wanted))) include = true;
            }
            if (include) selected.push_back(entry);
        }
        if (selected.empty() && !selected_paths.empty()) { error = L"压缩包中找不到所选项目。"; return false; }
        wchar_t temp[MAX_PATH]{};
        DWORD temp_length = GetTempPathW(MAX_PATH, temp);
        if (!temp_length || temp_length >= MAX_PATH) { error = L"无法访问临时目录。"; return false; }
        fs::path stage;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            stage = fs::path(temp) / (L"PulseArchive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            if (fs::create_directory(stage)) break;
            stage.clear();
        }
        if (stage.empty()) { error = L"无法创建临时解压目录。"; return false; }
        Cleanup cleanup{stage};
        fs::path contents = stage / L"contents";
        fs::path selection = stage / L"selection.txt";
        { std::ofstream stream(selection, std::ios::binary); for (const auto& entry : selected) if (!entry.is_dir) stream << Encode(entry.path) << '\n'; }
        const bool files = std::any_of(selected.begin(), selected.end(), [](const ArchiveEntry& entry) { return !entry.is_dir; });
        if (files) {
            auto run = Run({L"x", L"-y", L"-aos", L"-spd", L"-bd", L"-sccUTF-8", L"-scsUTF-8", L"-p" + password,
                L"-o" + contents.wstring(), L"-i@" + selection.wstring(), L"--", path}, cancel);
            if (run.code != 0) { error = run.output; return false; }
        }
        for (const auto& entry : selected) {
            if (cancel && cancel->load()) { error = L"已取消解压。"; return false; }
            fs::path output = target / fs::path(entry.path);
            if (!NoReparse(output)) { error = L"目标路径包含链接，无法安全解压。"; return false; }
            if (entry.is_dir) { fs::create_directories(output); continue; }
            fs::path source = contents / fs::path(entry.path);
            if (!NoReparse(source) || !fs::is_regular_file(source)) { error = L"解压输出不是安全的普通文件。"; return false; }
            fs::create_directories(output.parent_path());
            if (!MoveFileExW(source.c_str(), output.c_str(), MOVEFILE_COPY_ALLOWED)) {
                DWORD code = GetLastError();
                if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) { error = L"无法写入解压文件 (" + std::to_wstring(code) + L")。"; return false; }
            }
        }
        return true;
    } catch (const fs::filesystem_error& exception) { error = L"解压文件系统错误：" + Decode(exception.what()); return false; }
}
}
