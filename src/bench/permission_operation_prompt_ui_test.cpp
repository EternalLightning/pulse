// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

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
        retry.req.sources = {status.elevation_source_path};
        retry.req.dest_dir = status.elevation_destination_path;
        retry.req.new_name = L"Fixture volume";
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
        return manager.queue_.size() == 1 && manager.queue_.front().seq == id &&
            manager.queue_.front().req.type == OpType::Rename &&
            manager.queue_.front().req.sources == std::vector<std::wstring>{L"Z:\\"} &&
            manager.queue_.front().req.new_name == L"Fixture volume";
    }
    static bool QueuedCopy(const OpsManager& manager, uint64_t id, const OpStatus& status) {
        return manager.queue_.size() == 1 && manager.queue_.front().seq == id &&
            manager.queue_.front().elevated_retry && manager.queue_.front().req.type == OpType::Copy &&
            manager.queue_.front().req.sources == std::vector<std::wstring>{status.elevation_source_path} &&
            manager.queue_.front().req.dest_dir == status.elevation_destination_path;
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
size_t expected_rows = 2;
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
        "drive label and path row remain above bounded confirmation controls");
    if (narrow) {
        SendMessageW(dialog, WM_KEYDOWN, VK_END, 0);
        pulse::ui::ConfirmDialogInspection end;
        SendMessageW(dialog, pulse::ui::kConfirmInspectMessage, 0, reinterpret_cast<LPARAM>(&end));
        Check(end.max_scroll >= 0 && end.scroll == end.max_scroll,
            "drive-label permission details fit completely or End reaches the scroll limit");
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
pulse::ops::OpStatus DriveLabelFailure(uint64_t id) {
    pulse::ops::OpStatus status;
    status.phase = pulse::ops::OpPhase::Failed;
    status.type = pulse::ops::OpType::Rename;
    status.task_id = id;
    status.elevation_retry_available = true;
    status.failure_hr = E_ACCESSDENIED;
    status.elevation_source_path = L"Z:\\";
    status.failed_path = status.elevation_source_path;
    status.source_label = status.elevation_source_path;
    status.completed_items = 2;
    status.total_items = 3;
    status.completed_ops = 4;
    status.transferred_bytes = 123;
    return status;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    const bool file_only = argc > 1 && std::wstring_view(argv[1]) == L"--file-operation-only";
    HDESK private_desktop = nullptr;
    if (file_only) {
        const auto name = L"PulsePermissionFixture-" + std::to_wstring(GetCurrentProcessId());
        private_desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!private_desktop || !SetThreadDesktop(private_desktop)) {
            std::printf("[BLOCKED] private UI test desktop unavailable; no dialog was opened\n");
            return 2;
        }
    }
    compat::EnableDpiAwareness();
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(file_only ? (argc > 2 ? std::filesystem::path(argv[2])
        : std::filesystem::path(L"bench_data/permission_prompt_file_ui")) : argc > 1 ? std::filesystem::path(argv[1])
        : std::filesystem::path(L"bench_data/permission_prompt_ui"));
    std::filesystem::create_directories(output);
    if (file_only) {
        expected_rows = 4;
        for (bool dark : {false, true}) {
            auto owned = std::make_unique<AppState>();
            state = owned.get(); state->isolatedTest = true;
            state->appPrefs.persist = state->places.persist = state->searchHistory.persist = false;
            state->hwnd = CreateWindowExW(0, L"STATIC", L"Pulse file permission fixture", 0,
                0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
            state->darkMode = dark; state->accentColor = ui::GetAccentColor();
            dpi = dark ? 144 : 96; narrow = dark;
            for (auto next : {Action::Escape, Action::Accept, Action::Stale}) {
                action = next;
                auto failure = DriveLabelFailure(300 + static_cast<unsigned>(next));
                failure.type = ops::OpType::Copy;
                failure.elevation_source_path = L"E:\\permission-fixture\\source\\same-name.txt";
                failure.elevation_destination_path = L"E:\\permission-fixture\\protected destination";
                ops::TransferIntegrationProbe::Seed(state->ops, failure.task_id, failure);
                const auto confirmation = BuildPermissionOperationConfirmationSpec(state->ops.Status());
                Check(confirmation.rows.size() == 4 && confirmation.cancel_is_default &&
                    confirmation.rows[1].text == failure.elevation_source_path &&
                    confirmation.rows[3].text == failure.elevation_destination_path,
                    "file permission confirmation shows the exact source and destination with safe default");
                screenshot = (output / (std::wstring(dark ? L"dark_144_" : L"light_96_") +
                    std::to_wstring(static_cast<unsigned>(next)) + L".png")).wstring();
                const auto before = opened; captured = false;
                timer = SetTimer(nullptr, 0, 200, Drive);
                PumpPermissionOperationPrompt(*state);
                KillTimer(nullptr, timer);
                Check(opened == before + 1 && captured && !state->permissionOperationPromptActive,
                    "ordinary file permission failure opens the Pulse confirmation window");
                Check(next == Action::Accept ? ops::TransferIntegrationProbe::QueuedCopy(state->ops, failure.task_id, failure)
                    : ops::TransferIntegrationProbe::Queued(state->ops) == 0,
                    "only explicit current consent queues the exact file retry; no worker or UAC runs in this UI test");
            }
            DestroyWindow(state->hwnd); state->hwnd = nullptr;
            state->ops.Stop(); state = nullptr;
        }
        OleUninitialize();
        return failures ? 1 : 0;
    }
    const auto failure = DriveLabelFailure(100);
    const auto spec = BuildPermissionOperationConfirmationSpec(failure);
    Check(spec.cancel_is_default && spec.fit_to_work_area && !spec.danger && spec.rows.size() == 2 &&
        !spec.rows[0].is_path && !spec.rows[0].boxed && spec.rows[1].is_path && spec.rows[1].boxed &&
        spec.rows[1].text == path::FriendlyPathText(failure.elevation_source_path) &&
        spec.note == l10n::Get(l10n::StringId::PermissionOperationUacHint) &&
        spec.confirm_text == l10n::Get(l10n::StringId::PermissionOperationContinue),
        "drive-label permission confirmation uses an unboxed label and exact boxed path with safe default");
    auto unrelated_failure = failure;
    unrelated_failure.failed_path = LR"(C:\unrelated\failure-endpoint.txt)";
    const auto unrelated_spec = BuildPermissionOperationConfirmationSpec(unrelated_failure);
    Check(unrelated_spec.rows.size() == 2 && unrelated_spec.rows[1].text == spec.rows[1].text &&
        std::none_of(unrelated_spec.rows.begin(), unrelated_spec.rows.end(), [&](const auto& row) {
            return row.text == unrelated_failure.failed_path;
        }), "permission presentation depends on cached operation endpoints and never shows the failed-path row");
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
                english.cancel_text + english.note + english.rows[0].text;
            Check(l10n::effective_language() == l10n::Language::EnUS &&
                english.title == L"Administrator permission required" &&
                english.message == L"Permission denied. Retry unfinished items as administrator." &&
                english.note == L"Windows will request administrator permission after you continue." &&
                english.confirm_text == L"Continue as administrator" &&
                english.cancel_text == L"Cancel" && english.rows[0].text == L"Source path:" &&
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
        auto unavailable = DriveLabelFailure(99);
        unavailable.elevation_retry_available = false;
        ops::TransferIntegrationProbe::Seed(state->ops, 99, unavailable);
        const auto unopened = opened;
        PumpPermissionOperationPrompt(*state);
        Check(opened == unopened && ops::TransferIntegrationProbe::Queued(state->ops) == 0,
            "plain failed status without elevation availability never requests administrator consent");
        ops::TransferIntegrationProbe::Seed(state->ops, 98, DriveLabelFailure(98));
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
            auto status = DriveLabelFailure(id);
            ops::TransferIntegrationProbe::Seed(state->ops, id, status);
            const auto cached_status = state->ops.Status();
            const auto cached_spec = BuildPermissionOperationConfirmationSpec(cached_status);
            Check(cached_status.source_label == status.elevation_source_path && cached_spec.rows.size() == 2 &&
                cached_spec.rows[1].text == path::FriendlyPathText(status.elevation_source_path),
                "drive-label permission confirmation presents its exact cached drive path");
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
        // Destroying the owner while the native modal loop runs must also reject.
        action = Action::OwnerClose;
        ops::TransferIntegrationProbe::Seed(state->ops, 200, DriveLabelFailure(200));
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
