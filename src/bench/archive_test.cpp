#include "ops/archive.h"
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <algorithm>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
using namespace pulse::ops;
namespace {
int failures = 0;
void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}
bool Pack(const fs::path& archive, const fs::path& source, const std::wstring& options) {
    fs::path exe;
#ifdef PULSE_ARCHIVE_RUNTIME_DIR
    exe = fs::path(PULSE_ARCHIVE_RUNTIME_DIR) / L"7z.exe";
#else
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, 32768);
    exe = fs::path(module).parent_path() / L"7zip/7z.exe";
#endif
    std::wstring command = L"\"" + exe.wstring() + L"\" a -y -bd " + options + L" \"" + archive.wstring() + L"\" .\\*";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, source.c_str(), &startup, &process)) return false;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 2; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); CloseHandle(process.hThread);
    return code == 0;
}
void Word(std::ofstream& stream, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) stream.put(static_cast<char>(value >> (i * 8)));
}
void UnsafeZip(const fs::path& file, const std::string& name, uint32_t attributes = 0) {
    std::ofstream stream(file, std::ios::binary);
    Word(stream, 0x04034b50, 4); Word(stream, 20, 2); Word(stream, 0, 2); Word(stream, 0, 2);
    Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, 0, 4);
    Word(stream, static_cast<uint32_t>(name.size()), 2); Word(stream, 0, 2); stream << name;
    auto offset = static_cast<uint32_t>(stream.tellp());
    Word(stream, 0x02014b50, 4); Word(stream, 0x0314, 2); Word(stream, 20, 2);
    Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 0, 4); Word(stream, 0, 4);
    Word(stream, 0, 4); Word(stream, 0, 4); Word(stream, static_cast<uint32_t>(name.size()), 2);
    Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 0, 2);
    Word(stream, attributes, 4); Word(stream, 0, 4); stream << name;
    auto size = static_cast<uint32_t>(stream.tellp()) - offset;
    Word(stream, 0x06054b50, 4); Word(stream, 0, 2); Word(stream, 0, 2); Word(stream, 1, 2);
    Word(stream, 1, 2); Word(stream, size, 4); Word(stream, offset, 4); Word(stream, 0, 2);
}
bool Junction(const fs::path& link, const fs::path& target) {
    fs::create_directories(link);
    std::wstring substitute = L"\\??\\" + target.wstring();
    struct MountPoint {
        DWORD tag; WORD length; WORD reserved;
        WORD substitute_offset; WORD substitute_length;
        WORD print_offset; WORD print_length;
        wchar_t paths[2048];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(target.wstring().size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.paths);
    auto display = target.wstring();
    std::copy(display.begin(), display.end(), data.paths + substitute.size() + 1);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.length + 8), nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(handle);
    return result;
}
}
#ifdef PULSE_ARCHIVE_PROCESS_TEST
void ProcessTests() {
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    const std::wstring fixture = (fs::path(module).parent_path() / L"pulse_archive_process_fixture.exe").wstring();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE unrelated = CreateEventW(&security, TRUE, FALSE, nullptr);
    Check(unrelated != nullptr, "create unrelated inheritable event");
    if (unrelated) {
        auto result = RunArchiveProcessForTest(fixture, {L"inherit", std::to_wstring(reinterpret_cast<uintptr_t>(unrelated))}, 1024);
        Check(result.code == 0 && WaitForSingleObject(unrelated, 0) == WAIT_TIMEOUT,
            "archive child does not inherit unrelated handles");
        CloseHandle(unrelated);
    }
    auto excessive = RunArchiveProcessForTest(fixture, {L"output"}, 1024);
    Check(excessive.code != 0 && excessive.output == L"压缩包输出超过安全大小限制。",
        "archive output limit terminates writer and fails explicitly");
    auto final = RunArchiveProcessForTest(fixture, {L"final"}, 1024);
    Check(final.code == 0 && final.output == L"final-output", "archive final output is drained after exit");
    const std::wstring ready_name = L"Local\\PulseArchiveCancel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    Check(ready != nullptr, "create archive cancellation fixture event");
    if (ready) {
        std::atomic_bool cancel{false}, launched{false};
        std::thread canceller([&]() {
            launched = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0;
            cancel = true;
        });
        auto interrupted = RunArchiveProcessForTest(fixture, {L"wait", ready_name}, 1024, &cancel);
        canceller.join();
        Check(launched && interrupted.code != 0 && interrupted.output == L"已取消解压。",
            "archive cancellation terminates a silent running child");
        CloseHandle(ready);
    }
}
#endif
int wmain(int argc, wchar_t** argv) {
#ifdef PULSE_ARCHIVE_PROCESS_TEST
    if (argc == 2 && std::wstring(argv[1]) == L"--process-only") {
        ProcessTests();
        return failures ? 1 : 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    fs::path root = fs::path(temp) / (L"PulseArchiveTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"source/子目录/空目录");
    std::ofstream(root / L"source/子目录/你好.txt") << "unicode-data";
    std::ofstream(root / L"source/top.txt") << "top-data";
    std::wstring error;
    Check(IsArchivePath(L"TEST.RAR") && IsArchivePath(L"a.ZIP") && !IsArchivePath(L"a.txt"), "archive extension detection");
    Check(IsArchivePath(L"backup.tar.gz") && IsArchivePath(L"backup.TXZ") && IsArchivePath(L"backup.cab"), "additional archive extensions");
    Check(IsSafeArchiveMember(L"子目录/你好.txt") && !IsSafeArchiveMember(L"../outside") && !IsSafeArchiveMember(L"C:/outside") && !IsSafeArchiveMember(L"a/NUL.txt") && !IsSafeArchiveMember(L"a/b:stream"), "unsafe member validation");
    for (const auto& extension : {L"zip", L"7z"}) {
        fs::path archive = root / (std::wstring(L"plain.") + extension);
        Check(Pack(archive, root / L"source", L""), "create archive");
        auto list = ListArchive(archive.wstring());
        bool unicode = false, directory = false;
        for (const auto& entry : list.entries) { if (entry.path == L"子目录/你好.txt") unicode = true; if (entry.path == L"子目录/空目录/" && entry.is_dir) directory = true; }
        Check(list.error.empty() && unicode && directory, "list unicode and empty directories");
        Check(std::any_of(list.entries.begin(), list.entries.end(), [](const ArchiveEntry& entry) {
            return entry.mtime.dwLowDateTime || entry.mtime.dwHighDateTime;
        }), "archive modification time retained");
        fs::path output = root / (std::wstring(L"selected-") + extension);
        Check(ExtractArchive(archive.wstring(), output.wstring(), {L"子目录/"}, L"", error) && fs::exists(output / L"子目录/你好.txt") && fs::is_directory(output / L"子目录/空目录") && !fs::exists(output / L"top.txt"), "recursive directory selection");
        std::ofstream(output / L"子目录/你好.txt", std::ios::trunc) << "keep";
        Check(ExtractArchive(archive.wstring(), output.wstring(), {}, L"", error) && fs::exists(output / L"top.txt"), "extract all and skip collisions");
        std::ifstream existing(output / L"子目录/你好.txt"); std::string text; existing >> text;
        Check(text == "keep", "existing content preserved");
        fs::path encrypted = root / (std::wstring(L"encrypted.") + extension);
        Check(Pack(encrypted, root / L"source", std::wstring(L"-psecret ") + (std::wstring(extension) == L"7z" ? L"-mhe=on" : L"-mem=AES256")), "create encrypted archive");
        auto locked = ListArchive(encrypted.wstring());
        Check(locked.password_required, "missing password reported without interactive prompt");
        if (std::wstring(extension) == L"zip") {
            bool member_encrypted = false;
            for (const auto& entry : locked.entries) if (!entry.is_dir && entry.encrypted) member_encrypted = true;
            Check(locked.error.empty() && member_encrypted, "encrypted zip permits browsing and flags encrypted members");
        }
        Check(!ExtractArchive(encrypted.wstring(), (root / L"wrong").wstring(), {}, L"wrong", error) && !error.empty(), "wrong password rejected");
        Check(ExtractArchive(encrypted.wstring(), (root / (std::wstring(L"correct-") + extension)).wstring(), {}, L"secret", error), "correct password extraction");
        std::atomic_bool cancel{true};
        Check(!ExtractArchive(archive.wstring(), (root / L"cancelled").wstring(), {}, L"", error, &cancel) && !fs::exists(root / L"cancelled"), "cancellation before launch");
        cancel = false;
        std::thread canceller([&cancel]() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); cancel = true; });
        auto interrupted = ListArchive(archive.wstring(), L"", &cancel);
        canceller.join();
        Check(interrupted.error == L"已取消解压。", "in-flight archive listing cancellation");
    }
    for (const auto& name : {"../outside.txt", "/outside.txt", "C:/outside.txt", "safe/../../outside.txt"}) {
        fs::path unsafe = root / L"unsafe.zip"; UnsafeZip(unsafe, name);
        Check(!ListArchive(unsafe.wstring()).error.empty() && !ExtractArchive(unsafe.wstring(), (root / L"unsafe-output").wstring(), {}, L"", error), "archive traversal rejected");
    }
    UnsafeZip(root / L"link.zip", "symlink", 0120777u << 16);
    Check(!ListArchive((root / L"link.zip").wstring()).error.empty(), "archive symbolic link rejected");
    UnsafeZip(root / L"link-zero-permissions.zip", "symlink", 0120000u << 16);
    Check(!ListArchive((root / L"link-zero-permissions.zip").wstring()).error.empty(), "symbolic link without permission bits rejected");
    fs::create_directories(root / L"outside");
    if (Junction(root / L"junction-output/子目录", root / L"outside")) {
        Check(!ExtractArchive((root / L"plain.zip").wstring(), (root / L"junction-output").wstring(), {}, L"", error) && !fs::exists(root / L"outside/你好.txt"), "destination junction traversal rejected");
        fs::remove(root / L"junction-output/子目录");
    } else std::cout << "[SKIP] destination junction test: environment cannot create a junction\n";
    std::ofstream(root / L"broken.zip") << "not an archive";
    Check(!ListArchive((root / L"broken.zip").wstring()).error.empty(), "corrupt archive error");
    if (failures) std::wcout << L"Fixtures retained at " << root.wstring() << L'\n';
    else { std::error_code ec; fs::remove_all(root, ec); }
    return failures ? 1 : 0;
}
