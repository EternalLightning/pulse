// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pulse::model {

enum class DetailsColumnKind : uint8_t { Name, Path, Date, Type, Size, Cloud, Dimensions, Duration, DateTaken };
constexpr size_t kDetailsColumnCount = 9;

struct DetailsColumnPrefs {
    bool configured = false;
    // Name stays first; Path is managed by the search layout.
    std::vector<DetailsColumnKind> order;
    std::array<float, kDetailsColumnCount> widths{};
    bool show_cloud_on_local = false;
    bool operator==(const DetailsColumnPrefs&) const = default;
};

inline constexpr std::wstring_view DetailsColumnToken(DetailsColumnKind kind) {
    constexpr std::wstring_view names[] = {L"name", L"path", L"date", L"type", L"size", L"cloud",
                                          L"dimensions", L"duration", L"date_taken"};
    const auto index = static_cast<size_t>(kind);
    return index < kDetailsColumnCount ? names[index] : std::wstring_view{};
}

inline DetailsColumnPrefs DefaultDetailsColumns() {
    return {true, {DetailsColumnKind::Name, DetailsColumnKind::Cloud, DetailsColumnKind::Date,
                   DetailsColumnKind::Type, DetailsColumnKind::Size}, {}};
}

inline bool ValidDetailsColumns(const DetailsColumnPrefs& prefs) {
    if (!prefs.configured) return prefs.order.empty();
    if (prefs.order.empty() || prefs.order.front() != DetailsColumnKind::Name ||
        prefs.order.size() >= kDetailsColumnCount) return false;
    std::array<bool, kDetailsColumnCount> seen{};
    for (const auto kind : prefs.order) {
        const auto index = static_cast<size_t>(kind);
        if (index >= kDetailsColumnCount || kind == DetailsColumnKind::Path || seen[index]) return false;
        seen[index] = true;
    }
    return std::all_of(prefs.widths.begin(), prefs.widths.end(), [](float v) {
        return std::isfinite(v) && (v == 0.0f || (v >= 24.0f && v <= 4096.0f));
    });
}

inline bool IsMediaDetailsColumn(DetailsColumnKind kind) {
    return kind == DetailsColumnKind::Dimensions || kind == DetailsColumnKind::Duration ||
           kind == DetailsColumnKind::DateTaken;
}

} // namespace pulse::model
