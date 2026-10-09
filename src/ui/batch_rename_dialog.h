// batch_rename_dialog.h — Modal preview UI for batch rename.
#pragma once
#include "../model/batch_rename.h"
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>

namespace pulse::ui {

struct BatchRenameDialogResult {
    bool accepted = false;
    model::BatchRenameRule rule;
    std::vector<model::BatchRenameItem> items;
};

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner,
                                              const std::vector<std::wstring>& paths,
                                              bool dark,
                                              D2D1_COLOR_F accent);

} // namespace pulse::ui
