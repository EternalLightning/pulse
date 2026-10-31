// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "index_config.h"
#include "../common/json_utils.h"
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <windows.h>

namespace pulse::index {
inline bool AdvanceMachineConfigGeneration(IndexConfig& config) {
    if (config.generation == (std::numeric_limits<uint64_t>::max)()) return false;
    ++config.generation;
    return true;
}
// Missing members keep legacy defaults. Present members must have their declared
// type; reject the whole document before publishing any values.
inline bool ParseMachineConfig(std::wstring_view input, const std::wstring& default_path, IndexConfig& output) {
    const json::Document document(input);
    if (!document.Valid() || document.Root().kind != json::Kind::Object) return false;
    IndexConfig config;
    config.index_path = default_path;
    std::unordered_set<std::wstring> names;
    for (const auto& value : document.Root().children) {
        if (!names.insert(value.name).second) return false;
        if (value.name == L"version" || value.name == L"generation") {
            uint64_t number = 0;
            if (value.kind != json::Kind::Number || !json::ParseUnsigned(value.text, number) || !number) return false;
            if (value.name == L"version") {
                if (number != 1) return false;
                config.version = static_cast<uint32_t>(number);
            } else config.generation = number;
        } else if (value.name == L"include_fixed_ntfs" || value.name == L"include_removable_ntfs") {
            if (value.kind != json::Kind::Boolean) return false;
            (value.name == L"include_fixed_ntfs" ? config.include_fixed_ntfs : config.include_removable_ntfs) = value.text == L"true";
        } else if (value.name == L"index_path") {
            if (value.kind != json::Kind::String || value.text.find(L'\0') != std::wstring::npos) return false;
            if (!value.text.empty()) config.index_path = value.text;
        } else if (value.name == L"excluded_volume_ids" || value.name == L"excluded_paths") {
            if (value.kind != json::Kind::Array) return false;
            for (const auto& item : value.children) {
                if (item.kind != json::Kind::String || item.text.find(L'\0') != std::wstring::npos) return false;
                auto path = item.text;
                if (value.name == L"excluded_volume_ids") config.excluded_volume_ids.insert(NormalizeVolumeId(std::move(path)));
                else {
                    std::replace(path.begin(), path.end(), L'/', L'\\');
                    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
                    if (!path.empty()) config.excluded_paths.push_back(std::move(path));
                }
            }
        }
    }
    auto less = [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    };
    std::sort(config.excluded_paths.begin(), config.excluded_paths.end(), less);
    config.excluded_paths.erase(std::unique(config.excluded_paths.begin(), config.excluded_paths.end(),
        [](const auto& a, const auto& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }), config.excluded_paths.end());
    output = std::move(config);
    return true;
}
}
