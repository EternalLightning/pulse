#include "app_internal.h"
#include "../ui/file_operation_dialog.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse {
namespace {
std::wstring SummarizeDeletePath(std::wstring path) {
    path = pulse::path::FriendlyPathText(path);
    std::replace(path.begin(), path.end(), L'\n', L' ');
    std::replace(path.begin(), path.end(), L'\r', L' ');
    std::replace(path.begin(), path.end(), L'\t', L' ');
    if (path.size() > 88) path = path.substr(0, 40) + L"…" + path.substr(path.size() - 44);
    return path;
}
}

ui::ConfirmDialogSpec BuildDeleteConfirmationSpec(const ops::DeleteConfirmation& confirmation) {
    const auto* pending = &confirmation;
    using I = l10n::StringId;
    ui::ConfirmDialogSpec spec;
    spec.title = l10n::Get(I::DeleteConfirmTitle);
    spec.confirm_text = l10n::Get(I::PermanentDelete);
    spec.cancel_text = l10n::Get(I::Cancel);
    spec.danger = true;
    spec.cancel_is_default = true;
    spec.fit_to_work_area = true;
    spec.message = l10n::Get(I::DeleteIrreversible);
    uint64_t permanent = 0, recyclable = 0;
    for (const auto& target : pending->plan.targets) {
        if (target.disposition == ops::DeleteDisposition::Permanent) ++permanent;
        else if (target.disposition == ops::DeleteDisposition::Recyclable) ++recyclable;
    }
    wchar_t count[256]{};
    swprintf_s(count, l10n::Get(I::DeleteCountFormat).c_str(),
        static_cast<unsigned long long>(pending->plan.targets.size()),
        static_cast<unsigned long long>(permanent), static_cast<unsigned long long>(recyclable));
    spec.message += L"\n\n" + std::wstring(count);
    if (permanent && recyclable) spec.message += L"\n" + l10n::Get(I::DeleteMixedWarning);
    std::vector<std::wstring> reasons;
    for (const auto& target : pending->plan.targets) {
        if (!target.reason.empty() && std::find(reasons.begin(), reasons.end(), target.reason) == reasons.end()) reasons.push_back(target.reason);
    }
    for (const auto& reason : reasons) spec.message += L"\n" + reason;
    spec.message += L"\n\n" + l10n::Get(I::DeletePathHeading);
    const size_t shown = std::min<size_t>(5, pending->plan.targets.size());
    for (size_t i = 0; i < shown; ++i) spec.message += L"\n" + SummarizeDeletePath(pending->plan.targets[i].path);
    if (shown < pending->plan.targets.size()) {
        wchar_t more[128]{};
        swprintf_s(more, l10n::Get(I::DeleteMoreFormat).c_str(),
            static_cast<unsigned long long>(pending->plan.targets.size() - shown));
        spec.message += L"\n" + std::wstring(more);
    }
    return spec;
}

void PresentDeleteConfirmation(AppState& s) {
    const auto pending = s.ops.PendingDeleteConfirmation();
    if (!pending || pending->token == s.deleteUiToken) return;
    s.deleteUiToken = pending->token; // Set before the modal loop can reenter WM_OPS_NOTIFY.
    auto spec = BuildDeleteConfirmationSpec(*pending);
    const uint64_t token = pending->token;
    spec.still_valid = [&s, token] {
        const auto current = s.ops.PendingDeleteConfirmation();
        return current && current->token == token && IsWindow(s.hwnd);
    };
    const bool accepted = ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor);
    s.ops.ResolveDeleteConfirmation(token, accepted);
}
}
