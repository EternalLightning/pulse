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
std::string DiagnosticUtf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    if (size) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
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
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, source.c_str(), &startup, &process)) {
        std::cerr << "[DIAG] archive fixture process creation failed: " << GetLastError() << '\n';
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 2; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess); CloseHandle(process.hThread);
    if (code != 0) std::cerr << "[DIAG] archive fixture process exit: " << code << '\n';
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

int PasswordTests() {
    std::cout << "[DIAG] starting password tests\n";
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, 32768);
    const fs::path root = fs::path(module).parent_path().parent_path() / L"bench_data" / (L"archive-password-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::cout << "[DIAG] fixture root: " << DiagnosticUtf8(root.wstring()) << '\n';
    std::error_code fixture_error;
    fs::create_directories(root / L"source", fixture_error);
    if (fixture_error) {
        std::cerr << "[DIAG] fixture setup failed: " << fixture_error.value() << '\n';
        return 1;
    }
    struct RestoreTemp {
        std::wstring tmp, temp;
        RestoreTemp() {
            wchar_t value[32768]{};
            if (GetEnvironmentVariableW(L"TMP", value, 32768)) tmp = value;
            if (GetEnvironmentVariableW(L"TEMP", value, 32768)) temp = value;
        }
        ~RestoreTemp() {
            SetEnvironmentVariableW(L"TMP", tmp.empty() ? nullptr : tmp.c_str());
            SetEnvironmentVariableW(L"TEMP", temp.empty() ? nullptr : temp.c_str());
        }
    } restore_temp;
    const auto owned_temp = root / L"temp";
    fs::create_directory(owned_temp, fixture_error);
    if (fixture_error || !SetEnvironmentVariableW(L"TMP", owned_temp.c_str()) ||
        !SetEnvironmentVariableW(L"TEMP", owned_temp.c_str())) {
        std::cerr << "[DIAG] isolated temporary directory setup failed\n";
        return 1;
    }
    std::ofstream(root / L"source/password encrypted.txt") << "password encrypted";
    for (const auto name : {L"password-missing.7z", L"Password-missing.zip", L"encrypted-missing.7z"}) {
        const auto result = ListArchive((root / name).wstring());
        Check(result.outcome == ArchiveOutcome::Failed && !result.error.empty() && result.password_state == ArchivePasswordState::Unknown,
            "missing archive with password keywords does not request a password");
    }
    const auto corrupt = root / L"encrypted password broken.7z";
    std::ofstream(corrupt) << "not an archive";
    const auto broken = ListArchive(corrupt.wstring());
    Check(broken.outcome == ArchiveOutcome::Failed && !broken.error.empty() && broken.password_state == ArchivePasswordState::Unknown,
        "corrupt archive with password keywords stays a generic failure");
    for (const auto extension : {L"7z", L"zip"}) {
        const auto archive = root / (std::wstring(L"加密 password archive.") + extension);
        Check(Pack(archive, root / L"source", std::wstring(L"-psecret ") +
            (std::wstring(extension) == L"7z" ? L"-mhe=on" : L"-mem=AES256")),
            "create password diagnostic fixture");
        const auto locked = ListArchive(archive.wstring());
        Check(std::wstring(extension) == L"7z"
            ? locked.outcome == ArchiveOutcome::Failed && locked.password_state == ArchivePasswordState::Unknown && locked.exit_code == 2u
            : locked.outcome == ArchiveOutcome::Success && locked.password_state == ArchivePasswordState::Required,
            "hidden encrypted headers remain unknown; successful metadata provides encryption evidence");
        const auto wrong = ListArchive(archive.wstring(), L"wrong");
        if (std::wstring(extension) == L"7z")
            Check(wrong.outcome == ArchiveOutcome::Failed && !wrong.error.empty() && wrong.password_state == ArchivePasswordState::Unknown && wrong.exit_code == 2u,
                "encrypted-header failure cannot classify a supplied password from human diagnostics");
        else
            Check(wrong.outcome == ArchiveOutcome::Success && wrong.error.empty() && wrong.password_state == ArchivePasswordState::Required,
                "zip metadata listing does not verify a supplied password");
        const auto opened = ListArchive(archive.wstring(), L"secret");
        Check(opened.outcome == ArchiveOutcome::Success && opened.error.empty() && opened.password_state == ArchivePasswordState::Required && opened.entries.size() == 1,
            "correct password opens archive with keyword member names");
        const auto extracted_wrong = ExtractArchive(archive.wstring(), (root / (std::wstring(L"wrong-") + extension)).wstring(), {}, L"wrong");
        Check(extracted_wrong.outcome == ArchiveOutcome::Failed && extracted_wrong.exit_code == 2u && !extracted_wrong.error.empty() &&
            extracted_wrong.password_state == (std::wstring(extension) == L"7z" ? ArchivePasswordState::Unknown : ArchivePasswordState::Required),
            "typed extraction preserves exit failure without labeling its cause as a wrong password");
        const auto extracted = ExtractArchive(archive.wstring(), (root / (std::wstring(L"correct-") + extension)).wstring(), {}, L"secret");
        if (extracted.outcome != ArchiveOutcome::Success || !extracted.error.empty())
            std::cerr << "[DIAG] extraction outcome=" << static_cast<int>(extracted.outcome)
                << " exit=" << (extracted.exit_code ? std::to_string(*extracted.exit_code) : "none") << " error=" << DiagnosticUtf8(extracted.error) << '\n';
        Check(extracted.outcome == ArchiveOutcome::Success && extracted.exit_code == 0u && extracted.error.empty() &&
            fs::exists(root / (std::wstring(L"correct-") + extension) / L"password encrypted.txt"),
            "typed extraction reports success only after the files are committed");
        std::atomic_bool cancel{true};
        const auto cancelled = ListArchive(archive.wstring(), L"", &cancel);
        Check(cancelled.outcome == ArchiveOutcome::Cancelled && !cancelled.exit_code && !cancelled.error.empty() && cancelled.password_state == ArchivePasswordState::Unknown,
            "cancelled listing does not request a password");
    }
    if (failures) std::wcout << L"Fixtures retained at " << root.wstring() << L'\n';
    else { std::error_code error; fs::remove_all(root, error); }
    return failures ? 1 : 0;
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
        if (result.outcome != ArchiveOutcome::Success || result.exit_code != 0u)
            std::cerr << "[DIAG] inherit outcome=" << static_cast<int>(result.outcome)
                << " exit=" << (result.exit_code ? std::to_string(*result.exit_code) : "none") << " output=" << DiagnosticUtf8(result.output) << '\n';
        Check(result.outcome == ArchiveOutcome::Success && result.exit_code == 0u && WaitForSingleObject(unrelated, 0) == WAIT_TIMEOUT,
            "archive child does not inherit unrelated handles");
        CloseHandle(unrelated);
    }
    auto excessive = RunArchiveProcessForTest(fixture, {L"output"}, 1024);
    Check(excessive.outcome == ArchiveOutcome::OutputLimit && excessive.exit_code.has_value() && excessive.output == L"压缩包输出超过安全大小限制。",
        "archive output limit terminates writer and fails explicitly");
    auto final = RunArchiveProcessForTest(fixture, {L"final"}, 1024);
    Check(final.outcome == ArchiveOutcome::Success && final.exit_code == 0u && final.output == L"final-output", "archive final output is drained after exit");
    const auto unavailable = RunArchiveProcessForTest(fixture + L".missing", {}, 1024);
    Check(unavailable.outcome == ArchiveOutcome::Failed && !unavailable.exit_code && !unavailable.output.empty(),
        "unstarted archive process does not fabricate an exit code 2");
    std::atomic_bool before_launch{true};
    const auto early_cancel = RunArchiveProcessForTest(fixture, {L"final"}, 1024, &before_launch);
    Check(early_cancel.outcome == ArchiveOutcome::Cancelled && !early_cancel.exit_code,
        "cancellation before launch is distinct from a process failure");
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
        Check(launched && interrupted.outcome == ArchiveOutcome::Cancelled && interrupted.exit_code != 0u && interrupted.output == L"已取消解压。",
            "archive cancellation terminates a silent running child");
        CloseHandle(ready);
    }
}
void OutcomeTests() {
    for (const auto* diagnostic : {L"ERROR: archive.7z : Cannot open encrypted archive. Wrong password?",
                                  L"错误：无法打开加密压缩包。密码错误？", L"Echec de lecture", L""}) {
        const auto result = ParseArchiveListingForTest(diagnostic, 2);
        Check(result.outcome == ArchiveOutcome::Failed && result.exit_code == 2u &&
            result.password_state == ArchivePasswordState::Unknown && result.entries.empty(),
            "exit 2 and localized or empty human diagnostics never classify password state");
    }
    const auto encrypted = ParseArchiveListingForTest(L"Path = secret.txt\nSize = 4\nEncrypted = +\n", 0);
    Check(encrypted.outcome == ArchiveOutcome::Success && encrypted.password_state == ArchivePasswordState::Required &&
        encrypted.entries.size() == 1 && encrypted.entries.front().encrypted,
        "successful slt machine encryption field provides positive evidence");
    const auto named = ParseArchiveListingForTest(L"Path = Wrong password encrypted.txt\nSize = 1\nEncrypted = -\n", 0);
    Check(named.outcome == ArchiveOutcome::Success && named.password_state == ArchivePasswordState::NotRequired &&
        named.entries.size() == 1 && !named.entries.front().encrypted,
        "member names never provide encryption or password evidence");
    const auto warning = ParseArchiveListingForTest(L"Path = partial.txt\nEncrypted = +\n", 1);
    Check(warning.outcome == ArchiveOutcome::Failed && warning.exit_code == 1u && warning.entries.empty() &&
        warning.password_state == ArchivePasswordState::Unknown,
        "a warning exit cannot publish a complete successful listing");
    const auto unsafe = ParseArchiveListingForTest(L"Path = ../outside.txt\nEncrypted = -\n", 0);
    Check(unsafe.outcome == ArchiveOutcome::Failed && unsafe.exit_code == 0u && unsafe.entries.empty() && !unsafe.error.empty(),
        "adapter rejection stays failed even when the process exit is zero");
}
#endif
int wmain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (argc == 2 && std::wstring(argv[1]) == L"--password-only") return PasswordTests();
#ifdef PULSE_ARCHIVE_PROCESS_TEST
    if (argc == 2 && std::wstring(argv[1]) == L"--outcome-only") {
        OutcomeTests();
        return failures ? 1 : 0;
    }
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
        Check(locked.password_state == ArchivePasswordState::Required ||
            (locked.outcome == ArchiveOutcome::Failed && locked.password_state == ArchivePasswordState::Unknown),
            "encrypted listing uses machine evidence or an explicit unknown failure without interactive prompt");
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
