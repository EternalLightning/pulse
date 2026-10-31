// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>

namespace pulse::index {
// Only used by positive literal task queries. Background extraction reads all text.
class DocumentLiteralMatch {
public:
    DocumentLiteralMatch(std::wstring_view needle, bool case_sensitive)
        : needle_(needle), case_sensitive_(case_sensitive) {
        if (!needle.empty() && !case_sensitive) for (size_t i = 0; i < folded_.size(); ++i)
            folded_[i] = static_cast<wchar_t>(towlower(static_cast<wchar_t>(i)));
    }
    bool Found(std::wstring_view text) {
        if (needle_.empty()) return false;
        const size_t start = checked_ >= needle_.size() ? checked_ - needle_.size() + 1 : 0;
        checked_ = text.size();
        return std::search(text.begin() + (std::min)(start, text.size()), text.end(),
            needle_.begin(), needle_.end(), [&](wchar_t a, wchar_t b) {
                if (a == b) return true;
                if (case_sensitive_) return false;
                return (static_cast<unsigned>(a) < folded_.size() ? folded_[a] : towlower(a)) ==
                    (static_cast<unsigned>(b) < folded_.size() ? folded_[b] : towlower(b));
            }) != text.end();
    }
private:
    std::wstring_view needle_;
    bool case_sensitive_;
    size_t checked_ = 0;
    std::array<wchar_t, 128> folded_{};
};
}
