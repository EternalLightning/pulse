#include "../ui/dialog_text_fit.h"
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    const auto measure = [](const std::wstring& text) { return static_cast<float>(text.size()); };
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    check(FitTextEnd(L"short", 10, measure) == L"short" && FitTextEnd(L"long", 0, measure).empty(),
        "short labels remain intact and zero width draws nothing");
    check(FitTextEnd(L"long label", 5, measure) == L"long\u2026",
        "long labels end with a fitting ellipsis");
    const std::wstring unicode = L"a\U0001F4C1folder";
    const auto fitted = FitTextEnd(unicode, 3, measure);
    check(fitted == L"a\u2026", "ellipsis never splits a UTF-16 surrogate pair");
    const std::wstring local = L"C:\\first\\second\\third\\file.txt";
    const auto local_fit = FitPathMiddle(local, 20, measure);
    check(local_fit.starts_with(L"C:\\\u2026\\") && local_fit.ends_with(L"file.txt") && measure(local_fit) <= 20,
        "middle fitting preserves the drive root and final name");
    const std::wstring unc = L"\\\\server\\share\\first\\second\\file.txt";
    const auto unc_fit = FitPathMiddle(unc, 30, measure);
    check(unc_fit.starts_with(L"\\\\server\\share\\\u2026\\") && unc_fit.ends_with(L"file.txt") && measure(unc_fit) <= 30,
        "UNC fitting preserves both server and share");
    check(FitPathMiddle(L"\\\\?\\UNC\\server\\share\\first\\second\\file.txt", 30, measure) == unc_fit &&
        FitPathMiddle(L"\\\\?\\C:\\first\\second\\third\\file.txt", 20, measure) == local_fit,
        "extended local and UNC paths use the same friendly presentation");
    const auto long_leaf = FitPathMiddle(L"C:\\first\\" + std::wstring(80, L'长') + L".txt", 18, measure);
    check(long_leaf.starts_with(L"C:\\\u2026\\") && long_leaf.ends_with(L"\u2026") && measure(long_leaf) <= 18,
        "a long final folder or filename also fits the remaining space");
    check(FitPathMiddle(local, 0.5f, measure).empty(), "sub-ellipsis width does not overflow");
    check(FitPathMiddle(L"C:\\first\\second\\", 8, measure).size() <= 8,
        "trailing separators cannot escape the measured width");
    check(FitPathMiddle(L"C:/first/second/file.txt", 20, measure).starts_with(L"C:\\\u2026\\"),
        "forward slash paths are normalized only for presentation");
    return failures ? 1 : 0;
}
