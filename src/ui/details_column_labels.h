// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../model/details_columns.h"
#include "../common/localization.h"
#include <string>

namespace pulse::ui {

inline std::wstring DetailsColumnLabel(model::DetailsColumnKind kind) {
    using K = model::DetailsColumnKind;
    using l10n::StringId;
    switch (kind) {
    case K::Name: return l10n::Get(StringId::ColumnName);
    case K::Path: return l10n::Get(StringId::ColumnPath);
    case K::Date: return l10n::Get(StringId::ColumnModified);
    case K::Type: return l10n::Get(StringId::ColumnType);
    case K::Size: return l10n::Get(StringId::ColumnSize);
    case K::Cloud: return l10n::Get(StringId::CloudStatusColumn);
    case K::Dimensions: return l10n::Get(StringId::PreviewPropertyDimensions);
    case K::Duration: return l10n::Get(StringId::PreviewPropertyDuration);
    case K::DateTaken: return l10n::Get(StringId::PreviewPropertyDateTaken);
    }
    return {};
}

inline bool HasMediaDetailsExtension(std::wstring_view path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos) return false;
    std::wstring extension(path.substr(dot));
    for (wchar_t& c : extension) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    constexpr std::wstring_view supported[] = {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff",
        L".webp", L".heic", L".mp4", L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v",
        L".mp3", L".wav", L".flac", L".m4a", L".aac"};
    return std::find(std::begin(supported), std::end(supported), extension) != std::end(supported);
}

} // namespace pulse::ui
