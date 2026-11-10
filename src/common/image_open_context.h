// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../model/sort_types.h"
#include <cstddef>
#include <string>
#include <vector>

namespace pulse {

constexpr size_t kMaxImageOpenContextBytes = 8 * 1024 * 1024;

struct ImageOpenContext {
    std::vector<std::wstring> paths; // displayed file order; empty keeps the Shell fallback
    model::SortColumn column = model::SortColumn::Name;
    model::SortDirection direction = model::SortDirection::Asc;
};

} // namespace pulse
