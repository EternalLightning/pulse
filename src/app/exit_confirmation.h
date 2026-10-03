#pragma once
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

namespace pulse::app {
inline ui::ConfirmDialogSpec ExitConfirmationSpec() {
    const bool english = l10n::effective_language() == l10n::Language::EnUS;
    ui::ConfirmDialogSpec spec;
    spec.title = english ? L"File operations are still in progress" : L"文件操作尚未完成";
    spec.message = english ? L"Exiting stops current and queued tasks. Completed work will be kept."
                           : L"退出将停止当前及排队任务。已完成的部分会保留。";
    spec.confirm_text = english ? L"Stop tasks and exit" : L"停止任务并退出";
    spec.cancel_text = english ? L"Keep running" : L"继续执行";
    spec.cancel_is_default = true;
    spec.danger = true;
    spec.fit_to_work_area = true;
    return spec;
}
}
