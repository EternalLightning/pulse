#include "../ui/name_highlight.h"
#include "../ui/address_search_layout.h"
#include "../app/search_query.h"
#include "../index/filename_pinyin.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <random>
#include <string>
#include <utility>
namespace {
using namespace pulse;
using ui::NameMatchRange;
// Frozen pre-optimization implementation for range equivalence checks.
std::vector<NameMatchRange> ReferenceNameMatchRanges(std::wstring_view name, const std::vector<index::Term>& terms) {
    if (name.empty() || terms.empty()) return {};
    std::vector<bool> mask(name.size(), false);
    const auto folded = index::Fold(name);
    for (const auto& term : terms) {
        if (!index::MatchName(name.data(), static_cast<uint32_t>(name.size()), term)) continue;
        if (term.pinyin && term.name_how == index::NameHow::Substring &&
            folded.find(term.name) == std::wstring::npos) {
            const auto match = index::FindPinyinMatch(name, term.name);
            if (match && match.start + match.length <= mask.size())
                std::fill(mask.begin() + match.start, mask.begin() + match.start + match.length, true);
        }
        for (size_t i = 0; i < term.name.size();) {
            const size_t start = i;
            if (term.name_how == index::NameHow::Wildcard) {
                while (i < term.name.size() && term.name[i] != L'*' && term.name[i] != L'?') ++i;
            } else i = term.name.size();
            const auto literal = term.name.substr(start, i - start);
            if (!literal.empty()) for (size_t at = 0; (at = folded.find(literal, at)) != std::wstring::npos; ++at) {
                std::fill(mask.begin() + at, mask.begin() + at + literal.size(), true);
            }
            if (i < term.name.size()) ++i;
        }
    }
    std::vector<NameMatchRange> result;
    for (size_t i = 0; i < mask.size();) {
        if (!mask[i]) { ++i; continue; }
        const size_t start = i;
        while (i < mask.size() && mask[i]) ++i;
        result.push_back({static_cast<UINT32>(start), static_cast<UINT32>(i - start)});
    }
    return result;
}

int failures = 0;
void Check(bool pass, const char* label) {
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!pass) ++failures;
}
void Ranges(std::wstring_view name, std::wstring_view query,
            std::initializer_list<std::pair<UINT32, UINT32>> expected, const char* label) {
    const auto terms = pulse::ui::NameHighlightTerms({}, query);
    const auto ranges = pulse::ui::NameMatchRanges(name, terms);
    bool same = ranges.size() == expected.size();
    size_t i = 0;
    for (const auto& [start, length] : expected) {
        if (i >= ranges.size() || ranges[i].start != start || ranges[i].length != length) same = false;
        ++i;
    }
    if (!same) {
        std::cout << "[INFO] actual spans:";
        for (const auto& range : ranges) std::cout << ' ' << range.start << '+' << range.length;
        std::cout << '\n';
    }
    Check(same, label);
}
bool RectInside(D2D1_RECT_F rect, D2D1_RECT_F field) {
    constexpr float epsilon = 0.01f;
    return std::isfinite(rect.left) && std::isfinite(rect.right) && std::isfinite(rect.top) && std::isfinite(rect.bottom) &&
        rect.left + epsilon >= field.left && rect.right <= field.right + epsilon &&
        rect.top + epsilon >= field.top && rect.bottom <= field.bottom + epsilon &&
        rect.right + epsilon >= rect.left && rect.bottom + epsilon >= rect.top;
}
void LayoutCase(float width, float scale, bool logical_width) {
    const float physical_width = logical_width ? width * scale : width;
    const auto field = D2D1::RectF(17.0f, 9.0f, 17.0f + physical_width, 9.0f + 36.0f * scale);
    const auto layout = pulse::ui::LayoutAddressSearch(field, scale);
    const std::array<D2D1_RECT_F, 7> ordered = {layout.scope, layout.name, layout.content, layout.input, layout.clear, layout.options, layout.close};
    bool inside = true, separated = true;
    for (size_t i = 0; i < ordered.size(); ++i) {
        inside &= RectInside(ordered[i], field);
        if (i) separated &= ordered[i - 1].right <= ordered[i].left + 0.01f;
    }
    const float input_width = (layout.input.right - layout.input.left) / scale;
    // Normal pane sizes keep at least 64 DIP; constrained physical widths must
    // still leave a usable 32 DIP edit region instead of negative geometry.
    const bool enough_input = input_width + 0.01f >= (logical_width ? 64.0f : 32.0f);
    if (!inside || !separated || !enough_input)
        std::cout << "[INFO] width=" << width << " scale=" << scale << " logical=" << logical_width
                  << " inputDIP=" << input_width << " inside=" << inside << " separated=" << separated << '\n';
    Check(inside && separated && enough_input, logical_width ? "DIP-scaled address search bounds/input" : "physical-width address search bounds/input");
}
bool SameRanges(const std::vector<NameMatchRange>& a, const std::vector<NameMatchRange>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.start == y.start && x.length == y.length; });
}
void CheckRangeEquivalence() {
    std::vector<std::wstring> names = {L"", L"aaaaa", L"Report-report.TXT", L"中国报告.txt", L"重庆音乐.txt",
        L"中国-zhongguo.txt", L"foo-文档Report2026.txt", L"\U00020000中国\U0001f600.txt", L"ÉΩÄéωä.txt",
        std::wstring(L"foo\0bar", 7), std::wstring(260, L'a')};
    std::mt19937 random(20261013);
    constexpr std::wstring_view alphabet = L"aAbB.-*?中国重庆éΩ";
    for (size_t i = 0; i < 100; ++i) {
        std::wstring name;
        for (size_t j = random() % 40; j; --j) name += alphabet[random() % alphabet.size()];
        names.push_back(std::move(name));
    }
    std::vector<std::wstring> patterns = {L"", L"a", L"aa", L"aba", L"txt", L"report", L"*", L"?", L"**?*",
        L"a*a", L"*a*", L"*a?b*", L"*.txt", L"zhongguo", L"zg", L"zgbg", L"chongqing", L"中国baogao",
        L"wendangreport2026", L"é", L"ω", L"\U00020000", std::wstring(L"o\0b", 3), std::wstring(261, L'a')};
    for (size_t i = 0; i < 60; ++i) {
        std::wstring pattern;
        for (size_t j = random() % 8; j; --j) pattern += alphabet[random() % alphabet.size()];
        patterns.push_back(index::Fold(pattern));
    }
    bool same = true;
    size_t checked = 0;
    for (const auto& name : names) {
        std::vector<index::Term> combined;
        for (auto how : {index::NameHow::Any, index::NameHow::Substring, index::NameHow::Exact, index::NameHow::Wildcard}) {
            for (const auto& pattern : patterns) for (bool pinyin : {false, true}) {
                index::Term term;
                term.name = index::Fold(pattern);
                term.name_how = how;
                term.pinyin = pinyin;
                const std::vector<index::Term> terms{term};
                same &= SameRanges(ReferenceNameMatchRanges(name, terms), ui::NameMatchRanges(name, terms));
                combined.push_back(std::move(term));
                ++checked;
            }
        }
        same &= SameRanges(ReferenceNameMatchRanges(name, combined), ui::NameMatchRanges(name, combined));
        ++checked;
    }
    std::cout << "[INFO] reference comparison cases=" << checked << '\n';
    Check(same, "optimized ranges equal original across modes, overlaps, Unicode, NUL, pinyin and combined terms");
}
void BenchmarkRanges() {
    struct Case { std::wstring name, query; };
    for (const auto& sample : std::array<Case, 3>{{
        {L"Annual-Report-Report-2026-budget.xlsx", L"report annual budget 2026 xlsx"},
        {L"报告-中国重庆音乐文档2026.txt", L"zhongguo chongqing wendang yinyue zgbg"},
        {L"long-report-annual-report-final-document.xlsx", L"*report* *document* *.xlsx"}}}) {
        const auto terms = ui::NameHighlightTerms({}, sample.query);
        auto measure = [&](auto matcher) {
            size_t checksum = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 10000; ++i) {
                const auto ranges = matcher(sample.name, terms);
                for (const auto& range : ranges) checksum += range.start + range.length;
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << " checksum=" << checksum;
            return elapsed;
        };
        const double old_ms = measure(ReferenceNameMatchRanges);
        const double new_ms = measure(ui::NameMatchRanges);
        std::cout << " original_ms=" << old_ms << " optimized_ms=" << new_ms << " speedup=" << old_ms / new_ms << '\n';
    }
}
}
int main(int argc, char** argv) {
    using namespace pulse;
    CheckRangeEquivalence();
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") BenchmarkRanges();
    Ranges(L"报告-中国.txt", L"zhongguo", {{3, 2}}, "full-pinyin highlights original Chinese characters");
    Ranges(L"中国报告.txt", L"zgbg", {{0, 4}}, "initials highlight original character span");
    Ranges(L"重庆音乐.txt", L"chongqing", {{0, 2}}, "polyphonic full-pinyin highlight");
    Ranges(L"zg-中国.txt", L"zg", {{0, 2}}, "literal occurrence takes precedence over initials");
    Ranges(L"中国-zhongguo.txt", L"zhongguo", {{3, 8}}, "literal occurrence takes precedence over full pinyin");
    Ranges(L"中国.txt", L"zhongguo nopinyin:", {}, "nopinyin disables Chinese transliteration highlight");
    Ranges(L"zhongguo-中国.txt", L"zhongguo nopinyin:", {{0, 8}}, "nopinyin preserves literal highlighting");
    Ranges(L"foo-文档Report2026.txt", L"wendangreport2026", {{4, 12}}, "mixed pinyin English digits span");
    Ranges(L"中国报告.txt", L"中国baogao", {{0, 4}}, "mixed literal Chinese and pinyin span");
    Ranges(L"中国.txt", L"!zhongguo", {}, "excluded terms are not highlighted");
    Ranges(L"中国.txt", L"path:zhongguo", {}, "path terms are not filename highlights");
    Ranges(L"中国.txt", L"\"zhongguo\"", {}, "quoted exact term stays literal");
    Ranges(L"中国.txt", L"z", {}, "one Latin letter does not highlight pinyin");
    const auto pane_terms = ui::NameHighlightTerms(L"zgbg", {});
    Check(ui::NameMatchRanges(L"中国报告.txt", pane_terms).empty(), "pane filter keeps its literal semantics");
    const auto combined_terms = ui::NameHighlightTerms(L"tag:\"设计 初稿\" 报告 ext:pdf size:>1MB -tag:归档", {});
    Check(combined_terms.size() == 1 && combined_terms.front().name == L"报告" &&
        SameRanges(ui::NameMatchRanges(L"年度报告.pdf", combined_terms), {{2, 2}}),
        "combined pane filters highlight only the filename pattern");
    Check(ui::NameHighlightTerms(L"tag:设计 ext:pdf size:>1MB", {}).empty(),
        "metadata-only pane filters do not highlight names");
    Check(ui::NameHighlightTerms(L"报告 size:broken", {}).empty(),
        "invalid pane filters do not show misleading filename highlights");
    const auto wildcard_terms = ui::NameHighlightTerms(L"tag:设计 *报告* ext:pdf", {});
    Check(wildcard_terms.size() == 1 && wildcard_terms.front().name_how == index::NameHow::Wildcard &&
        SameRanges(ui::NameMatchRanges(L"年度报告.pdf", wildcard_terms), {{2, 2}}),
        "combined pane filters retain wildcard filename highlighting");
    const auto original_ranges = ui::NameMatchRanges(L"中国报告2026.txt", ui::NameHighlightTerms({}, L"zgbg"));
    const auto visible = ui::VisibleNameMatchRanges(L"中国报告2026.txt", L"中国…txt", original_ranges);
    Check(visible.size() == 1 && visible[0].start == 0 && visible[0].length == 2,
          "truncated labels highlight visible original characters only");
    for (const auto raw : {L"zhongguo nopinyin:", L"nopinyin: zgbg ext:txt", L"zhongguo content:budget nopinyin:"}) {
        const auto restored = app::CompileSearchQuery(app::ParseSearchQuery(raw));
        const auto compiled = index::ParseQuery(restored);
        Check(!compiled.pinyin_enabled && !index::QueryHasPinyin(compiled), "advanced/saved query roundtrip preserves nopinyin");
        const auto split = app::SplitSearchQueryText(raw);
        Check(!index::ParseQuery(split.filename_needle).pinyin_enabled, "content filename split preserves nopinyin");
    }
    const auto enabled = app::CompileSearchQuery(app::ParseSearchQuery(L"zhongguo"));
    Check(index::QueryHasPinyin(index::ParseQuery(enabled)), "advanced query roundtrip preserves default pinyin");
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (float width : {180.0f, 260.0f, 340.0f, 520.0f}) {
            LayoutCase(width, scale, true);
            LayoutCase(width, scale, false);
        }
    }
    return failures ? 1 : 0;
}
