// advanced_search_dialog.h — Structured search form (no regex).
#pragma once
#include "../model/search_query.h"
#include <windows.h>
#include <d2d1.h>
#include <string>

namespace pulse::ui {

struct AdvancedSearchDialogResult {
    bool accepted = false;
    std::wstring query;
};

AdvancedSearchDialogResult ShowAdvancedSearchDialog(HWND owner,
                                                    model::AdvancedSearchSpec spec,
                                                    bool dark,
                                                    D2D1_COLOR_F accent);

} // namespace pulse::ui
