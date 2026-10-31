// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/filename_pinyin.h"
#include "../index/index_query.h"
#include <iostream>
#include <string>
using namespace pulse::index;
int main() {
    int failed = 0;
    auto check = [&](bool pass, const char* text) {
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << text << '\n';
        if (!pass) ++failed;
    };
    auto match = [&](std::wstring_view name, std::wstring_view query, PinyinMatchKind kind,
                     uint32_t start, uint32_t length) {
        auto result = FindPinyinMatch(name, query);
        check(result.kind == kind && result.start == start && result.length == length,
              kind == PinyinMatchKind::Full ? "full pinyin original UTF-16 span" : "initials/mixed original UTF-16 span");
    };
    match(L"2026中国报告.txt", L"zhongguo", PinyinMatchKind::Full, 4, 2);
    match(L"中国报告.txt", L"zgbg", PinyinMatchKind::Initials, 0, 4);
    match(L"重庆.txt", L"chongqing", PinyinMatchKind::Full, 0, 2);
    match(L"重庆.txt", L"zhongqing", PinyinMatchKind::Full, 0, 2);
    match(L"银行.txt", L"yinhang", PinyinMatchKind::Full, 0, 2);
    match(L"音乐.txt", L"yinyue", PinyinMatchKind::Full, 0, 2);
    match(L"文档Report2026.txt", L"wendangreport2026", PinyinMatchKind::Full, 0, 12);
    match(L"中国报告.txt", L"中国baogao", PinyinMatchKind::Full, 0, 4);
    match(L"拼音搜索.txt", L"pysousuo", PinyinMatchKind::Initials, 0, 4);
    match(L"中国.txt", L"zhongg", PinyinMatchKind::Full, 0, 2);
    check(!FindPinyinMatch(L"中国.txt", L"z"), "one-letter query stays literal");
    check(!FindPinyinMatch(L"plain.txt", L"plain"), "non-Chinese has no pinyin candidate");
    check(!FindPinyinMatch(L"中国.txt", L"zx"), "nonmatching syllables rejected");
    auto q = ParseQuery(L"zhongguo");
    check(QueryHasPinyin(q) && MatchName(L"中国.txt", 6, q.groups[0][0]), "automatic query integration");
    auto disabled = ParseQuery(L"zhongguo nopinyin:");
    check(!QueryHasPinyin(disabled) && !MatchName(L"中国.txt", 6, disabled.groups[0][0]), "nopinyin opt-out after term");
    auto exact = ParseQuery(L"\"zhongguo\"");
    check(!QueryHasPinyin(exact), "quoted exact query stays literal");
    check(!QueryCanNarrow(L"z", L"zh"), "eligibility transition cannot reuse literal-only results");
    check(!QueryCanNarrow(L"!zh", L"!zhong"), "extending excluded pinyin must re-evaluate candidates");
    const auto full_rank = ParseQuery(L"an");
    check(RankName(L"安.txt", 5, false, full_rank) > RankName(L"爱你.txt", 6, true, full_rank),
          "full pinyin outranks initials with folder bonus");
    const auto rank_query = ParseQuery(L"zg");
    int a = RankName(L"zg", 2, false, rank_query);
    int b = RankName(L"zg.txt", 6, false, rank_query);
    int c = RankName(L"xxzg.txt", 8, false, rank_query);
    int d = RankName(L"中国.txt", 6, true, rank_query);
    check(a > b && b > c && c > d, "literal exact/prefix/substring outrank initials plus folder bonus");
    std::wstring ambiguous(80, L'重');
    check(!FindPinyinMatch(ambiguous, std::wstring(79, L'c') + L'x'), "polyphonic adversarial query remains bounded");
    return failed ? 1 : 0;
}
