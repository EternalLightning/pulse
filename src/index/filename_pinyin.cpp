#include "filename_pinyin.h"
#include "index_query.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>
namespace pinyin {
template<uint16_t N> struct PinyinCombination { uint16_t n; uint16_t pinyin[N]; };
struct PinyinRange { char32_t begin, end; const uint16_t* table; };
}
#include "../../third_party/ib-pinyin-cpp/ascii_data.inc"
#undef P
#undef F
namespace pulse::index {
namespace {
char32_t ReadCharacter(std::wstring_view text, size_t pos, size_t& width) {
    char32_t c = text[pos];
    width = 1;
    if (c >= 0xd800 && c <= 0xdbff && pos + 1 < text.size()) {
        const char32_t lo = text[pos + 1];
        if (lo >= 0xdc00 && lo <= 0xdfff) {
            c = 0x10000 + ((c - 0xd800) << 10) + lo - 0xdc00;
            width = 2;
        }
    }
    return c;
}
uint16_t PronunciationIndex(char32_t c) {
    for (const auto& range : pinyin::pinyin_ranges)
        if (c >= range.begin && c <= range.end) return range.table[c - range.begin];
    return 65535;
}
template<class F> void ReadingsByIndex(uint16_t idx, F&& fn) {
    if (idx == 65535) return;
    if (idx < 1514) fn(pinyin::pinyins[idx]);
    else if (idx - 1514 < 1104) {
        const auto& combination = pinyin::pinyin_combinations[idx - 1514];
        for (uint16_t i = 0; i < combination.n; ++i) fn(pinyin::pinyins[combination.pinyin[i]]);
    }
}
template<class F> void Readings(char32_t c, F&& fn) {
    ReadingsByIndex(PronunciationIndex(c), std::forward<F>(fn));
}
bool Latin(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
// Each character is one transition. Merge equal query offsets instead of expanding
// polyphonic filenames into a Cartesian product. Space is O(query length).
PinyinMatch Match(std::wstring_view name, std::wstring_view query, bool initials) {
    const size_t absent = static_cast<size_t>(-1);
    std::vector<size_t> current(query.size() + 1, absent), next(query.size() + 1, absent);
    for (size_t pos = 0; pos < name.size();) {
        size_t width;
        const char32_t c = ReadCharacter(name, pos, width);
        const uint16_t pronunciation = PronunciationIndex(c);
        current[0] = pos;
        std::fill(next.begin(), next.end(), absent);
        auto advance = [&](size_t offset, size_t count) {
            next[offset + count] = (std::min)(next[offset + count], current[offset]);
        };
        for (size_t offset = 0; offset < query.size(); ++offset) {
            if (current[offset] == absent) continue;
            if (width <= query.size() - offset) {
                bool same = true;
                for (size_t j = 0; j < width; ++j)
                    if (FoldChar(name[pos + j]) != FoldChar(query[offset + j])) same = false;
                if (same) advance(offset, width);
            }
            ReadingsByIndex(pronunciation, [&](std::string_view reading) {
                if (initials && !reading.empty() && FoldChar(query[offset]) == static_cast<wchar_t>(reading[0])) advance(offset, 1);
                const size_t count = (std::min)(reading.size(), query.size() - offset);
                if (!count) return;
                for (size_t j = 0; j < count; ++j)
                    if (FoldChar(query[offset + j]) != static_cast<wchar_t>(reading[j])) return;
                advance(offset, count);
            });
        }
        if (next[query.size()] != absent)
            return {initials ? PinyinMatchKind::Initials : PinyinMatchKind::Full,
                    static_cast<uint32_t>(next[query.size()]),
                    static_cast<uint32_t>(pos + width - next[query.size()])};
        current.swap(next);
        pos += width;
    }
    return {};
}
}
bool PinyinEligible(std::wstring_view query) {
    bool previous = false;
    for (wchar_t c : query) {
        const bool latin = Latin(c);
        if (latin && previous) return true;
        previous = latin;
    }
    return false;
}
bool HasPinyinCharacters(std::wstring_view name) {
    for (size_t i = 0; i < name.size();) {
        size_t width;
        if (PronunciationIndex(ReadCharacter(name, i, width)) != 65535) return true;
        i += width;
    }
    return false;
}
uint32_t PinyinCandidateLetters(std::wstring_view name) {
    uint32_t mask = 0;
    for (size_t i = 0; i < name.size();) {
        size_t width;
        const auto c = ReadCharacter(name, i, width);
        if (c < 128 && Latin(static_cast<wchar_t>(c))) mask |= 1u << (FoldChar(static_cast<wchar_t>(c)) - L'a');
        Readings(c, [&](std::string_view r) {
            if (!r.empty() && r[0] >= 'a' && r[0] <= 'z') mask |= 1u << (r[0] - 'a');
        });
        i += width;
    }
    return mask;
}
PinyinPairMask PinyinCandidatePairs(std::wstring_view name) {
    PinyinPairMask pairs{};
    uint32_t previous_ends = 0;
    auto add_pair = [&](unsigned first, unsigned second) {
        const unsigned index = first * 26 + second;
        pairs[index / 64] |= 1ull << (index % 64);
    };
    for (size_t pos = 0; pos < name.size();) {
        size_t width;
        const auto c = ReadCharacter(name, pos, width);
        uint32_t starts = 0, ends = 0;
        if (c < 128 && Latin(static_cast<wchar_t>(c))) {
            const unsigned letter = static_cast<unsigned>(FoldChar(static_cast<wchar_t>(c)) - L'a');
            starts = ends = 1u << letter;
        }
        Readings(c, [&](std::string_view reading) {
            if (reading.empty()) return;
            starts |= 1u << (reading.front() - 'a');
            // The same character may emit a complete syllable or its initial.
            ends |= (1u << (reading.back() - 'a')) | (1u << (reading.front() - 'a'));
            for (size_t i = 1; i < reading.size(); ++i)
                add_pair(static_cast<unsigned>(reading[i - 1] - 'a'), static_cast<unsigned>(reading[i] - 'a'));
        });
        for (unsigned first = 0; first < 26; ++first)
            if (previous_ends & (1u << first))
                for (unsigned second = 0; second < 26; ++second)
                    if (starts & (1u << second)) add_pair(first, second);
        previous_ends = ends;
        pos += width;
    }
    return pairs;
}
PinyinMatch FindPinyinMatch(std::wstring_view name, std::wstring_view query) {
    if (query.empty() || !PinyinEligible(query) || !HasPinyinCharacters(name)) return {};
    auto full = Match(name, query, false);
    return full ? full : Match(name, query, true);
}
}
