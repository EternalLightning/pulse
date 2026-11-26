// SPDX-License-Identifier: Apache-2.0
#include "terminal_process.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <wincrypt.h>

namespace pulse::ops {
namespace {

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { Reset(); }
    void Reset() {
        if (value) CloseHandle(value);
        value = nullptr;
    }
};

struct ConPty {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC create_export = GetProcAddress(kernel, "CreatePseudoConsole");
    FARPROC resize_export = GetProcAddress(kernel, "ResizePseudoConsole");
    FARPROC close_export = GetProcAddress(kernel, "ClosePseudoConsole");
    HPCON console = nullptr;

    bool Available() const { return create_export && resize_export && close_export; }
    HRESULT Create(COORD size, HANDLE in, HANDLE out) {
        return reinterpret_cast<decltype(&CreatePseudoConsole)>(create_export)(size, in, out, 0, &console);
    }
    void Resize(COORD size) const {
        reinterpret_cast<decltype(&ResizePseudoConsole)>(resize_export)(console, size);
    }
    void Close() {
        if (console) reinterpret_cast<decltype(&ClosePseudoConsole)>(close_export)(console);
        console = nullptr;
    }
    ~ConPty() { Close(); }
};

struct AttributeList {
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    ~AttributeList() { if (list) DeleteProcThreadAttributeList(list); }
    bool Set(HPCON console) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return false;
        storage.resize(bytes);
        auto candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &bytes)) return false;
        list = candidate;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            console, sizeof(console), nullptr, nullptr) != FALSE;
    }
};

std::wstring SystemShellPath(TerminalShell shell) {
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
    if (length == 0 || length >= ARRAYSIZE(system)) return {};
    return std::wstring(system, length) + (shell == TerminalShell::CommandPrompt
        ? L"\\cmd.exe" : L"\\WindowsPowerShell\\v1.0\\powershell.exe");
}

std::wstring ShellCommand(const std::wstring& shell, TerminalShell kind, const std::wstring& marker, bool isolated) {
    if (kind == TerminalShell::CommandPrompt) return L"\"" + shell + L"\" /D";
    // The title change is converted by ConHost into an OSC sequence. A new
    // title on each prompt lets the front end defer folder changes safely.
    std::wstring command = L"\"" + shell + L"\" -NoLogo -NoExit";
    if (isolated) command += L" -NoProfile";
    command += L" -Command \"";
    if (isolated) command += L"Set-PSReadLineOption -HistorySaveStyle SaveNothing -ErrorAction SilentlyContinue; ";
    return command +
        L"$global:__PulsePreviousPrompt = $function:prompt; "
        L"$global:__PulsePromptNumber = 0; function global:prompt { "
        L"$p = & $global:__PulsePreviousPrompt; "
        L"$Host.UI.RawUI.WindowTitle = '" + marker +
        L"' + (++$global:__PulsePromptNumber); $p }\"";
}

constexpr size_t kOutputCapacity = 512 * 1024;
constexpr size_t kInputCapacity = 128 * 1024;

} // namespace

bool IsPowerShell7(const std::wstring& executable) {
    const std::filesystem::path path(executable);
    if (!path.is_absolute() || _wcsicmp(path.filename().c_str(), L"pwsh.exe") != 0) return false;
    const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), nullptr);
    if (!size) return false;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(executable.c_str(), 0, size, data.data())) return false;
    void* root = nullptr;
    UINT length = 0;
    return VerQueryValueW(data.data(), L"\\", &root, &length) && length >= sizeof(VS_FIXEDFILEINFO) &&
        HIWORD(static_cast<const VS_FIXEDFILEINFO*>(root)->dwProductVersionMS) == 7;
}

std::wstring FindPowerShell7() {
    wchar_t standard[32768]{};
    const DWORD expanded = ExpandEnvironmentStringsW(L"%ProgramFiles%\\PowerShell\\7\\pwsh.exe", standard, ARRAYSIZE(standard));
    if (expanded && expanded <= ARRAYSIZE(standard) && IsPowerShell7(standard)) return standard;

    const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (!size) return {};
    std::wstring paths(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"PATH", paths.data(), size);
    if (!length || length >= size) return {};
    const std::wstring_view search(paths.data(), length);
    for (size_t start = 0; start < search.size();) {
        const auto end = search.find(L';', start);
        auto entry = search.substr(start, end == search.npos ? search.npos : end - start);
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
            entry = entry.substr(1, entry.size() - 2);
        const std::filesystem::path base(entry);
        if (base.is_absolute()) {
            auto candidate = (base / L"pwsh.exe").wstring();
            if (IsPowerShell7(candidate)) return candidate;
        }
        if (end == search.npos) break;
        start = end + 1;
    }
    return {};
}

std::string TerminalDirectoryInput(std::wstring_view directory) {
    if (directory.empty() || directory.size() > 32760) return {};
    const auto* bytes = reinterpret_cast<const BYTE*>(directory.data());
    const auto length = static_cast<DWORD>(directory.size() * sizeof(wchar_t));
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD capacity = 0;
    if (!CryptBinaryToStringA(bytes, length, flags, nullptr, &capacity)) return {};
    std::string encoded(capacity, '\0');
    if (!CryptBinaryToStringA(bytes, length, flags, encoded.data(), &capacity)) return {};
    encoded.resize(capacity);
    return " Set-Location -LiteralPath ([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" +
        encoded + "')))\r";
}

struct TerminalProcess::State {
    std::mutex mutex;
    std::condition_variable input_changed;
    std::condition_variable output_drained;
    Handle wake;
    HWND owner = nullptr;
    UINT_PTR notification_id = 0;
    bool posted = false;
    bool stopping = false;
    COORD wanted_size{};
    std::string output;
    std::string input;
    Status status = Status::Starting;

    void NotifyLocked() {
        if (!posted && owner) posted = PostMessageW(owner, kTerminalUpdated, notification_id, 0) != FALSE;
    }
    void SetStatus(Status next) {
        std::lock_guard lock(mutex);
        status = next;
        NotifyLocked();
    }
    void Read(HANDLE pipe) {
        char buffer[8192];
        DWORD count = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) && count) {
            std::unique_lock lock(mutex);
            output_drained.wait(lock, [&] { return stopping || output.size() < kOutputCapacity; });
            if (stopping && (!owner || output.size() >= kOutputCapacity)) continue;
            output.append(buffer, count);
            NotifyLocked();
        }
    }
    void Write(HANDLE pipe) {
        std::string bytes;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                input_changed.wait(lock, [&] { return stopping || !input.empty(); });
                if (stopping) return;
                bytes.swap(input);
            }
            for (size_t offset = 0; offset < bytes.size();) {
                DWORD written = 0;
                if (!WriteFile(pipe, bytes.data() + offset,
                    static_cast<DWORD>(bytes.size() - offset), &written, nullptr) || !written) return;
                offset += written;
            }
            bytes.clear();
        }
    }
    void StopIo() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        input_changed.notify_one();
        output_drained.notify_one();
        SetEvent(wake.value);
    }
    void Run(const std::wstring& directory, const std::wstring& marker, COORD size, bool isolated,
             TerminalShell kind, const std::wstring& pwsh_path) {
        Handle pty_in, pty_out, host_in, host_out;
        ConPty pty;
        auto shell = SystemShellPath(kind);
        if (kind == TerminalShell::PowerShell7) {
            shell = IsPowerShell7(pwsh_path) ? pwsh_path : FindPowerShell7();
            if (shell.empty()) {
                SetStatus(Status::ShellNotFound);
                return;
            }
        }
        if (shell.empty() || !pty.Available() ||
            !CreatePipe(&pty_in.value, &host_in.value, nullptr, 0) ||
            !CreatePipe(&host_out.value, &pty_out.value, nullptr, 0) ||
            FAILED(pty.Create(size, pty_in.value, pty_out.value))) {
            SetStatus(Status::Failed);
            return;
        }
        pty_in.Reset();
        pty_out.Reset();

        std::thread reader;
        std::thread writer;
        Handle shell_process, shell_thread, job;
        bool launched = false;
        try {
            reader = std::thread([&] { Read(host_out.value); });
            writer = std::thread([&] { Write(host_in.value); });
            AttributeList attributes;
            if (attributes.Set(pty.console)) {
                STARTUPINFOEXW startup{};
                startup.StartupInfo.cb = sizeof(startup);
                // Null standard handles request the pseudoconsole's streams
                // even if Pulse was launched with redirected input/output.
                startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startup.lpAttributeList = attributes.list;
                PROCESS_INFORMATION child{};
                auto command = ShellCommand(shell, kind, marker, isolated);
                if (CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE,
                    EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
                    directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &child)) {
                    shell_process.value = child.hProcess;
                    shell_thread.value = child.hThread;
                    job.value = CreateJobObjectW(nullptr, nullptr);
                    if (job.value) {
                        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                &limits, sizeof(limits)) ||
                            !AssignProcessToJobObject(job.value, shell_process.value)) job.Reset();
                    }
                    launched = ResumeThread(shell_thread.value) != static_cast<DWORD>(-1);
                }
            }
        } catch (...) {
            // Thread creation and startup allocation failures use the same cleanup.
        }

        if (launched) {
            SetStatus(Status::Running);
            const HANDLE waits[] = {shell_process.value, wake.value};
            for (;;) {
                if (WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
                COORD next{};
                {
                    std::lock_guard lock(mutex);
                    if (stopping) break;
                    next = wanted_size;
                }
                if (next.X != size.X || next.Y != size.Y) {
                    pty.Resize(next);
                    size = next;
                }
            }
        }
        if (job.value) job.Reset();
        else if (shell_process.value && WaitForSingleObject(shell_process.value, 0) == WAIT_TIMEOUT)
            TerminateProcess(shell_process.value, 1);
        // Wake a writer with no queued input, and release a reader behind the
        // output buffer cap before closing the console, even on old Windows.
        {
            std::lock_guard lock(mutex);
            if (owner && !stopping && launched) status = Status::Exited;
            else if (!launched) status = Status::Failed;
            NotifyLocked();
        }
        StopIo();
        pty.Close();
        if (reader.joinable()) reader.join();
        if (writer.joinable()) writer.join();
    }
};

TerminalProcess::~TerminalProcess() { Stop(); }

bool TerminalProcess::Supported() { return ConPty{}.Available(); }

bool TerminalProcess::Start(HWND window, std::wstring directory, std::wstring marker, COORD size, bool isolated,
                            TerminalShell shell, std::wstring pwsh_path) {
    Stop();
    try {
        auto state = std::make_shared<State>();
        state->wake.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!state->wake.value) return false;
        static std::atomic<UINT_PTR> next_notification_id{};
        state->notification_id = ++next_notification_id;
        state->owner = window;
        state->wanted_size = size;
        std::thread([state, directory = std::move(directory), marker = std::move(marker), size, isolated,
                     shell, pwsh_path = std::move(pwsh_path)] {
            try { state->Run(directory, marker, size, isolated, shell, pwsh_path); }
            catch (...) { state->SetStatus(Status::Failed); }
        }).detach();
        state_ = std::move(state);
    } catch (...) {
        return false;
    }
    return true;
}

void TerminalProcess::Stop() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->owner = nullptr;
    }
    state_->StopIo();
    state_.reset();
}

void TerminalProcess::SetOwner(HWND window) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (state_->owner == window) return;
    state_->owner = window;
    state_->posted = false;
    state_->NotifyLocked();
}

UINT_PTR TerminalProcess::NotificationId() const {
    return state_ ? state_->notification_id : 0;
}

TerminalProcess::Update TerminalProcess::TakeOutput() {
    if (!state_) return { {}, Status::Failed };
    Update update;
    {
        std::lock_guard lock(state_->mutex);
        state_->posted = false;
        update.status = state_->status;
        update.output.swap(state_->output);
    }
    state_->output_drained.notify_one();
    return update;
}

bool TerminalProcess::Write(std::string_view bytes) {
    if (!state_) return false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || state_->input.size() + bytes.size() > kInputCapacity) return false;
        state_->input.append(bytes);
    }
    state_->input_changed.notify_one();
    return true;
}

void TerminalProcess::Resize(COORD size) {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->wanted_size = size;
    }
    SetEvent(state_->wake.value);
}

} // namespace pulse::ops
