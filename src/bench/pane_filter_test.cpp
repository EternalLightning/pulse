#include "../app/app_model.h"
#include "../app/places.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cwctype>

namespace pulse::app {
std::wstring GetPulseDataDir() { return {}; }
namespace filter_reference {
// Preserve the previous implementation as an independent behavior and timing oracle.
static std::wstring EntryPathOf(const Tab& tab, const fs::DirEntry& e) {
    if (!e.full_path.empty()) return e.full_path;
    if (fs::IsVirtualPath(tab.current_path)) return {};
    std::wstring full = tab.current_path;
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    full += e.name;
    return full;
}

static std::wstring ToLowerCopy(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

static bool WildcardMatch(const std::wstring& text, const std::wstring& pat) {
    size_t si = 0, pi = 0, star = static_cast<size_t>(-1), match = 0;
    const size_t n = text.size();
    const size_t pn = pat.size();
    while (si < n) {
        if (pi < pn && pat[pi] == L'*') {
            star = pi++;
            match = si;
        } else if (pi < pn && (pat[pi] == L'?' || pat[pi] == text[si])) {
            ++si;
            ++pi;
        } else if (star != static_cast<size_t>(-1)) {
            pi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (pi < pn && pat[pi] == L'*') ++pi;
    return pi == pn;
}

bool NameMatchesPattern(std::wstring_view name, std::wstring_view needle) {
    if (needle.empty()) return true;
    const std::wstring folded_name = ToLowerCopy(std::wstring(name));
    const std::wstring folded_pat = ToLowerCopy(std::wstring(needle));
    if (folded_pat.find(L'*') != std::wstring::npos ||
        folded_pat.find(L'?') != std::wstring::npos) {
        return WildcardMatch(folded_name, folded_pat);
    }
    return folded_name.find(folded_pat) != std::wstring::npos;
}

static void ParseFilterText(const std::wstring& text, std::wstring& name_needle,
                            std::vector<std::wstring>& tag_needles) {
    name_needle.clear();
    tag_needles.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::iswspace(text[i])) ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        while (i < text.size() && !std::iswspace(text[i])) ++i;
        std::wstring tok = text.substr(start, i - start);
        if (!tok.empty() && tok[0] == L'#' && tok.size() > 1) {
            tag_needles.push_back(ToLowerCopy(tok.substr(1)));
        } else if (!tok.empty()) {
            if (!name_needle.empty()) name_needle += L' ';
            name_needle += tok;
        }
    }
    name_needle = ToLowerCopy(name_needle);
}

static bool TagMatchesNeedle(const ColorTag& tag, int index, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (ToLowerCopy(tag.name).find(needle) != std::wstring::npos) return true;
    static const wchar_t* kNicks[] = { L"红", L"橙", L"绿", L"青", L"紫", L"蓝", L"灰" };
    if (index >= 0 && index < 7 && needle == kNicks[index]) return true;
    if (index == 1 && needle == L"黄") return true;
    return false;
}

index::ContentResultStore::Filter ContentFilter(const std::wstring& text, const PlacesCatalog& places) {
    if(text.empty()) return {};
    std::wstring name; std::vector<std::wstring> tags;
    ParseFilterText(text,name,tags);
    if(name.empty() && tags.empty()) name=ToLowerCopy(text);
    std::unordered_set<std::wstring> paths;
    for(size_t id=0;id<places.tags.size();++id) for(const auto& needle:tags) {
        if(TagMatchesNeedle(places.tags[id],static_cast<int>(id),needle))
            for(const auto& path:places.tags[id].paths) paths.insert(ToLowerCopy(path));
    }
    return [name=std::move(name),has_tags=!tags.empty(),paths=std::move(paths)](const fs::DirEntry& entry) {
        return NameMatchesPattern(entry.name,name) && (!has_tags || paths.contains(ToLowerCopy(entry.full_path)));
    };
}

void CollectFilterMatches(const Tab& tab, const PlacesCatalog* places, std::vector<int>& out) {
    out.clear();
    if (!tab.snapshot) return;
    const int n = static_cast<int>(tab.EntryCount());
    if(tab.content_results) { out.reserve(n); for(int i=0;i<n;++i) out.push_back(i); return; }
    if (tab.filter_text.empty() && tab.ShowsEveryEntry()) {
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) if (tab.EntryVisible(i)) out.push_back(i);
        return;
    }
    std::wstring name_needle;
    std::vector<std::wstring> tag_needles;
    ParseFilterText(tab.filter_text, name_needle, tag_needles);
    if (name_needle.empty() && tag_needles.empty())
        name_needle = ToLowerCopy(tab.filter_text);
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const auto& e = tab.EntryAt(static_cast<size_t>(i));
        if (!tab.EntryVisible(i)) continue;
        const bool name_ok = NameMatchesPattern(e.name, name_needle);
        bool tag_ok = tag_needles.empty();
        if (!tag_ok && places) {
            const std::wstring full = EntryPathOf(tab, e);
            const auto ids = places->TagsForPath(full);
            for (int id : ids) {
                if (id < 0 || id >= static_cast<int>(places->tags.size())) continue;
                for (const auto& needle : tag_needles) {
                    if (TagMatchesNeedle(places->tags[static_cast<size_t>(id)], id, needle)) {
                        tag_ok = true;
                        break;
                    }
                }
                if (tag_ok) break;
            }
        }
        if (name_ok && tag_ok) out.push_back(i);
    }
}


} // namespace filter_reference
} // namespace pulse::app

int main(int argc, char** argv) {
    using namespace pulse;
    app::Tab tab;
    tab.current_path = L"C:\\fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    constexpr std::array names{L"Report.TXT", L"年度报告.txt", L"document.docx", L"photo.JPG",
        L"report archive.txt", L"École.txt", L"#", L"  ", L"report1.txt", L"report12.txt"};
    const bool benchmark = argc == 2 && std::string_view(argv[1]) == "--benchmark";
    const size_t count = benchmark ? 20000 : 240;
    app::PlacesCatalog places;
    places.persist = false;
    places.tags = {{L"red", L"Important Work", 0, {}}, {L"orange", L"Archive", 0, {}},
                  {L"green", L"Travel", 0, {}}};
    for (size_t i = 0; i < count; ++i) {
        fs::DirEntry entry;
        entry.name = names[i % names.size()];
        if (i % 3 == 0) entry.full_path = L"\\\\server\\share\\" + std::to_wstring(i) + L"\\" + entry.name;
        else if (i % 3 == 1) entry.full_path = L"\\\\?\\C:\\fixture\\" + std::wstring(300, L'x') +
            std::to_wstring(i) + L"\\" + entry.name;
        entry.attrs = i % 4 == 0 ? FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM
            : i % 4 == 1 ? FILE_ATTRIBUTE_HIDDEN : FILE_ATTRIBUTE_NORMAL;
        entry.is_dir = i % 5 == 0;
        const auto path = app::filter_reference::EntryPathOf(tab, entry);
        if (i % 2 == 0) places.tags[0].paths.push_back(path);
        if (i % 3 == 0) places.tags[1].paths.push_back(path);
        if (i % 7 == 0) places.tags[2].paths.push_back(path);
        entries->push_back(std::move(entry));
    }
    places.TagsReordered();
    tab.snapshot = entries;
    int failures = 0;
    size_t cases = 0;
    auto check = [&](bool passed, const char* message) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
        if (!passed) ++failures;
    };
    if (benchmark) {
        tab.show_hidden_files = true;
        tab.show_protected_os_files = true;
        for (const wchar_t* filter : {L"report", L"*.txt", L"#work", L"report #work", L"#missing"}) {
            tab.filter_text = filter;
            std::vector<int> expected, actual;
            app::filter_reference::CollectFilterMatches(tab, &places, expected);
            app::CollectFilterMatches(tab, &places, actual);
            check(actual == expected, "benchmark output matches previous implementation");
            std::array<double, 7> before{}, after{};
            for (size_t round = 0; round < before.size(); ++round) {
                const auto start = std::chrono::steady_clock::now();
                app::filter_reference::CollectFilterMatches(tab, &places, expected);
                const auto middle = std::chrono::steady_clock::now();
                app::CollectFilterMatches(tab, &places, actual);
                const auto end = std::chrono::steady_clock::now();
                before[round] = std::chrono::duration<double, std::milli>(middle - start).count();
                after[round] = std::chrono::duration<double, std::milli>(end - middle).count();
            }
            std::sort(before.begin(), before.end());
            std::sort(after.begin(), after.end());
            std::printf("filter=%ls rows=%zu matched=%zu before_ms=%.3f after_ms=%.3f\n",
                filter, count, actual.size(), before[3], after[3]);
        }
        return failures ? 1 : 0;
    }
    bool equivalent = true;
    for (bool hidden : {false, true}) for (bool system : {false, true}) {
        tab.show_hidden_files = hidden;
        tab.show_protected_os_files = system;
        for (const wchar_t* filter : {L"", L" ", L"#", L"report", L"REPORT", L"*.txt", L"report?.txt",
                L"报告", L"*报告*", L"report archive", L"#work", L"#WORK", L"#红", L"#黄", L"#绿",
                L"#missing", L"#work #archive", L"report #work", L"*.txt #黄", L"#work report #绿"}) {
            tab.filter_text = filter;
            for (const app::PlacesCatalog* catalog : {&places, static_cast<app::PlacesCatalog*>(nullptr)}) {
                std::vector<int> expected, actual{999};
                app::filter_reference::CollectFilterMatches(tab, catalog, expected);
                app::CollectFilterMatches(tab, catalog, actual);
                equivalent &= actual == expected;
                ++cases;
            }
            const auto expected = app::filter_reference::ContentFilter(filter, places);
            const auto actual = app::ContentFilter(filter, places);
            equivalent &= static_cast<bool>(actual) == static_cast<bool>(expected);
            if (actual && expected) for (const auto& entry : *entries) {
                equivalent &= actual(entry) == expected(entry);
                ++cases;
            }
        }
    }
    check(equivalent, "directory and content filters preserve names, tags, visibility, UNC and long paths");
    bool names_equivalent = true;
    for (const wchar_t* name : names) for (const wchar_t* pattern : {L"", L"*", L"?", L"REPORT", L"*.txt", L"*?*txt", L"报告"})
        names_equivalent &= app::NameMatchesPattern(name, pattern) == app::filter_reference::NameMatchesPattern(name, pattern);
    check(names_equivalent, "public name matching retains original behavior");
    std::vector<int> expected, actual;
    tab.filter_text = L"#renamed";
    places.tags[0].name = L"Renamed";
    places.TagsReordered();
    app::filter_reference::CollectFilterMatches(tab, &places, expected);
    app::CollectFilterMatches(tab, &places, actual);
    check(!actual.empty() && actual == expected, "tag rename takes effect on the next filter pass");
    tab.snapshot = std::make_shared<const std::vector<fs::DirEntry>>(1, entries->front());
    app::filter_reference::CollectFilterMatches(tab, &places, expected);
    app::CollectFilterMatches(tab, &places, actual);
    check(!actual.empty() && actual == expected, "small directory avoids eager tag preparation and preserves matches");
    tab.snapshot = std::make_shared<const std::vector<fs::DirEntry>>();
    app::CollectFilterMatches(tab, &places, actual);
    check(actual.empty(), "empty snapshot clears output");
    tab.snapshot.reset();
    actual = {999};
    app::CollectFilterMatches(tab, &places, actual);
    check(actual.empty(), "missing snapshot clears output");
    std::printf("Compared %zu filter cases; %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
