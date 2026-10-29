#include "../ipc/shell_launch_protocol.h"
#include "../ipc/file_identity_protocol.h"
#include <tlhelp32.h>
#include <shellscalingapi.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace pulse::ipc;

#pragma comment(lib, "shcore.lib")

namespace {
int failures = 0;

void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Reset(); }
    void Reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

struct OwnedChild {
    Handle process;
    ~OwnedChild() {
        if (process && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.value, ERROR_CANCELLED);
            WaitForSingleObject(process.value, 5000);
        }
    }
};

struct TempDirectory {
    std::filesystem::path path;
    bool Create() {
        std::error_code error;
        const auto temp = std::filesystem::temp_directory_path(error);
        if (error) return false;
        path = temp / (L"PulseShellLaunchTest-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
        return std::filesystem::create_directory(path, error) && !error;
    }
    bool Clean() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        return !error && !std::filesystem::exists(path, error) && !error;
    }
    ~TempDirectory() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

struct Completion {
    HRESULT result = E_FAIL;
    bool cancelled = false;
    std::wstring error;
};

DWORD PidIn(const std::wstring& text, const wchar_t* label) {
    const auto start = text.find(label);
    if (start == std::wstring::npos) return 0;
    const wchar_t* digits = text.c_str() + start + std::wcslen(label);
    wchar_t* end = nullptr;
    const auto pid = std::wcstoull(digits, &end, 10);
    return end != digits && pid && pid <= MAXDWORD ? static_cast<DWORD>(pid) : 0;
}

struct Fixture {
    Handle process, pipe;
    DWORD pid = 0;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> replies;

    ~Fixture() {
        if (process && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.value, ERROR_CANCELLED);
            WaitForSingleObject(process.value, 5000);
        }
    }
    bool Start(const std::wstring& executable) {
        std::wstring command = L"\"" + executable + L"\" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
            std::printf("[DIAG] fixture CreateProcess error=%lu\n", GetLastError());
            return false;
        }
        process.Reset(info.hProcess);
        CloseHandle(info.hThread);
        pid = info.dwProcessId;
        const auto name = PipeNameFor(GetCurrentProcessId());
        const auto deadline = GetTickCount64() + 8000;
        while (GetTickCount64() < deadline) {
            pipe.Reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            if (pipe) {
                ULONG server = 0;
                const bool authenticated = GetNamedPipeServerProcessId(pipe.value, &server) && server == pid;
                if (!authenticated) std::printf("[DIAG] pipe server=%lu expected=%lu\n", server, pid);
                return authenticated;
            }
            if (WaitForSingleObject(process.value, 20) != WAIT_TIMEOUT) break;
        }
        std::printf("[DIAG] fixture pipe unavailable error=%lu\n", GetLastError());
        return false;
    }
    bool Send(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload = {}) {
        MsgHeader header;
        header.type = type;
        header.request_id = id;
        header.payload_size = static_cast<uint32_t>(payload.size());
        PipeIoOptions options;
        options.pending_timeout_ms = 2000;
        const bool sent = PipeWrite(pipe.value, &header, sizeof(header), options) &&
            PipeWrite(pipe.value, payload.data(), header.payload_size, options);
        if (!sent) std::printf("[DIAG] send type=%u request=%u error=%lu\n", type, id, GetLastError());
        return sent;
    }
    bool Wait(uint32_t type, uint32_t id, DWORD timeout = 8000) {
        const auto key = std::make_pair(type, id);
        const auto deadline = GetTickCount64() + timeout;
        while (replies.find(key) == replies.end() && GetTickCount64() < deadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) {
                std::printf("[DIAG] peek type=%u request=%u error=%lu\n", type, id, GetLastError());
                return false;
            }
            if (available < sizeof(MsgHeader)) { Sleep(5); continue; }
            const auto read = [&](void* data, DWORD size) {
                const auto now = GetTickCount64();
                if (now >= deadline) return false;
                PipeIoOptions options;
                options.pending_timeout_ms = static_cast<DWORD>(deadline - now);
                return PipeRead(pipe.value, data, size, options);
            };
            MsgHeader header{};
            if (!read(&header, sizeof(header)) || header.magic != kMagic || header.payload_size > kMaxPayload) {
                std::printf("[DIAG] invalid/read header request=%u error=%lu\n", id, GetLastError());
                return false;
            }
            std::vector<uint8_t> payload(header.payload_size);
            if (!read(payload.data(), header.payload_size)) {
                std::printf("[DIAG] read payload request=%u error=%lu\n", id, GetLastError());
                return false;
            }
            replies[{header.type, header.request_id}] = std::move(payload);
        }
        if (replies.find(key) != replies.end()) return true;
        std::printf("[DIAG] timeout type=%u request=%u budget=%lu ms\n", type, id, timeout);
        return false;
    }
    bool Done(uint32_t type, uint32_t id, Completion& completion, DWORD timeout = 8000) {
        if (!Wait(type, id, timeout)) return false;
        const auto& payload = replies.at({type, id});
        PayloadReader reader(payload.data(), payload.size());
        uint32_t result = 0, cancelled = 0;
        std::vector<pulse::FileIdentity> produced;
        if (!reader.GetU32(result) || !reader.GetU32(cancelled) || cancelled > 1 ||
            !reader.GetString(completion.error) ||
            (type == RSP_DONE && reader.remaining() && !ReadFileIdentities(reader, produced)) ||
            reader.remaining()) {
            std::printf("[DIAG] malformed completion type=%u request=%u\n", type, id);
            return false;
        }
        completion.result = static_cast<HRESULT>(result);
        completion.cancelled = cancelled != 0;
        std::printf("[DIAG] request=%u HRESULT=0x%08lX cancelled=%u error=%ls\n",
            id, static_cast<unsigned long>(result), cancelled, completion.error.c_str());
        return true;
    }
    bool Launch(uint32_t id, const wchar_t* hook, const std::wstring& event = {},
                const std::wstring& pid_file = {}) {
        ShellLaunchRequest request;
        request.paths = {hook};
        request.parameters = event;
        request.directory = pid_file;
        return Send(REQ_SHELL_LAUNCH, id, WriteShellLaunch(request));
    }
    bool Alive() const { return process && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT; }
    bool Shutdown() {
        return Send(REQ_SHUTDOWN, 0) && WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0;
    }
};

struct WindowSearch {
    DWORD pid = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindVisibleWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search.pid || !IsWindowVisible(window)) return TRUE;
    search.window = window;
    return FALSE;
}

HWND WaitForWindow(DWORD pid, DWORD timeout = 3000) {
    WindowSearch search{pid};
    const auto deadline = GetTickCount64() + timeout;
    do {
        EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) return search.window;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return nullptr;
}

struct OwnedWindows {
    DWORD pid = 0;
    std::vector<HWND> windows;
};

BOOL CALLBACK CollectVisibleWindows(HWND window, LPARAM parameter) {
    auto& owned = *reinterpret_cast<OwnedWindows*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == owned.pid && IsWindowVisible(window)) owned.windows.push_back(window);
    return TRUE;
}

std::vector<HWND> VisibleWindows(DWORD pid) {
    OwnedWindows owned;
    owned.pid = pid;
    EnumWindows(CollectVisibleWindows, reinterpret_cast<LPARAM>(&owned));
    return owned.windows;
}

std::set<DWORD> BrokerChildren(DWORD broker_pid) {
    Handle snapshot;
    snapshot.Reset(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    std::set<DWORD> children;
    if (!snapshot) {
        std::printf("[DIAG] process snapshot error=%lu\n", GetLastError());
        return children;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (entry.th32ParentProcessID == broker_pid) children.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot.value, &entry));
    }
    return children;
}

DWORD WaitForNewWorker(const Fixture& fixture, const std::set<DWORD>& previous) {
    const auto deadline = GetTickCount64() + 5000;
    while (fixture.Alive() && GetTickCount64() < deadline) {
        for (DWORD pid : BrokerChildren(fixture.pid)) {
            if (!previous.contains(pid)) return pid;
        }
        Sleep(10);
    }
    std::printf("[DIAG] no new direct worker of broker PID=%lu\n", fixture.pid);
    return 0;
}

std::wstring Utf8ToWide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), wide.data(), size) != size) return {};
    return wide;
}

void CheckRealCommand(Fixture& fixture, const std::filesystem::path& executable,
                      const std::filesystem::path& root) {
    const auto directory = root / L"command working directory";
    const auto output = root / L"command output.txt";
    std::error_code error;
    const bool directory_created = std::filesystem::create_directory(directory, error) && !error;
    Check(directory_created, "create isolated command directory containing spaces");
    if (!directory_created) return;
    ShellLaunchRequest request;
    request.kind = ShellLaunchKind::Command;
    request.flags = 0;
    request.executable = L"\"" + executable.wstring() + L"\" --launch-test-command \"" + output.wstring() + L"\"";
    request.directory = directory.wstring();
    Completion completion;
    const bool launched = fixture.Send(REQ_SHELL_LAUNCH, 400, WriteShellLaunch(request)) &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 400, completion);
    Check(launched && SUCCEEDED(completion.result) && !completion.cancelled,
        "real command preserves quoted executable and arguments in expanded command line");
    DWORD child_pid = 0;
    std::wstring current_directory;
    const auto deadline = GetTickCount64() + 5000;
    do {
        std::ifstream input(output, std::ios::binary);
        std::string pid_line, directory_line;
        if (std::getline(input, pid_line) && std::getline(input, directory_line)) {
            child_pid = PidIn(L"PID " + Utf8ToWide(pid_line), L"PID ");
            current_directory = Utf8ToWide(directory_line);
            break;
        }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    std::printf("[DIAG] command child PID=%lu CWD=%ls expected=%ls\n",
        child_pid, current_directory.c_str(), directory.c_str());
    const DWORD worker_pid = PidIn(completion.error, L"worker PID ");
    Check(child_pid && child_pid != fixture.pid && child_pid != GetCurrentProcessId() &&
        child_pid != worker_pid && _wcsicmp(current_directory.c_str(), directory.c_str()) == 0,
        "real command child runs outside broker and UI with requested working directory");
}

void CheckMissingExecute(Fixture& fixture, const std::filesystem::path& root) {
    ShellLaunchRequest request;
    request.flags = 0;
    request.paths = {(root / L"missing-launch-target.pulse-launch-test").wstring()};
    Completion completion;
    const bool done = fixture.Send(REQ_SHELL_LAUNCH, 401, WriteShellLaunch(request)) &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 401, completion);
    wchar_t code[32]{};
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(completion.result));
    Check(done && FAILED(completion.result) && !completion.cancelled &&
        completion.error.find(code) != std::wstring::npos &&
        completion.error.find(request.paths.front()) != std::wstring::npos,
        "real execute missing path reports failing HRESULT and path without interactive error UI");
    Check(fixture.Send(REQ_PING, 402) && fixture.Wait(RSP_PONG, 402, 2000) && fixture.Alive(),
        "broker remains responsive after real execute failure");
}

void CheckInteractionIsolation(Fixture& fixture, const std::filesystem::path& root) {
    std::array<DWORD, 4> held{};
    bool ready = true;
    for (size_t i = 0; i < held.size(); ++i) {
        Completion completion;
        const auto id = 700 + static_cast<uint32_t>(i);
        ready &= fixture.Launch(id, L":pulse-shell-launch-modeless:") &&
            fixture.Done(RSP_SHELL_LAUNCH_DONE, id, completion) && SUCCEEDED(completion.result);
        held[i] = PidIn(completion.error, L"worker PID ");
        ready &= held[i] && WaitForWindow(held[i]);
    }
    Check(ready, "hold four independent viewer/property sessions");
    const auto previous = BrokerChildren(fixture.pid);
    ShellLaunchRequest request;
    request.kind = ShellLaunchKind::PickFolder;
    request.flags = 0;
    request.directory = root.wstring();
    request.title = L"Pulse independent picker fixture";
    const bool sent = fixture.Send(REQ_SHELL_LAUNCH, 710, WriteShellLaunch(request));
    const auto picker_pid = sent ? WaitForNewWorker(fixture, previous) : 0;
    const auto window = picker_pid ? WaitForWindow(picker_pid, 8000) : nullptr;
    Check(ready && picker_pid && window && !previous.contains(picker_pid),
        "folder picker opens despite all four ordinary launch slots being occupied");
    if (window) PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
    else fixture.Send(REQ_CANCEL, 710);
    Completion cancelled;
    Check(fixture.Done(RSP_SHELL_LAUNCH_DONE, 710, cancelled) && cancelled.cancelled &&
        cancelled.result == HRESULT_FROM_WIN32(ERROR_CANCELLED), "independent picker cancellation returns known cancellation");
    bool preserved = true;
    for (const auto pid : held) preserved &= pid && !VisibleWindows(pid).empty();
    Check(preserved && fixture.Send(REQ_PING, 711) && fixture.Wait(RSP_PONG, 711, 2000),
        "picker cleanup preserves all viewer sessions and broker responsiveness");
    for (const auto pid : held) for (const auto held_window : VisibleWindows(pid))
        PostMessageW(held_window, WM_CLOSE, 0, 0);
}

void CheckProperties(Fixture& fixture, uint32_t id, const std::vector<std::wstring>& paths) {
    ShellLaunchRequest request;
    request.flags = 0;
    request.verb = L"properties";
    request.paths = paths;
    const auto previous = BrokerChildren(fixture.pid);
    const bool sent = fixture.Send(REQ_SHELL_LAUNCH, id, WriteShellLaunch(request));
    const DWORD worker_pid = sent ? WaitForNewWorker(fixture, previous) : 0;
    Handle worker;
    if (worker_pid) worker.Reset(OpenProcess(SYNCHRONIZE, FALSE, worker_pid));
    const HWND first_window = worker_pid ? WaitForWindow(worker_pid, 8000) : nullptr;
    if (first_window) Sleep(250);
    const auto windows = worker_pid ? VisibleWindows(worker_pid) : std::vector<HWND>{};
    std::printf("[DIAG] real %s properties request=%u worker PID=%lu visible windows=%zu\n",
        paths.size() == 1 ? "single-file" : "multi-file", id, worker_pid, windows.size());
    for (HWND window : windows) {
        wchar_t title[256]{}, window_class[128]{};
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        GetClassNameW(window, window_class, static_cast<int>(std::size(window_class)));
        std::printf("[DIAG] properties HWND=%p class=%ls title=%ls\n",
            reinterpret_cast<void*>(window), window_class, title);
    }
    Check(sent && worker && worker_pid != fixture.pid && worker_pid != GetCurrentProcessId() &&
        first_window && windows.size() == 1 && WaitForSingleObject(worker.value, 0) == WAIT_TIMEOUT,
        paths.size() == 1 ? "single-file properties opens one visible sheet in its launch worker" :
            "multi-file properties opens one shared sheet in its launch worker");
    for (HWND window : windows) PostMessageW(window, WM_CLOSE, 0, 0);
    Completion completion;
    const bool done = sent && fixture.Done(RSP_SHELL_LAUNCH_DONE, id, completion);
    Check(done && SUCCEEDED(completion.result) && !completion.cancelled &&
        PidIn(completion.error, L"worker PID ") == worker_pid && worker &&
        WaitForSingleObject(worker.value, 8000) == WAIT_OBJECT_0,
        paths.size() == 1 ? "closing single-file properties releases its successful launch worker" :
            "closing multi-file properties releases its successful launch worker");
    bool preserved = true;
    for (const auto& path : paths) {
        std::error_code error;
        preserved &= std::filesystem::is_regular_file(path, error) && !error &&
            std::filesystem::file_size(path, error) == 0 && !error;
    }
    Check(preserved, "real properties preserves all isolated empty files");
    if (worker && WaitForSingleObject(worker.value, 0) == WAIT_TIMEOUT) fixture.Send(REQ_CANCEL, id);
}

void CheckBurstAndQueuedCancel(Fixture& fixture) {
    const auto started = GetTickCount64();
    const auto deadline = started + 15000;
    bool burst_sent = true;
    for (uint32_t id = 500; id < 508; ++id)
        burst_sent &= fixture.Launch(id, L":pulse-shell-launch-ready:");
    bool burst_completed = burst_sent;
    std::set<DWORD> workers;
    for (uint32_t id = 500; id < 508; ++id) {
        Completion completion;
        const auto now = GetTickCount64();
        const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 1;
        const bool done = fixture.Done(RSP_SHELL_LAUNCH_DONE, id, completion, remaining);
        const DWORD worker = PidIn(completion.error, L"worker PID ");
        burst_completed &= done && SUCCEEDED(completion.result) && !completion.cancelled &&
            worker && worker != fixture.pid && worker != GetCurrentProcessId();
        if (worker) workers.insert(worker);
    }
    std::printf("[DIAG] eight-launch burst elapsed=%llu ms distinct workers=%zu\n",
        GetTickCount64() - started, workers.size());
    Check(burst_completed && workers.size() == 8 && GetTickCount64() <= deadline,
        "eight queued launches all succeed in distinct workers within fifteen seconds");

    bool held_sent = true;
    for (uint32_t id = 510; id < 514; ++id)
        held_sent &= fixture.Launch(id, L":pulse-shell-launch-modeless:");
    std::array<Handle, 4> held;
    std::array<DWORD, 4> held_pids{};
    bool all_held = held_sent;
    for (size_t index = 0; index < held.size(); ++index) {
        Completion completion;
        const uint32_t id = 510 + static_cast<uint32_t>(index);
        const bool done = fixture.Done(RSP_SHELL_LAUNCH_DONE, id, completion);
        held_pids[index] = PidIn(completion.error, L"worker PID ");
        if (held_pids[index]) held[index].Reset(OpenProcess(SYNCHRONIZE, FALSE, held_pids[index]));
        const HWND window = held_pids[index] ? WaitForWindow(held_pids[index]) : nullptr;
        all_held &= done && SUCCEEDED(completion.result) && !completion.cancelled && held[index] &&
            window && WaitForSingleObject(held[index].value, 0) == WAIT_TIMEOUT;
    }
    Check(all_held, "four visible modeless launch sessions hold every active worker slot");
    const auto before_queue = BrokerChildren(fixture.pid);
    const bool queued = fixture.Launch(520, L":pulse-shell-launch-ready:");
    const bool ping = fixture.Send(REQ_PING, 521) && fixture.Wait(RSP_PONG, 521, 2000);
    const auto after_queue = BrokerChildren(fixture.pid);
    bool no_extra_worker = before_queue.size() == held.size() && after_queue.size() == held.size();
    for (DWORD pid : after_queue) no_extra_worker &= before_queue.contains(pid);
    Check(all_held && queued && ping && no_extra_worker &&
        !fixture.replies.contains({RSP_SHELL_LAUNCH_DONE, 520}),
        "fifth launch remains queued without spawning a worker while broker ping completes");
    Completion cancelled;
    Check(fixture.Send(REQ_CANCEL, 520) && fixture.Done(RSP_SHELL_LAUNCH_DONE, 520, cancelled, 3000) &&
        cancelled.result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && cancelled.cancelled,
        "queued launch cancellation reports known pre-dispatch cancellation");
    bool held_preserved = true;
    for (const auto& worker : held)
        held_preserved &= worker && WaitForSingleObject(worker.value, 0) == WAIT_TIMEOUT;
    Check(held_preserved && fixture.Alive(), "queued cancellation preserves all four active launch sessions");
    for (DWORD pid : held_pids) {
        if (!pid) continue;
        for (HWND window : VisibleWindows(pid)) PostMessageW(window, WM_CLOSE, 0, 0);
    }
    bool reclaimed = true;
    for (size_t index = 0; index < held.size(); ++index) {
        const bool exited = held[index] && WaitForSingleObject(held[index].value, 8000) == WAIT_OBJECT_0;
        reclaimed &= exited;
        if (!exited) fixture.Send(REQ_CANCEL, 510 + static_cast<uint32_t>(index));
    }
    Check(reclaimed, "closing all four held windows physically reclaims their launch workers");
}

std::vector<std::vector<uint8_t>> MalformedPayloads() {
    ShellLaunchRequest request;
    request.paths = {L":pulse-shell-launch-ready:"};
    const auto valid = WriteShellLaunch(request);
    std::vector<std::vector<uint8_t>> malformed;
    malformed.push_back(valid);
    const uint32_t wrong_version = kShellLaunchVersion + 1;
    std::memcpy(malformed.back().data(), &wrong_version, sizeof(wrong_version));
    malformed.push_back(valid);
    malformed.back().push_back(0x42);
    request.paths = {std::wstring(L"safe\0suffix", 11)};
    malformed.push_back(WriteShellLaunch(request));
    return malformed;
}

void CheckProtocol(const std::vector<std::vector<uint8_t>>& malformed) {
    ShellLaunchRequest request;
    request.paths = {L"C:\\isolated\\example.txt"};
    const auto payload = WriteShellLaunch(request);
    ShellLaunchRequest parsed;
    Check(ReadShellLaunch(payload.data(), payload.size(), parsed) && parsed.paths == request.paths &&
        parsed.kind == request.kind && parsed.flags == request.flags, "launch payload round-trips");
    const char* labels[]{"unknown launch version rejected", "launch trailing bytes rejected",
        "launch path containing embedded NUL rejected"};
    for (size_t index = 0; index < malformed.size(); ++index)
        Check(!ReadShellLaunch(malformed[index].data(), malformed[index].size(), parsed), labels[index]);
    bool strings_rejected = true;
    for (unsigned int field = 0; field < 4; ++field) {
        ShellLaunchRequest invalid = request;
        auto* value = field == 0 ? &invalid.verb : field == 1 ? &invalid.executable :
            field == 2 ? &invalid.parameters : &invalid.directory;
        *value = std::wstring(L"safe\0suffix", 11);
        const auto bytes = WriteShellLaunch(invalid);
        strings_rejected &= !ReadShellLaunch(bytes.data(), bytes.size(), parsed);
    }
    Check(strings_rejected, "all launch string fields reject embedded NUL");
}
}

int wmain(int argc, wchar_t** argv) {
    const bool protocol_only = argc == 3 && std::wstring_view(argv[2]) == L"--protocol";
    const bool interaction_only = argc == 3 && std::wstring_view(argv[2]) == L"--interactions";
    const bool startup_only = argc == 3 && std::wstring_view(argv[2]) == L"--startup-only";
    const bool commands_only = argc == 3 && std::wstring_view(argv[2]) == L"--commands-only";
    if (argc != 2 && !protocol_only && !interaction_only && !startup_only && !commands_only) {
        std::printf("Requires isolated shell-launch worker fixture executable\n");
        return 2;
    }
    const auto malformed = MalformedPayloads();
    CheckProtocol(malformed);
    if (protocol_only) return failures ? 1 : 0;
    TempDirectory temp;
    if (!temp.Create()) { Check(false, "create isolated fixture directory"); return 1; }
    Fixture fixture;
    if (!fixture.Start(argv[1])) { Check(false, "start authenticated launch broker fixture"); return 1; }
    PROCESS_DPI_AWARENESS awareness = PROCESS_DPI_UNAWARE;
    Check(SUCCEEDED(GetProcessDpiAwareness(fixture.process.value, &awareness)) &&
        awareness == PROCESS_PER_MONITOR_DPI_AWARE,
        "Shell manifest enables per-monitor DPI awareness before broker startup");
    if (startup_only) {
        Check(fixture.Send(REQ_PING, 1) && fixture.Wait(RSP_PONG, 1),
            "production Shell broker responds to authenticated ping");
        const auto folder = temp.path / L"created by production Shell";
        PayloadWriter create;
        create.PutString(folder.wstring());
        Completion completion;
        Check(fixture.Send(REQ_NEW_FOLDER, 2, create.data()) &&
            fixture.Done(RSP_DONE, 2, completion) && SUCCEEDED(completion.result) &&
            !completion.cancelled && std::filesystem::is_directory(folder),
            "production Shell creates an isolated folder through its real operation handler");
        Check(fixture.Shutdown(), "production Shell exits after broker shutdown");
        Check(temp.Clean(), "production Shell startup fixture is cleaned");
        return failures ? 1 : 0;
    }
    if (interaction_only) {
        CheckInteractionIsolation(fixture, temp.path);
        Check(fixture.Shutdown(), "interaction fixture broker exits and reclaims workers");
        Check(temp.Clean(), "interaction fixture directory cleanup");
        return failures ? 1 : 0;
    }
    if (commands_only) {
        std::error_code error;
        const auto executable = std::filesystem::absolute(std::filesystem::path(argv[1]), error);
        if (error) Check(false, "resolve real command fixture executable path");
        else CheckRealCommand(fixture, executable, temp.path);
        Check(fixture.Shutdown(), "command fixture broker exits and reclaims workers");
        Check(temp.Clean(), "command fixture directory cleanup");
        return failures ? 1 : 0;
    }
    for (uint32_t index = 0; index < malformed.size(); ++index) {
        Completion invalid;
        Check(fixture.Send(REQ_SHELL_LAUNCH, 100 + index, malformed[index]) &&
            fixture.Done(RSP_SHELL_LAUNCH_DONE, 100 + index, invalid) &&
            FAILED(invalid.result) && !invalid.cancelled,
            "broker rejects malformed launch without losing transport");
    }

    Completion ready;
    const bool ready_done = fixture.Launch(200, L":pulse-shell-launch-ready:") &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 200, ready);
    const DWORD ready_pid = PidIn(ready.error, L"worker PID ");
    Check(ready_done && SUCCEEDED(ready.result) && !ready.cancelled && ready_pid &&
        ready_pid != fixture.pid && ready_pid != GetCurrentProcessId(),
        "successful launch executes outside broker and UI process");

    Completion crashed;
    Check(fixture.Launch(201, L":pulse-shell-launch-crash:") &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 201, crashed) &&
        crashed.result == kShellLaunchOutcomeUnknown && !crashed.cancelled && fixture.Alive(),
        "worker crash reports unknown outcome and preserves broker");
    Completion recovered;
    Check(fixture.Launch(202, L":pulse-shell-launch-ready:") &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 202, recovered) && SUCCEEDED(recovered.result),
        "launch after worker crash completes successfully");

    Completion modeless_done;
    const bool modeless_ready = fixture.Launch(203, L":pulse-shell-launch-modeless:") &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 203, modeless_done);
    const DWORD modeless_pid = PidIn(modeless_done.error, L"worker PID ");
    Handle modeless;
    if (modeless_pid) modeless.Reset(OpenProcess(SYNCHRONIZE, FALSE, modeless_pid));
    const HWND modeless_window = modeless_pid ? WaitForWindow(modeless_pid) : nullptr;
    Check(modeless_ready && SUCCEEDED(modeless_done.result) && modeless && modeless_window &&
        WaitForSingleObject(modeless.value, 3500) == WAIT_TIMEOUT,
        "visible modeless session survives completion and idle grace");

    const auto event_name = L"Local\\Pulse.ShellLaunchTest.Entered." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const auto pid_file = temp.path / L"hung-worker.pid";
    Handle entered;
    entered.Reset(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    const bool hang_sent = entered && fixture.Launch(204, L":pulse-shell-launch-hang:",
        event_name, pid_file.wstring());
    const bool hang_entered = hang_sent && WaitForSingleObject(entered.value, 5000) == WAIT_OBJECT_0;
    DWORD hung_pid = 0;
    if (hang_entered) { std::ifstream input(pid_file); input >> hung_pid; }
    Handle hung;
    if (hung_pid) hung.Reset(OpenProcess(SYNCHRONIZE, FALSE, hung_pid));
    Check(hang_entered && hung && WaitForSingleObject(hung.value, 0) == WAIT_TIMEOUT,
        "fixture enters a permanently hung launch in a live worker");
    Check(fixture.Send(REQ_PING, 300) && fixture.Wait(RSP_PONG, 300, 2000),
        "hung launch leaves broker ping responsive");
    const auto created_path = temp.path / L"created-during-hang.txt";
    PayloadWriter create;
    create.PutString(created_path.wstring());
    Completion created;
    std::error_code path_error;
    Check(fixture.Send(REQ_NEW_FILE, 301, create.data()) && fixture.Done(RSP_DONE, 301, created, 3000) &&
        SUCCEEDED(created.result) && std::filesystem::is_regular_file(created_path, path_error),
        "hung launch does not block independent file creation");
    Completion cancelled;
    Check(fixture.Send(REQ_CANCEL, 204) && fixture.Done(RSP_SHELL_LAUNCH_DONE, 204, cancelled, 5000) &&
        cancelled.result == kShellLaunchOutcomeUnknown && cancelled.cancelled && hung &&
        WaitForSingleObject(hung.value, 5000) == WAIT_OBJECT_0,
        "cancellation reclaims only the dispatched launch and reports unknown outcome");
    Check(fixture.Alive() && modeless && WaitForSingleObject(modeless.value, 0) == WAIT_TIMEOUT &&
        modeless_window && IsWindow(modeless_window) && std::filesystem::is_regular_file(created_path, path_error),
        "launch cancellation preserves broker, another launch session and created file");
    if (modeless_window) PostMessageW(modeless_window, WM_CLOSE, 0, 0);
    Check(modeless && WaitForSingleObject(modeless.value, 8000) == WAIT_OBJECT_0,
        "closing modeless window releases its launch worker");

    const auto concurrent_path = temp.path / L"concurrent.txt";
    PayloadWriter concurrent_create;
    concurrent_create.PutString(concurrent_path.wstring());
    Completion concurrent_launch, concurrent_file;
    Check(fixture.Launch(205, L":pulse-shell-launch-ready:") &&
        fixture.Send(REQ_NEW_FILE, 302, concurrent_create.data()) &&
        fixture.Done(RSP_DONE, 302, concurrent_file, 3000) &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 205, concurrent_launch) &&
        SUCCEEDED(concurrent_file.result) && SUCCEEDED(concurrent_launch.result) &&
        std::filesystem::is_regular_file(concurrent_path, path_error),
        "concurrent file creation and launch both complete independently");

    const auto fixture_executable = std::filesystem::absolute(std::filesystem::path(argv[1]), path_error);
    if (path_error) Check(false, "resolve real command fixture executable path");
    else CheckRealCommand(fixture, fixture_executable, temp.path);
    CheckMissingExecute(fixture, temp.path);
    const auto property_file = temp.path / L"single properties.txt";
    const auto second_property_file = temp.path / L"second properties.txt";
    bool property_files_created = false;
    {
        std::ofstream first(property_file, std::ios::binary | std::ios::trunc);
        std::ofstream second(second_property_file, std::ios::binary | std::ios::trunc);
        property_files_created = first.good() && second.good();
    }
    Check(property_files_created, "create isolated empty real properties fixtures");
    if (property_files_created) {
        CheckProperties(fixture, 403, {property_file.wstring()});
        CheckProperties(fixture, 404, {property_file.wstring(), second_property_file.wstring()});
    }

    Completion child_done;
    const auto child_worker_file = temp.path / L"child-worker.pid";
    const bool child_launched = fixture.Launch(206, L":pulse-shell-launch-child:", {}, child_worker_file.wstring()) &&
        fixture.Done(RSP_SHELL_LAUNCH_DONE, 206, child_done);
    DWORD worker_pid = PidIn(child_done.error, L"worker PID ");
    DWORD child_pid = PidIn(child_done.error, L"child PID ");
    if (!worker_pid) { std::ifstream input(child_worker_file); input >> worker_pid; }
    if (!child_pid) {
        std::ifstream input(std::filesystem::path(child_worker_file.wstring() + L".child"));
        input >> child_pid;
    }
    Handle launch_worker;
    OwnedChild child;
    if (worker_pid) launch_worker.Reset(OpenProcess(SYNCHRONIZE, FALSE, worker_pid));
    if (child_pid && child_pid != fixture.pid && child_pid != GetCurrentProcessId() && child_pid != worker_pid)
        child.process.Reset(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, child_pid));
    Check(child_launched && SUCCEEDED(child_done.result) && launch_worker && child.process &&
        WaitForSingleObject(launch_worker.value, 8000) == WAIT_OBJECT_0 &&
        WaitForSingleObject(child.process.value, 0) == WAIT_TIMEOUT,
        "launched application survives physical launch worker retirement");
    CheckBurstAndQueuedCancel(fixture);
    const bool shutdown = fixture.Shutdown();
    Check(shutdown && child.process && WaitForSingleObject(child.process.value, 1000) == WAIT_TIMEOUT,
        "launched application survives broker shutdown");
    if (child.process) {
        const bool terminated = TerminateProcess(child.process.value, ERROR_CANCELLED) != FALSE;
        Check(terminated && WaitForSingleObject(child.process.value, 5000) == WAIT_OBJECT_0,
            "owned persistent fixture child cleaned");
    }
    Check(temp.Clean(), "isolated launch fixture files cleaned");
    return failures ? 1 : 0;
}
