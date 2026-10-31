// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstdint>
#include <array>
#include <string_view>
namespace pulse::index {
enum class PinyinMatchKind : uint8_t { None, Full, Initials };
struct PinyinMatch {
    PinyinMatchKind kind = PinyinMatchKind::None;
    uint32_t start = 0;
    uint32_t length = 0; // UTF-16 code units, covering original characters
    explicit operator bool() const { return kind != PinyinMatchKind::None; }
};
inline constexpr uint32_t kPinyinDataVersion = 1;
bool PinyinEligible(std::wstring_view query);
bool HasPinyinCharacters(std::wstring_view name);
uint32_t PinyinCandidateLetters(std::wstring_view name);
using PinyinPairMask = std::array<uint64_t, 11>;
PinyinPairMask PinyinCandidatePairs(std::wstring_view name);
PinyinMatch FindPinyinMatch(std::wstring_view name, std::wstring_view folded_query);
}
