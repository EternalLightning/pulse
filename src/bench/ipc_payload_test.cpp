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
    return failures ? 1 : 0;
}
