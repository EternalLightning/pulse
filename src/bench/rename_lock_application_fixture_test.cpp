#include "../app/app_internal.h"
#include "../app/locked_operation_prompt.h"
#include "../common/localization.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
int failures = 0;
UINT_PTR timer = 0;
HANDLE held = INVALID_HANDLE_VALUE;
int action = 0;
int opened = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
}
BOOL CALLBACK FindDialog(HWND hwnd, LPARAM value) {
    wchar_t name[80]{}; GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (std::wstring_view(name) != L"PulseConfirmWindow") return TRUE;
    *reinterpret_cast<HWND*>(value) = hwnd; return FALSE;
}
void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
    HWND dialog = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindDialog, reinterpret_cast<LPARAM>(&dialog));
    if (!dialog) return;
    KillTimer(nullptr, timer); ++opened;
    if (action == 1) {
        if (held != INVALID_HANDLE_VALUE) { CloseHandle(held); held = INVALID_HANDLE_VALUE; }
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
    } else SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
}
bool Wait(pulse::AppState& s, uint64_t before) {
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        const auto status = s.ops.Status();
        if (!status.active && status.completed_ops > before) return true;
        Sleep(5);
    }
    return false;
}
}
int main() {
    using namespace pulse;
    OleInitialize(nullptr); l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"../bench_data") /
        (L"rename-app-lock-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.isolatedTest = true; s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"Pulse rename fixture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    s.darkMode = true;
    s.ops.SetUiWindow(s.hwnd); s.ops.Start([] {});
    for (int choice : {0, 1}) {
        const auto source = root / (choice ? L"retry.txt" : L"cancel.txt");
        const auto target = root / (choice ? L"retry-new.txt" : L"cancel-new.txt");
        std::ofstream(source) << "isolated fixture";
        held = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "application fixture holds only its newly created rename source");
        ops::OpRequest request; request.type = ops::OpType::Rename;
        request.sources = {source.wstring()}; request.new_name = target.filename().wstring();
        auto before = s.ops.Status().completed_ops;
        s.ops.Submit(request);
        Check(Wait(s, before) && s.ops.Status().phase == ops::OpPhase::Failed && s.ops.Status().lock_retry_available,
            "actual OpsManager worker publishes a current rename lock prompt instead of bare error 5");
        const uint64_t failed = s.ops.Status().task_id;
        action = choice; timer = SetTimer(nullptr, 0, 200, Drive);
        PumpLockedOperationPrompt(s);
        KillTimer(nullptr, timer);
        if (choice) {
            Check(Wait(s, before + 1) && s.ops.Status().phase == ops::OpPhase::Completed &&
                !std::filesystem::exists(source) && std::filesystem::exists(target),
                "production application prompt retries on explicit Retry after the lock closes");
            Check(!s.ops.IsLockedFailureCurrent(failed), "successful UI retry retires the original failed task");
        } else {
            Check(std::filesystem::exists(source) && !std::filesystem::exists(target) &&
                s.ops.Status().task_id == failed && !s.ops.Status().active,
                "production application prompt Cancel does not rename or terminate anything");
            if (held != INVALID_HANDLE_VALUE) { CloseHandle(held); held = INVALID_HANDLE_VALUE; }
        }
    }
    Check(opened == 2, "actual PumpLockedOperationPrompt opens one Pulse dialog for each failed attempt");
    s.ops.Stop(); DestroyWindow(s.hwnd); s.hwnd = nullptr; owned.reset();
    const auto parent = std::filesystem::absolute(L"../bench_data");
    Check(root.parent_path() == parent && root.filename().wstring().starts_with(L"rename-app-lock-"),
        "application fixture cleanup stays within its isolated root");
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"rename-app-lock-")) std::filesystem::remove_all(root);
    OleUninitialize();
    return failures ? 1 : 0;
}
