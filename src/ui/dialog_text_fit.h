#pragma once
#include "../common/path_utils.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

template <typename Measure>
std::wstring FitTextEnd(const std::wstring& text, float width, Measure measure) {
    if (width <= 0.0f) return {};
    if (text.empty() || measure(text) <= width) return text;
    if (measure(std::wstring(L"\u2026")) > width) return {};
    size_t low = 0, high = text.size();
    while (low < high) {
        const size_t middle = (low + high + 1) / 2;
        if (measure(text.substr(0, middle) + L"\u2026") <= width) low = middle;
        else high = middle - 1;
    }
    if (low && IS_HIGH_SURROGATE(text[low - 1])) --low;
    return text.substr(0, low) + L"\u2026";
}

template <typename Measure>
std::wstring FitPathMiddle(const std::wstring& input, float width, Measure measure) {
    std::wstring text = pulse::path::StripExtendedPathPrefix(input);
    if (width <= 0.0f) return {};
    if (text.empty() || measure(text) <= width) return text;
    std::replace(text.begin(), text.end(), L'/', L'\\');
    size_t start = 0;
    std::wstring root;
    if (text.size() >= 3 && text[1] == L':' && text[2] == L'\\') {
        root = text.substr(0, 3);
        start = 3;
    } else if (text.starts_with(L"\\\\")) {
        const size_t server_end = text.find(L'\\', 2);
        const size_t share_end = server_end == std::wstring::npos
            ? std::wstring::npos : text.find(L'\\', server_end + 1);
        if (share_end != std::wstring::npos) {
            root = text.substr(0, share_end + 1);
            start = share_end + 1;
        }
    }
    if (text.back() == L'\\') return FitTextEnd(text, width, measure);
    std::vector<std::wstring> parts;
    while (start < text.size()) {
        const size_t next = text.find(L'\\', start);
        const auto part = text.substr(start, next == std::wstring::npos ? next : next - start);
        if (!part.empty()) parts.push_back(part);
        if (next == std::wstring::npos) break;
        start = next + 1;
    }
    if (parts.empty() || (parts.size() == 1 && root.empty())) return FitTextEnd(text, width, measure);
    std::wstring tail = parts.back();
    std::wstring best = root + L"\u2026\\" + tail;
    if (measure(best) > width) {
        const std::wstring head = root + L"\u2026\\";
        const float remaining = width - measure(head);
        const std::wstring leaf = FitTextEnd(tail, remaining, measure);
        return leaf.empty() ? FitTextEnd(tail, width, measure) : head + leaf;
    }
    for (size_t index = parts.size() - 1; index > 0;) {
        --index;
        const std::wstring candidate_tail = parts[index] + L"\\" + tail;
        const std::wstring candidate = root + L"\u2026\\" + candidate_tail;
        if (measure(candidate) > width) break;
        tail = candidate_tail;
        best = candidate;
    }
    return best;
}

} // namespace pulse::ui
