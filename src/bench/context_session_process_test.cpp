#include "../ipc/protocol.h"
#include "../shell_host/ctx_session_process.h"
#include <cstdio>
#include <filesystem>
#include <map>

using namespace pulse::ipc;

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

DWORD IntegrityRid(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return 0;
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &bytes);
    std::vector<uint8_t> buffer(bytes);
    if (!bytes || !GetTokenInformation(token.value, TokenIntegrityLevel, buffer.data(), bytes, &bytes)) return 0;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    return count ? *GetSidSubAuthority(label->Label.Sid, count - 1) : 0;
}

struct Fixture {
    Handle process, pipe;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> replies;
    ~Fixture() {
        if (process.value) {
            TerminateProcess(process.value, ERROR_CANCELLED);
            WaitForSingleObject(process.value, 5000);
        }
    }
    bool Start(const std::wstring& executable) {
        std::wstring command = L"\"" + executable + L"\" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return false;
        process.value = info.hProcess;
        CloseHandle(info.hThread);
        const auto name = PipeNameFor(GetCurrentProcessId());
        const auto deadline = GetTickCount64() + 8000;
        while (GetTickCount64() < deadline) {
            pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe.value != INVALID_HANDLE_VALUE) {
                ULONG server = 0;
                return GetNamedPipeServerProcessId(pipe.value, &server) && server == info.dwProcessId;
            }
            if (WaitForSingleObject(process.value, 20) != WAIT_TIMEOUT) return false;
        }
        return false;
    }
    bool Send(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload = {}) {
        MsgHeader header;
        header.type = type; header.request_id = id;
        header.payload_size = static_cast<uint32_t>(payload.size());
        PipeIoOptions options; options.pending_timeout_ms = 2000;
        return PipeWrite(pipe.value, &header, sizeof(header), options) &&
            PipeWrite(pipe.value, payload.data(), header.payload_size, options);
    }
    bool Wait(uint32_t type, uint32_t id, DWORD timeout = 12000, bool allow_partial = false,
              uint32_t minimum_items = 0) {
        const auto key = std::make_pair(type, id);
        const auto final_reply = [&] {
            const auto found = replies.find(key);
            if (found == replies.end()) return false;
            if (type != RSP_CTX_ITEMS) return true;
            PayloadReader reader(found->second.data(), found->second.size());
            uint32_t session = 0, flags = 0, count = 0;
            return reader.GetU32(session) && reader.GetU32(flags) && reader.GetU32(count) &&
                (allow_partial || !(flags & CTX_ITEMS_PARTIAL)) && count >= minimum_items;
        };
        const auto deadline = GetTickCount64() + timeout;
        while (!final_reply() && GetTickCount64() < deadline) {
            DWORD bytes = 0;
            if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &bytes, nullptr)) return false;
            if (bytes < sizeof(MsgHeader)) { Sleep(5); continue; }
            MsgHeader header{};
            PipeIoOptions options; options.pending_timeout_ms = 2000;
            if (!PipeRead(pipe.value, &header, sizeof(header), options) ||
                header.magic != kMagic || header.payload_size > kMaxPayload) return false;
            std::vector<uint8_t> payload(header.payload_size);
            if (!PipeRead(pipe.value, payload.data(), header.payload_size, options)) return false;
            replies[{header.type, header.request_id}] = std::move(payload);
        }
        return final_reply();
    }
    void Query(uint32_t id) {
        PayloadWriter writer;
        writer.PutU32(0); writer.PutU32(0);
        writer.PutStringArray({L":pulse-context-process-test:"}); writer.PutStringArray({});
        Send(REQ_CTX_QUERY, id, writer.data());
    }
    DWORD Worker(uint32_t id, uint32_t* count_out = nullptr) {
        const auto found = replies.find({RSP_CTX_ITEMS, id});
        if (found == replies.end()) return 0;
        PayloadReader reader(found->second.data(), found->second.size());
        uint32_t session = 0, flags = 0, count = 0, item = 0;
        std::wstring verb, text;
        if (!reader.GetU32(session) || !reader.GetU32(flags) || !reader.GetU32(count)) return 0;
        if (count_out) *count_out = count;
        if (!count || !reader.GetU32(item) || !reader.GetU32(flags) ||
            !reader.GetString(verb) || !reader.GetString(text)) return 0;
        return static_cast<DWORD>(_wcstoui64(text.c_str(), nullptr, 10));
    }
    void Invoke(uint32_t session, uint32_t request, DWORD worker, bool hang = false,
                bool modeless = false, bool launch = false) {
        PayloadWriter writer;
        writer.PutU32(session); writer.PutU32(launch ? 4 : modeless ? 3 : hang ? 2 : 1);
        writer.PutString(launch ? L"fixture-launch-child" : modeless ? L"fixture-modeless" :
            hang ? L"fixture-invoke-hang" : L"fixture-ready");
        writer.PutString(std::to_wstring(worker));
        Send(REQ_CTX_INVOKE, request, writer.data());
    }
    void Close(uint32_t session) {
        PayloadWriter writer; writer.PutU32(session); Send(REQ_CTX_CLOSE, 0, writer.data());
    }
    bool Succeeded(uint32_t request) {
        const auto found = replies.find({RSP_DONE, request});
        if (found == replies.end()) return false;
        PayloadReader reader(found->second.data(), found->second.size());
        uint32_t error = static_cast<uint32_t>(E_FAIL);
        return reader.GetU32(error) && SUCCEEDED(static_cast<HRESULT>(error));
    }
    bool Alive() {
        return WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT;
    }
    DWORD LaunchedChild(uint32_t request) {
        const auto found = replies.find({RSP_DONE, request});
        if (found == replies.end()) return 0;
        PayloadReader reader(found->second.data(), found->second.size());
        uint32_t result = 0, cancelled = 0;
        std::wstring child;
        if (!reader.GetU32(result) || !reader.GetU32(cancelled) || !reader.GetString(child)) return 0;
        return static_cast<DWORD>(_wcstoui64(child.c_str(), nullptr, 10));
    }
    void Diagnose(uint32_t request) {
        const auto found = replies.find({RSP_DONE, request});
        if (found == replies.end()) { std::printf("[DIAG] request %u has no DONE frame\n", request); return; }
        PayloadReader reader(found->second.data(), found->second.size());
        uint32_t result = 0, cancelled = 0;
        std::wstring error;
        if (reader.GetU32(result) && reader.GetU32(cancelled) && reader.GetString(error))
            std::printf("[DIAG] request %u HRESULT=0x%08X cancelled=%u error=%ls\n",
                request, result, cancelled, error.c_str());
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::printf("Requires isolated context worker fixture executable\n"); return 2; }
    Fixture fixture;
    if (!fixture.Start(argv[1])) { Check(false, "start isolated file-operation host fixture"); return 1; }
    std::printf("[DIAG] runner integrity RID=%lu, file-operation host RID=%lu\n",
        IntegrityRid(GetCurrentProcess()), IntegrityRid(fixture.process.value));
    fixture.Query(101);
    const auto query_started = GetTickCount64();
    Check(fixture.Wait(RSP_CTX_ITEMS, 101, 5000, true, 3),
        "hung handler does not block the first ready commands");
    const DWORD first_pid = fixture.Worker(101);
    Handle first; first.value = OpenProcess(SYNCHRONIZE, FALSE, first_pid);
    Check(first_pid && first.value && GetTickCount64() - query_started < 6000,
        "partial response retains ready commands while another handler never returns");
    Check(fixture.Wait(RSP_CTX_ITEMS, 101, 5000, true, 6),
        "a slower normal handler is published later instead of being discarded by a query deadline");
    fixture.Invoke(101, 1001, first_pid);
    Check(fixture.Wait(RSP_DONE, 1001, 5000) && fixture.Succeeded(1001),
        "ready handler invokes successfully despite the hung sibling");
    Check(first.value && WaitForSingleObject(first.value, 5000) == WAIT_OBJECT_0,
        "completed menu physically terminates its worker including the permanently blocked thread");

    fixture.Query(102);
    Check(fixture.Wait(RSP_CTX_ITEMS, 102, 5000, true, 3), "second menu starts after physical worker reclamation");
    const DWORD second_pid = fixture.Worker(102);
    Handle second; second.value = OpenProcess(SYNCHRONIZE, FALSE, second_pid);
    fixture.Close(102);
    Check(second.value && WaitForSingleObject(second.value, 5000) == WAIT_OBJECT_0,
        "closing a menu physically terminates its blocked query worker");

    fixture.Query(103);
    Check(fixture.Wait(RSP_CTX_ITEMS, 103, 5000, true, 3), "invoke hang fixture publishes ready commands");
    const DWORD third_pid = fixture.Worker(103);
    Handle third; third.value = OpenProcess(SYNCHRONIZE, FALSE, third_pid);
    Handle entered;
    const auto event_name = L"Local\\Pulse.ContextInvokeEntered." + std::to_wstring(third_pid);
    entered.value = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    fixture.Invoke(103, 1003, third_pid, true);
    Check(entered.value && WaitForSingleObject(entered.value, 5000) == WAIT_OBJECT_0,
        "production dispatch enters a permanently blocked InvokeCommand fixture");
    fixture.Close(103);
    Check(third.value && WaitForSingleObject(third.value, 5000) == WAIT_OBJECT_0 &&
        fixture.Wait(RSP_DONE, 1003, 5000) && !fixture.Succeeded(1003),
        "close reclaims a blocked invocation and reports failure");

    fixture.Query(110);
    Check(fixture.Wait(RSP_CTX_ITEMS, 110, 5000, true, 3), "modeless command fixture publishes ready commands");
    const DWORD modeless_pid = fixture.Worker(110);
    Handle modeless; modeless.value = OpenProcess(SYNCHRONIZE, FALSE, modeless_pid);
    fixture.Invoke(110, 1010, modeless_pid, false, true);
    Check(fixture.Wait(RSP_DONE, 1010, 5000) && fixture.Succeeded(1010) && modeless.value &&
        WaitForSingleObject(modeless.value, 1500) == WAIT_TIMEOUT,
        "InvokeCommand completion preserves a visible modeless window and its STA");
    const auto title = L"Pulse Context Modeless " + std::to_wstring(modeless_pid);
    HWND modeless_window = FindWindowW(L"STATIC", title.c_str());
    if (modeless_window) PostMessageW(modeless_window, WM_CLOSE, 0, 0);
    Check(modeless_window && WaitForSingleObject(modeless.value, 5000) == WAIT_OBJECT_0,
        "closing the modeless window allows physical worker reclamation");

    fixture.Query(111);
    Check(fixture.Wait(RSP_CTX_ITEMS, 111, 5000, true, 3), "application launch fixture publishes ready commands");
    const DWORD launch_pid = fixture.Worker(111);
    Handle launch; launch.value = OpenProcess(SYNCHRONIZE, FALSE, launch_pid);
    fixture.Invoke(111, 1011, launch_pid, false, false, true);
    Check(fixture.Wait(RSP_DONE, 1011, 5000) && fixture.Succeeded(1011), "context command launches an isolated child application");
    Handle application;
    application.value = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, fixture.LaunchedChild(1011));
    Check(launch.value && WaitForSingleObject(launch.value, 5000) == WAIT_OBJECT_0 && application.value &&
        WaitForSingleObject(application.value, 0) == WAIT_TIMEOUT,
        "menu worker reclamation preserves applications launched by the command");
    if (application.value) {
        TerminateProcess(application.value, ERROR_CANCELLED);
        Check(WaitForSingleObject(application.value, 5000) == WAIT_OBJECT_0, "isolated launched application cleaned");
    }

    std::vector<HANDLE> capped_workers;
    for (uint32_t id = 104; id != 108; ++id) fixture.Query(id);
    for (uint32_t id = 104; id != 108; ++id) {
        Check(fixture.Wait(RSP_CTX_ITEMS, id, 5000, true, 3), "admitted menu publishes ready commands");
        capped_workers.push_back(OpenProcess(SYNCHRONIZE, FALSE, fixture.Worker(id)));
    }
    fixture.Query(108);
    uint32_t rejected_count = 1;
    Check(fixture.Wait(RSP_CTX_ITEMS, 108, 2000) && fixture.Worker(108, &rejected_count) == 0 && rejected_count == 0,
        "process admission limit rejects a fifth live menu without spawning more workers");
    for (uint32_t id = 104; id != 108; ++id) fixture.Close(id);
    bool capped_exited = true;
    for (HANDLE worker : capped_workers) {
        capped_exited &= worker && WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0;
        if (worker) CloseHandle(worker);
    }
    Check(capped_exited, "all admitted blocked workers physically exit on close");

    const auto directory = std::filesystem::temp_directory_path() /
        (L"PulseContextProcessTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    PayloadWriter create; create.PutString(directory.wstring());
    const bool create_sent = fixture.Send(REQ_NEW_FOLDER, 2001, create.data());
    const bool create_done = create_sent && fixture.Wait(RSP_DONE, 2001, 5000);
    const bool create_succeeded = create_done && fixture.Succeeded(2001);
    const bool directory_exists = std::filesystem::is_directory(directory);
    if (!create_succeeded || !directory_exists) {
        std::printf("[DIAG] create sent=%d done=%d success=%d exists=%d path=%ls\n",
            create_sent, create_done, create_succeeded, directory_exists, directory.c_str());
        fixture.Diagnose(2001);
    }
    Check(create_succeeded && directory_exists,
        "file-operation host still creates an isolated directory after menu process termination");
    std::error_code cleanup;
    std::filesystem::remove(directory, cleanup);
    Check(!cleanup && !std::filesystem::exists(directory), "isolated fixture directory cleaned");
    Check(fixture.Send(REQ_PING, 2002) && fixture.Wait(RSP_PONG, 2002, 2000) && fixture.Alive(),
        "terminating context workers preserves the file-operation host and its pipe");

    fixture.Query(109);
    Check(fixture.Wait(RSP_CTX_ITEMS, 109, 5000, true, 3), "parent exit fixture reaches ready state");
    Handle orphan; orphan.value = OpenProcess(SYNCHRONIZE, FALSE, fixture.Worker(109));
    TerminateProcess(fixture.process.value, ERROR_CANCELLED);
    Check(orphan.value && WaitForSingleObject(orphan.value, 5000) == WAIT_OBJECT_0,
        "file-operation host exit kills outstanding menu workers through its job");
    return failures ? 1 : 0;
}
