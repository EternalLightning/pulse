#include "../ipc/protocol.h"
#include "../ipc/preview_protocol.h"
#include "../index/document_protocol.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>

namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct PipePair {
    Handle server, client;
    bool Open() {
        const auto name = L"\\\\.\\pipe\\pulse_io_test_" + std::to_wstring(GetCurrentProcessId());
        server.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr);
        if (server.value == INVALID_HANDLE_VALUE) return false;
        client.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        return client.value != INVALID_HANDLE_VALUE &&
            (ConnectNamedPipe(server.value, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED);
    }
};

template<typename Check>
void TestPipeIo(Check check) {
    using namespace pulse::ipc;
    check(kMagic == 0x53504C50 && kPreviewMagic == 0x57565250 &&
        pulse::index::document::kMagic == 0x39444350 && sizeof(MsgHeader) == 16 &&
        sizeof(PreviewRequest) == 40 && sizeof(PreviewResponse) == 80 &&
        sizeof(pulse::index::document::Request) == 72 &&
        sizeof(pulse::index::document::Begin) == 64 &&
        sizeof(pulse::index::document::Completion) == 24, "preserve protocol magic and native wire layouts");
    check(PipeRead(INVALID_HANDLE_VALUE, nullptr, 0) &&
        PipeWrite(INVALID_HANDLE_VALUE, nullptr, 0), "empty exact transfer needs no handle or buffer");
    uint8_t byte = 0;
    check(!PipeRead(INVALID_HANDLE_VALUE, &byte, 1,
        {.overlapped = false, .pending_timeout_ms = 10}) && GetLastError() == ERROR_INVALID_PARAMETER,
        "reject false timeout promises on synchronous handles");
    {
        Handle input, output, ready;
        ready.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool opened = CreatePipe(&input.value, &output.value, nullptr, 128 * 1024) && ready.value;
        check(opened, "open isolated anonymous document pipe");
        if (opened) {
            std::vector<uint8_t> expected(3 * 64 * 1024 + 17, 0x4d), actual(expected.size());
            bool written = false;
            std::thread writer([&] {
                written = PipeWrite(output.value, expected.data(), static_cast<DWORD>(expected.size()),
                    {.overlapped = false, .chunk_size = 64 * 1024, .progress_event = ready.value});
            });
            const bool notified = WaitForSingleObject(ready.value, 1000) == WAIT_OBJECT_0;
            const bool read = ReadAll(input.value, actual.data(), static_cast<DWORD>(actual.size()));
            writer.join();
            check(notified && written && read && actual == expected,
                "synchronous document chunk notifications and preview exact reads retain all bytes");
        }
    }
    PipePair pair;
    const bool connected = pair.Open();
    check(connected, "open isolated overlapped byte-mode pipe");
    if (!connected) return;
    {
        uint8_t received = 0, request = 0;
        bool read = false;
        std::thread reader([&] {
            read = PipeRead(pair.client.value, &received, 1, {.pending_timeout_ms = 1000});
        });
        const uint8_t expected = 0x63;
        const bool roundtrip = PipeWrite(pair.client.value, &expected, 1, {.pending_timeout_ms = 1000}) &&
            ReadAll(pair.server.value, &request, 1) && WriteAll(pair.server.value, &request, 1);
        reader.join();
        check(roundtrip && read && received == expected,
            "concurrent overlapped read does not starve writing on the same handle");
    }
    {
        const std::vector<uint8_t> expected{1, 2, 3, 4, 5, 6, 7, 8};
        std::vector<uint8_t> actual(expected.size());
        bool written = false;
        std::thread writer([&] {
            written = WriteAll(pair.server.value, expected.data(), 3);
            Sleep(20);
            written = written && WriteAll(pair.server.value, expected.data() + 3, 5);
        });
        const bool read = PipeRead(pair.client.value, actual.data(), static_cast<DWORD>(actual.size()),
            {.pending_timeout_ms = 1000});
        writer.join();
        check(read && written && actual == expected, "overlapped exact read joins partial chunks");
    }
    Handle cancel;
    cancel.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(cancel.value != nullptr, "create cancellation event");
    if (!cancel.value) return;
    {
        SetEvent(cancel.value);
        byte = 0xab;
        const bool cancelled = !PipeRead(pair.client.value, &byte, 1, {.cancel_event = cancel.value}) &&
            GetLastError() == ERROR_CANCELLED;
        ResetEvent(cancel.value);
        check(cancelled && byte == 0xab, "pre-signalled cancellation never starts I/O");
    }
    for (bool timeout : {false, true}) {
        byte = 0xab;
        std::thread canceller;
        if (!timeout) canceller = std::thread([&] { Sleep(20); SetEvent(cancel.value); });
        const bool read = PipeRead(pair.client.value, &byte, 1,
            {.pending_timeout_ms = timeout ? 20u : INFINITE, .cancel_event = timeout ? nullptr : cancel.value});
        const DWORD error = GetLastError();
        if (canceller.joinable()) canceller.join();
        ResetEvent(cancel.value);
        const uint8_t expected = 0x37;
        uint8_t received = 0;
        const bool reused = WriteAll(pair.server.value, &expected, 1) &&
            PipeRead(pair.client.value, &received, 1, {.pending_timeout_ms = 1000});
        check(!read && error == static_cast<DWORD>(timeout ? ERROR_TIMEOUT : ERROR_CANCELLED) &&
            reused && byte == 0xab && received == expected,
            timeout ? "timed-out read is reaped before buffer reuse; connection survives" :
                "cancelled read is reaped before buffer reuse; connection survives");
    }
    for (bool timeout : {false, true}) {
        std::vector<uint8_t> blocked(1024 * 1024, 0x58);
        std::thread canceller;
        if (!timeout) canceller = std::thread([&] { Sleep(20); SetEvent(cancel.value); });
        const bool written = PipeWrite(pair.client.value, blocked.data(), static_cast<DWORD>(blocked.size()),
            {.pending_timeout_ms = timeout ? 20u : INFINITE, .cancel_event = timeout ? nullptr : cancel.value});
        const DWORD error = GetLastError();
        if (canceller.joinable()) canceller.join();
        ResetEvent(cancel.value);
        // A cancelled write may have delivered a prefix; cancellation never
        // implies rollback and must not trigger a replay of the request.
        std::fill(blocked.begin(), blocked.end(), uint8_t{0xcc});
        DWORD available = 0;
        bool drained = PeekNamedPipe(pair.server.value, nullptr, 0, nullptr, &available, nullptr) != FALSE;
        std::vector<uint8_t> prefix(available);
        if (available) drained = drained && ReadAll(pair.server.value, prefix.data(), available);
        const uint8_t expected = 0x49;
        uint8_t received = 0;
        const bool reused = PipeWrite(pair.client.value, &expected, 1, {.pending_timeout_ms = 1000}) &&
            ReadAll(pair.server.value, &received, 1);
        check(!written && error == static_cast<DWORD>(timeout ? ERROR_TIMEOUT : ERROR_CANCELLED) && drained &&
            std::all_of(prefix.begin(), prefix.end(), [](uint8_t value) { return value == 0x58; }) &&
            reused && received == expected,
            timeout ? "timed-out blocked write is reaped without replay; connection survives" :
                "cancelled blocked write is reaped without replay; connection survives");
    }
    check(DisconnectNamedPipe(pair.server.value) != FALSE &&
        !PipeRead(pair.client.value, &byte, 1, {.pending_timeout_ms = 1000}) &&
        (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED),
        "disconnected pipe fails without reconnect or replay");
}
} // namespace

int main(int argc, char** argv) {
    using namespace pulse::ipc;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    TestPipeIo(check);
    if (argc == 2 && std::strcmp(argv[1], "--io-only") == 0) return failures ? 1 : 0;
    auto array = [&](const PayloadWriter& wire, std::vector<std::wstring>& out) {
        PayloadReader reader(wire.data().data(), wire.data().size());
        return reader.GetStringArray(out);
    };
    std::vector<std::wstring> out;
    PayloadWriter huge;
    huge.PutU32((std::numeric_limits<uint32_t>::max)());
    check(!array(huge, out) && out.empty() && out.capacity() == 0,
          "reject huge array count before allocation");
    PayloadWriter truncated;
    truncated.PutU32(2); truncated.PutString(L"present");
    check(!array(truncated, out) && out.empty(), "reject missing elements atomically");
    PayloadWriter truncated_text;
    truncated_text.PutU32(1); truncated_text.PutU32(100);
    check(!array(truncated_text, out) && out.empty(), "reject truncated string before allocation");
    PayloadWriter valid;
    const std::vector<std::wstring> expected{L"", L"中文", L"\\\\server\\share\\file.txt"};
    valid.PutStringArray(expected);
    check(array(valid, out) && out == expected, "retain valid empty Unicode and UNC entries");
    PayloadWriter empty;
    empty.PutStringArray({});
    check(array(empty, out) && out.empty(), "retain empty arrays");
    PayloadWriter too_many;
    too_many.PutU32(65537);
    for (uint32_t i = 0; i < 65537; ++i) too_many.PutString(L"");
    check(!array(too_many, out), "bound even fully framed empty elements");
    PayloadReader optional(nullptr, 0);
    check(optional.TryStringArray(out) && out.empty(), "retain omitted optional legacy arrays");
    PayloadWriter optional_bad;
    optional_bad.PutU32(2); optional_bad.PutU32(0);
    PayloadReader malformed(optional_bad.data().data(), optional_bad.data().size());
    check(!malformed.TryStringArray(out), "reject malformed optional arrays");
    // UTF-16 strings follow packed u32/u64 fields and need not be aligned.
    std::vector<uint8_t> unaligned(valid.data().size() + 1);
    std::memcpy(unaligned.data() + 1, valid.data().data(), valid.data().size());
    PayloadReader packed(unaligned.data() + 1, valid.data().size());
    check(packed.GetStringArray(out) && out == expected, "parse packed unaligned UTF-16 safely");
    PayloadWriter budget;
    const std::wstring block(1024 * 1024, L'x');
    for (int i = 0; i < 9; ++i) budget.PutString(block);
    PayloadReader limited(budget.data().data(), budget.data().size());
    std::wstring text;
    bool first_eight = true;
    for (int i = 0; i < 8; ++i) first_eight &= limited.GetString(text);
    check(first_eight && !limited.GetString(text), "bound aggregate string allocations per frame");
    const auto invocation = [&](const PayloadWriter& wire, ContextInvokePayload& parsed) {
        PayloadReader reader(wire.data().data(), wire.data().size());
        return ReadContextInvokePayload(reader, parsed);
    };
    ContextInvokePayload parsed;
    PayloadWriter invoke;
    invoke.PutU32(17); invoke.PutU32(29); invoke.PutString(L"runas"); invoke.PutString(L"管理员运行");
    check(invocation(invoke, parsed) && parsed.session == 17 && parsed.item == 29 &&
        parsed.verb == L"runas" && parsed.text == L"管理员运行", "context invoke retains complete Unicode payload");
    PayloadWriter no_strings;
    no_strings.PutU32(17); no_strings.PutU32(29);
    check(!invocation(no_strings, parsed) && !parsed.session && parsed.verb.empty(),
        "malformed context invoke never retains an executable menu command");
    PayloadWriter missing_text;
    missing_text.PutU32(17); missing_text.PutU32(29); missing_text.PutString(L"runas");
    check(!invocation(missing_text, parsed), "context invoke rejects a missing display string");
    invoke.PutU32(42);
    check(!invocation(invoke, parsed), "context invoke rejects trailing data");
    PayloadWriter embedded_nul;
    embedded_nul.PutU32(17); embedded_nul.PutU32(29);
    embedded_nul.PutString(std::wstring(L"runas\0open", 10)); embedded_nul.PutString(L"Run");
    check(!invocation(embedded_nul, parsed), "context invoke rejects embedded terminators before COM execution");
    const auto deletion = [&](const PayloadWriter& wire, DeleteResultPayload& result) {
        PayloadReader reader(wire.data().data(), wire.data().size());
        return ReadDeleteResultPayload(reader, result);
    };
    const auto delete_wire = [](const std::wstring& message, const std::wstring& failed_path) {
        PayloadWriter wire;
        wire.PutU32(static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)));
        wire.PutU32(0); wire.PutString(message);
        wire.PutStringArray({L"C:\\completed.txt"}); wire.PutStringArray({}); wire.PutStringArray({});
        wire.PutString(failed_path);
        return wire;
    };
    DeleteResultPayload result;
    const std::wstring failed_path = L"\\\\server\\share\\目录\\item; delete diagnostics: literal.txt";
    for (const auto* message : {L"Access is denied | unrelated.txt", L"拒绝访问", L"任意翻译"}) {
        auto wire = delete_wire(message, failed_path);
        check(deletion(wire, result) && result.failed_path == failed_path && result.error == message &&
            result.hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) &&
            result.deleted_paths == std::vector<std::wstring>{L"C:\\completed.txt"},
            "deletion preserves exact failure path and HRESULT independently of display message");
    }
    auto bad_tail = delete_wire(L"arbitrary", failed_path);
    bad_tail.PutU32(9);
    check(!deletion(bad_tail, result) && result.failed_path.empty() && result.deleted_paths.empty(),
        "malformed deletion tail discards all mutation and failure-path evidence");
    PayloadWriter legacy_delete;
    legacy_delete.PutU32(0); legacy_delete.PutU32(0); legacy_delete.PutString(L"");
    legacy_delete.PutStringArray({}); legacy_delete.PutStringArray({}); legacy_delete.PutStringArray({});
    check(!deletion(legacy_delete, result), "deletion requires independent failure-path field even when empty");
    PayloadWriter mismatched_delete;
    mismatched_delete.PutU32(0); mismatched_delete.PutU32(0); mismatched_delete.PutString(L"");
    mismatched_delete.PutStringArray({}); mismatched_delete.PutStringArray({L"C:\\recycled.txt"});
    mismatched_delete.PutStringArray({}); mismatched_delete.PutString(L"");
    check(!deletion(mismatched_delete, result), "deletion rejects unmatched recycle destination evidence");
    auto nul_delete = delete_wire(L"translated", std::wstring(L"C:\\one\0two", 10));
    check(!deletion(nul_delete, result), "deletion rejects embedded null failure path");
    PayloadWriter malformed_header;
    malformed_header.PutU32(0);
    check(!deletion(malformed_header, result), "deletion rejects malformed completion header");
    return failures ? 1 : 0;
}
