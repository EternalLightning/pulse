#include "../ipc/protocol.h"
#include <cstdio>
#include <limits>

int main() {
    using namespace pulse::ipc;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
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
