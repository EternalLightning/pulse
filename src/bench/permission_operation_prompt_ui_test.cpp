#include "../app/app_state.h"
#include "../app/permission_operation_prompt.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace pulse::ops {
// No worker is started: accepted consent can only enqueue a fixture request.
// These tests must never invoke the elevation transport or touch these paths.
struct TransferIntegrationProbe {
    static void Seed(OpsManager& manager, uint64_t id, const OpStatus& status) {
        manager.running_ = true;
        manager.accepting_ = true;
        manager.queue_.clear();
        OpsManager::QueueItem retry;
        retry.seq = id;
        retry.req.type = status.type;
        const bool creation = status.type == OpType::CreateFolder || status.type == OpType::CreateTextFile;
        retry.req.sources = {creation ? status.elevation_destination_path : status.elevation_source_path};
        retry.req.dest_dir = fs::ParentPath(status.elevation_destination_path);
        if (status.type == OpType::Copy || status.type == OpType::Move) {
            TransferPlanEntry entry;
            entry.source = status.elevation_source_path;
            entry.destination = status.elevation_destination_path;
            retry.retry_transfer_plan.push_back(std::move(entry));
        }
        retry.retry_failure_hr = E_ACCESSDENIED;
        retry.retry_failed_path = status.failed_path;
        retry.retry_generation = manager.lock_retry_generation_.load();
        if (status.elevation_retry_available) manager.elevation_retry_ = std::move(retry);
        else manager.elevation_retry_.reset();
        manager.next_seq_ = id + 1;
        manager.SetStatus([&](OpStatus& current) { current = status; });
    }
    static void Supersede(OpsManager& manager, uint64_t id) {
        manager.elevation_retry_.reset();
        manager.SetStatus([&](OpStatus& current) {
            current.task_id = id;
            current.elevation_retry_available = false;
        });
    }
    static size_t Queued(const OpsManager& manager) { return manager.queue_.size(); }
    static bool QueuedOne(const OpsManager& manager, uint64_t id) {
        return manager.queue_.size() == 1 && manager.queue_.front().seq != id &&
            manager.queue_.front().req.type == OpType::Copy;
    }
};
}

namespace {
int failures = 0;
UINT_PTR timer = 0;
int opened = 0;
int dpi = 96;
bool narrow = false;
bool captured = false;
size_t expected_rows = 4;
std::wstring screenshot;
pulse::AppState* state = nullptr;
enum class Action { Enter, Space, Escape, Close, CancelButton, Accept, Stale, Exit, OwnerClose };
Action action = Action::Escape;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
BOOL CALLBACK FindDialog(HWND hwnd, LPARAM value) {
    wchar_t name[80]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring_view(name) != L"PulseConfirmWindow") return TRUE;
    *reinterpret_cast<HWND*>(value) = hwnd;
    return FALSE;
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer);
    ++opened;
    if (state) {
        const auto before = opened;
        pulse::PumpPermissionOperationPrompt(*state);
        Check(opened == before && state->permissionOperationPromptActive,
            "modal reentry preserves a single active permission prompt");
    }
    RECT rect{};
    GetWindowRect(dialog, &rect);
    SendMessageW(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
    if (narrow) SetWindowPos(dialog, nullptr, 0, 0, 280 * dpi / 96, 300 * dpi / 96,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(dialog);
    RECT client{};
    GetClientRect(dialog, &client);
    pulse::ui::ConfirmDialogInspection info;
    Check(SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0,
        reinterpret_cast<LPARAM>(&info)) != 0 && info.row_count == expected_rows &&
        info.content.bottom <= std::min(info.primary.top, info.cancel.top) &&
        info.primary.left >= 0 && info.cancel.left >= 0 &&
        info.primary.right <= client.right && info.cancel.right <= client.right &&
        info.primary.bottom <= client.bottom && info.cancel.bottom <= client.bottom &&
        (info.primary.left >= info.cancel.right || info.cancel.left >= info.primary.right ||
         info.primary.top >= info.cancel.bottom || info.cancel.top >= info.primary.bottom),
        "source/destination labels and path rows remain above bounded confirmation controls");
    if (narrow) {
        SendMessageW(dialog, WM_KEYDOWN, VK_END, 0);
        pulse::ui::ConfirmDialogInspection end;
        SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&end));
        const bool overflow_required = expected_rows == 4;
        Check(end.max_scroll >= 0 && end.scroll == end.max_scroll &&
            (!overflow_required || end.max_scroll > 0),
            overflow_required
                ? "copy permission details overflow and End reaches the narrow layout scroll limit"
                : "delete permission details fit completely or End reaches the scroll limit");
    }
    if (!screenshot.empty()) {
        const auto theme = pulse::ui::IsHighContrast() ? pulse::ui::MakeHighContrastTheme()
            : pulse::ui::MakeTheme(state->darkMode, state->accentColor);
        Check(state->accentColor.a == 1.0f && theme.accent.a == 1.0f && theme.accent_text.a == 1.0f,
            "captured dialog uses the production opaque accent and primary text colors");
        captured = SendMessageW(dialog, pulse::ui::kConfirmSnapshotMessage,
            0, reinterpret_cast<LPARAM>(screenshot.c_str())) != 0;
    }
    if (action == Action::Close) SendMessageW(dialog, WM_CLOSE, 0, 0);
    else if (action == Action::Escape) SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
    else if (action == Action::Enter || action == Action::Space)
        SendMessageW(dialog, WM_KEYDOWN, action == Action::Enter ? VK_RETURN : VK_SPACE, 0);
    else if (action == Action::CancelButton) {
        const LPARAM point = MAKELPARAM(static_cast<int>((info.cancel.left + info.cancel.right) / 2),
            static_cast<int>((info.cancel.top + info.cancel.bottom) / 2));
        SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(dialog, WM_LBUTTONUP, 0, point);
    } else if (action == Action::OwnerClose) {
        DestroyWindow(state->hwnd);
        state->hwnd = nullptr;
    } else {
        if (action == Action::Stale)
            pulse::ops::TransferIntegrationProbe::Supersede(state->ops, state->ops.Status().task_id + 99);
        if (action == Action::Exit) state->exit_requested = true;
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    }
}
pulse::ops::OpStatus Failure(uint64_t id) {
    pulse::ops::OpStatus status;
    status.phase = pulse::ops::OpPhase::Failed;
    status.type = pulse::ops::OpType::Copy;
    status.task_id = id;
    status.elevation_retry_available = true;
    status.failure_hr = E_ACCESSDENIED;
    status.elevation_source_path = LR"(\\?\C:\pulse-permission-fixture\source\)";
    for (int segment = 0; segment < 5; ++segment)
        status.elevation_source_path += std::wstring(240, L'x') + L"\\";
    status.elevation_source_path += L"tmp.csv";
    status.elevation_destination_path = LR"(C:\pulse-permission-fixture\protected\tmp.csv)";
    status.failed_path = status.elevation_destination_path;
    status.source_label = L"tmp.csv";
    status.destination_label = LR"(C:\pulse-permission-fixture\protected)";
    status.completed_items = 2;
    status.total_items = 3;
    status.completed_ops = 4;
    status.transferred_bytes = 123;
    return status;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(argc > 1 ? std::filesystem::path(argv[1])
        : std::filesystem::path(L"bench_data/permission_prompt_ui"));
    std::filesystem::create_directories(output);
    const auto failure = Failure(100);
    const auto spec = BuildPermissionOperationConfirmationSpec(failure);
    Check(spec.cancel_is_default && spec.fit_to_work_area && !spec.danger && spec.rows.size() == 4 &&
        !spec.rows[0].is_path && !spec.rows[0].boxed && spec.rows[1].is_path && spec.rows[1].boxed &&
        !spec.rows[2].is_path && !spec.rows[2].boxed && spec.rows[3].is_path && spec.rows[3].boxed &&
        spec.rows[1].text == path::FriendlyPathText(failure.elevation_source_path) &&
        spec.rows[1].text.size() > 1200 && spec.rows[3].text == failure.elevation_destination_path &&
        spec.note == l10n::Get(l10n::StringId::PermissionOperationUacHint) &&
        spec.confirm_text == l10n::Get(l10n::StringId::PermissionOperationContinue),
        "permission confirmation uses two unboxed labels and two exact boxed paths with safe default");
    auto unrelated_failure = failure;
    unrelated_failure.failed_path = LR"(C:\unrelated\failure-endpoint.txt)";
    const auto unrelated_spec = BuildPermissionOperationConfirmationSpec(unrelated_failure);
    Check(unrelated_spec.rows.size() == 4 && unrelated_spec.rows[1].text == spec.rows[1].text &&
        unrelated_spec.rows[3].text == spec.rows[3].text &&
        std::none_of(unrelated_spec.rows.begin(), unrelated_spec.rows.end(), [&](const auto& row) {
            return row.text == unrelated_failure.failed_path;
        }), "permission presentation depends on cached operation endpoints and never shows the failed-path row");
    auto creation = failure;
    creation.type = ops::OpType::CreateFolder;
    creation.elevation_source_path.clear();
    const auto creation_spec = BuildPermissionOperationConfirmationSpec(creation);
    Check(creation_spec.rows.size() == 2 && !creation_spec.rows[0].boxed && creation_spec.rows[1].boxed &&
        creation_spec.rows[1].text == creation.elevation_destination_path,
        "new-item permission confirmation shows only its boxed target path");
    struct UiScenario {
        const wchar_t* language;
        bool dark;
        int dpi;
        bool narrow;
        const wchar_t* screenshot_prefix;
    };
    const UiScenario scenarios[] = {
        {L"zh-CN", false, 96, false, L"light_96_"},
        {L"zh-CN", true, 144, true, L"dark_144_narrow_"},
        {L"en-US", false, 96, false, L"english_light_96_"},
        {L"en-US", true, 144, true, L"english_dark_144_narrow_"},
    };
    for (const auto& scenario : scenarios) {
        l10n::SetLanguage(scenario.language);
        if (std::wstring_view(scenario.language) == L"en-US") {
            const auto english = BuildPermissionOperationConfirmationSpec(failure);
            const auto ui_text = english.title + english.message + english.confirm_text +
                english.cancel_text + english.note + english.rows[0].text + english.rows[2].text;
            Check(l10n::effective_language() == l10n::Language::EnUS &&
                english.title == L"Administrator permission required" &&
                english.message.starts_with(L"This operation failed because permission was denied.") &&
                english.note == L"Windows will request administrator permission after you continue." &&
                english.confirm_text == L"Continue as administrator" &&
                english.cancel_text == L"Cancel" && english.rows[0].text == L"Source path:" &&
                english.rows[2].text == L"Destination path:" &&
                std::none_of(ui_text.begin(), ui_text.end(),
                    [](wchar_t ch) { return ch >= 0x3400 && ch <= 0x9FFF; }),
                "English permission title, body, actions, note and path labels contain no Chinese UI text");
        }
        auto owned = std::make_unique<AppState>();
        state = owned.get();
        state->isolatedTest = true;
        state->appPrefs.persist = state->places.persist = state->searchHistory.persist = false;
        state->hwnd = CreateWindowExW(0, L"STATIC", L"Pulse permission fixture", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        state->darkMode = scenario.dark;
        state->accentColor = ui::GetAccentColor();
        dpi = scenario.dpi;
        narrow = scenario.narrow;
        auto unavailable = Failure(99);
        unavailable.elevation_retry_available = false;
        ops::TransferIntegrationProbe::Seed(state->ops, 99, unavailable);
        const auto unopened = opened;
        PumpPermissionOperationPrompt(*state);
        Check(opened == unopened && ops::TransferIntegrationProbe::Queued(state->ops) == 0,
            "plain failed status without elevation availability never requests administrator consent");
        ops::TransferIntegrationProbe::Seed(state->ops, 98, Failure(98));
        state->lockedOperationPromptActive = true;
        PumpPermissionOperationPrompt(*state);
        Check(opened == unopened && ops::TransferIntegrationProbe::Queued(state->ops) == 0,
            "permission prompt waits while the existing lock prompt owns the modal loop");
        state->lockedOperationPromptActive = false;
        int index = 0;
        for (auto next : {Action::Enter, Action::Space, Action::Escape, Action::Close,
                Action::CancelButton, Action::Accept, Action::Stale, Action::Exit}) {
            action = next;
            state->exit_requested = false;
            const uint64_t id = 100 + index++;
            auto status = Failure(id);
            ops::TransferIntegrationProbe::Seed(state->ops, id, status);
            const auto cached_status = state->ops.Status();
            const auto cached_spec = BuildPermissionOperationConfirmationSpec(cached_status);
            Check(cached_status.source_label == L"tmp.csv" && cached_spec.rows.size() == 4 &&
                cached_spec.rows[1].text == path::FriendlyPathText(status.elevation_source_path) &&
                cached_spec.rows[3].text == path::FriendlyPathText(status.elevation_destination_path),
                "copy with a basename status label presents both full cached operation paths");
            captured = false;
            screenshot = (output / (std::wstring(scenario.screenshot_prefix) +
                std::to_wstring(index) + L".png")).wstring();
            const auto before = opened;
            timer = SetTimer(nullptr, 0, 200, Drive);
            PumpPermissionOperationPrompt(*state);
            KillTimer(nullptr, timer);
            Check(opened == before + 1 && captured && !state->permissionOperationPromptActive,
                "each current failed attempt opens and closes one captured permission dialog");
            if (action == Action::Accept)
                Check(ops::TransferIntegrationProbe::QueuedOne(state->ops, id),
                    "explicit administrator consent queues exactly one retry without starting a worker");
            else Check(ops::TransferIntegrationProbe::Queued(state->ops) == 0,
                "default/cancel/Escape/close/stale/exit never queues an administrator retry");
            Check(!state->ops.IsElevationFailureCurrent(id), "answered or stale permission attempt is retired");
            Check(state->ops.Status().completed_items == 2 && state->ops.Status().transferred_bytes == 123 &&
                state->ops.Status().completed_ops == 4,
                "permission decisions retain already completed item and byte accounting");
            state->exit_requested = false;
            PumpPermissionOperationPrompt(*state);
            Check(opened == before + 1, "answered attempt never presents the permission prompt twice");
        }
        auto deletion = Failure(300);
        deletion.type = ops::OpType::RealDelete;
        deletion.elevation_destination_path.clear();
        deletion.failed_path = deletion.elevation_source_path;
        deletion.destination_label.clear();
        const auto delete_spec = BuildPermissionOperationConfirmationSpec(deletion);
        Check(delete_spec.rows.size() == 2 && !delete_spec.rows[0].is_path && !delete_spec.rows[0].boxed &&
            delete_spec.rows[1].is_path && delete_spec.rows[1].boxed &&
            delete_spec.rows[1].text == path::FriendlyPathText(deletion.elevation_source_path),
            "delete permission confirmation contains only one unboxed source label and one boxed source path");
        ops::TransferIntegrationProbe::Seed(state->ops, 300, deletion);
        expected_rows = 2;
        action = Action::Escape;
        captured = false;
        screenshot = (output / (std::wstring(scenario.screenshot_prefix) + L"delete_source_only.png")).wstring();
        const auto delete_before = opened;
        timer = SetTimer(nullptr, 0, 200, Drive);
        PumpPermissionOperationPrompt(*state);
        KillTimer(nullptr, timer);
        Check(opened == delete_before + 1 && captured && ops::TransferIntegrationProbe::Queued(state->ops) == 0,
            "source-only delete dialog is captured and cancellation never queues a retry");
        expected_rows = 4;
        // Destroying the owner while the native modal loop runs must also reject.
        action = Action::OwnerClose;
        ops::TransferIntegrationProbe::Seed(state->ops, 200, Failure(200));
        screenshot.clear();
        const auto before = opened;
        timer = SetTimer(nullptr, 0, 200, Drive);
        PumpPermissionOperationPrompt(*state);
        KillTimer(nullptr, timer);
        Check(opened == before + 1 && ops::TransferIntegrationProbe::Queued(state->ops) == 0 &&
            !state->permissionOperationPromptActive && !state->ops.IsElevationFailureCurrent(200),
            "owner window destruction rejects consent and retires only its current failed attempt");
        state->ops.Stop();
        state = nullptr;
        owned.reset();
    }
    OleUninitialize();
    return failures ? 1 : 0;
}
