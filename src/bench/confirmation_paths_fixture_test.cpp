#include "../app/app_internal.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/dialog_text_fit.h"
#include "../common/localization.h"
#include <cstdio>
#include "../common/display_path.h"

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
}
int main() {
    using namespace pulse;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const std::wstring long_local = L"\\\\?\\C:\\fixture\\very long parent\\" + std::wstring(180, L'x') + L".txt";
    const std::wstring unc = L"\\\\?\\UNC\\server\\share\\many\\folders\\" + std::wstring(150, L'文');
    ops::DeleteConfirmation pending;
    pending.token = 1;
    pending.plan.targets = {{long_local, {long_local}, ops::DeleteDisposition::Permanent, L"irreversible fixture"},
        {unc, {unc}, ops::DeleteDisposition::Permanent, L"irreversible fixture"}};
    const auto spec = BuildDeleteConfirmationSpec(pending);
    Check(spec.rows.size() == 2 && spec.rows[0].text == path::FriendlyPathText(long_local) &&
        spec.rows[1].text == path::FriendlyPathText(unc),
        "delete confirmation retains complete friendly local and UNC paths until layout");
    const auto measure = [](const std::wstring& text) { return static_cast<float>(text.size()); };
    for (const auto& row : spec.rows) {
        const auto fitted = ui::FitPathMiddle(row.text, 48, measure);
        Check(measure(fitted) <= 48 && fitted.find(L'\u2026') != std::wstring::npos,
            "confirmation rows shorten by available width rather than fixed character slicing");
    }
    for (int index = 0; index < 12; ++index)
        pending.plan.targets.push_back({long_local, {long_local}, ops::DeleteDisposition::Permanent, L"irreversible fixture"});
    const auto many = BuildDeleteConfirmationSpec(pending);
    Check(many.rows.size() == 5 && many.note.find(L"more") != std::wstring::npos &&
        many.cancel_is_default && many.danger && many.fit_to_work_area,
        "large deletion confirmation keeps total counts and safe default with a bounded path summary");
    // Actual modal screenshots are covered by pulse_link_confirmation_ui_test,
    // whose compiled snapshot hook is isolated from the shipping executable.
    OleUninitialize();
    return failures ? 1 : 0;
}
