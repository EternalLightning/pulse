#include "../common/json_utils.h"
#include <iostream>

int main() {
    using namespace pulse::json;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; failures += !ok;
    };
    const std::wstring nested = LR"({"child":{"key":99},"text":"\"key\":77","key":4})";
    const Document doc(nested);
    check(doc.Is(Kind::Object) && ExtractInt(doc, L"key", -1) == 4,
          "member lookup ignores nested same-key and strings containing pseudo-members");
    check(ExtractInt(LR"({"child":{"key":99}})", L"key", -1) == -1,
          "missing root member never falls through to nested member");
    check(ExtractInt(LR"({"\u006bey":1,"key":2})", L"key") == 1,
          "decoded member names and first duplicate preserve lookup contract");
    std::wstring controls;
    for (wchar_t c = 0; c < 32; ++c) controls += c;
    controls += L"\"\\/汉字";
    std::wstring escaped;
    Escape(controls, escaped);
    check(ExtractString(L"{\"text\":\"" + escaped + L"\"}", L"text") == controls,
          "writer round-trips every JSON control escape plus quote slash and Unicode");
    check(ExtractString(LR"({"text":"\b\f\n\r\t\/\"\\\u0041\u6C49"})", L"text") ==
          L"\b\f\n\r\t/\"\\A汉", "reader decodes all standard escapes and BMP Unicode");
    check(ExtractString(LR"({"text":"\uD83D\uDE00"})", L"text") == L"\U0001F600",
          "Unicode surrogate pair decodes to supplementary character");
    for (const auto* broken : {LR"({"x":"\uD800"})", LR"({"x":"\uDC00"})",
                               LR"({"x":"\uD800\u0041"})", LR"({"x":"\u00xz"})",
                               LR"({"x":"\q"})", LR"({"x":"unfinished})"})
        check(!Document(broken).Valid(), "invalid surrogate escape or truncated string rejected");
    check(ExtractInt(LR"({"x":2147483647})", L"x", 9) == 2147483647 &&
          ExtractInt(LR"({"x":-2147483648})", L"x", 9) == (-2147483647 - 1),
          "signed integer endpoints round-trip without intermediate overflow");
    check(ExtractInt(LR"({"x":2147483648})", L"x", 9) == 9 &&
          ExtractInt(LR"({"x":-2147483649})", L"x", 9) == 9 &&
          ExtractInt(LR"({"x":999999999999999999999999999})", L"x", 9) == 9,
          "integer overflow returns caller fallback");
    uint64_t value = 7;
    check(ParseUnsigned(L"18446744073709551615", value) && value == UINT64_MAX &&
          !ParseUnsigned(L"18446744073709551616", value) &&
          !ParseUnsigned(L"12junk", value) && !ParseUnsigned(L"-1", value),
          "unsigned serialized counters require complete bounded decimal tokens");
    for (const auto* broken : {LR"({"x":truefalse})", LR"({"x":false0})", LR"({"x":1junk})",
                               LR"({"x":01})", LR"({"x":1.})", LR"({"x":1e})", LR"({"x":+1})",
                               LR"({"x":1,})", LR"({"x":1 "y":2})", LR"([1,,2])", LR"({} trailing)"})
        check(!Document(broken).Valid(), "bad scalar suffix number grammar separator or trailing token rejected");
    check(Document(LR"({"x":1.25e-3,"y":null})").Valid() &&
          ExtractInt(LR"({"x":1.25e-3})", L"x", 9) == 9,
          "complete JSON numeric grammar supported while integer accessor remains typed");
    check(ExtractStringArray(LR"({"x":["a",12,"b"]})", L"x").empty(),
          "string-array accessor rejects mixed types without publishing partial values");
    const std::wstring legacy = LR"({"tags":[{"name":"legacy","rgb":0xEF4444,"paths":["C:\\old"]}]})";
    const Document legacy_doc(legacy, true);
    check(!Document(legacy).Valid() && legacy_doc.Valid() && ExtractObjectArray(legacy_doc, L"tags").size() == 1,
          "only explicit Places compatibility accepts actual legacy rgb hex output");
    check(!Document(LR"({"count":0x10})", true).Valid() &&
          !Document(LR"({"rgb":0x123456789})", true).Valid() &&
          !Document(LR"({"rgb":0x123junk})", true).Valid(),
          "legacy rgb exception retains key range and token boundaries");
    check(!Document(std::wstring(129, L'[') + L"0" + std::wstring(129, L']')).Valid() &&
          !Document(std::wstring(kMaxDocumentChars + 1, L' ')).Valid(),
          "depth and document length limits reject oversized input");
    return failures ? 1 : 0;
}
