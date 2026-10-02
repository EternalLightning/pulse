#include "../ops/ops_manager.h"
#include "../common/localization.h"
#include "../fs/fs_recycle.h"
#include "../common/path_utils.h"
#include "../shell_host/deletion_identity.h"
#include "../ops/delete_operation.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <commctrl.h>
#include <shlobj.h>
#include <winnetwk.h>

namespace pulse::ops {
struct DeleteIntegrationProbe {
    static uint32_t LastBackendResult(const OpsManager& manager) { return manager.done_hr_; }
};
}

namespace {
std::string Utf8(const std::wstring& text) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

int RunPermanentFromBin() {
    using namespace pulse;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"recycle-permanent-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"folder");
    std::ofstream(root / L"item.txt") << "recycled file fixture";
    std::ofstream(root / L"folder/child.txt") << "recycled directory fixture";
    const std::vector<std::wstring> originals = {(root / L"item.txt").wstring(), (root / L"folder").wstring()};
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse isolated recycle deletion test", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ops::OpsManager manager;
    manager.SetUiWindow(owner); manager.Start([] {});
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl;
        failures += !ok;
    };
    auto wait = [&](auto ready) {
        const auto deadline = GetTickCount64() + 15000;
        while (!ready() && GetTickCount64() < deadline) {
            Sleep(5);
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        return ready();
    };
    auto exists = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
    ops::OpRequest recycle; recycle.type = ops::OpType::RecycleDelete; recycle.sources = originals;
    auto before = manager.Status().completed_ops;
    manager.Submit(recycle);
    const bool recycled = wait([&] { return manager.Status().completed_ops > before; }) &&
        manager.Status().phase == ops::OpPhase::Completed && !exists(originals[0]) && !exists(originals[1]);
    check(recycled, "recycle only the isolated file and nonempty directory");
    manager.DrainCompletions();
    std::vector<fs::DirEntry> entries;
    std::vector<std::wstring> payloads, indexes;
    if (recycled && fs::EnumerateRecycleBinAtRoot(root.root_name().wstring() + L"\\$Recycle.Bin", entries)) {
        for (const auto& original : originals) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.full_path == fs::NormalizePath(original);
            });
            if (found != entries.end()) {
                payloads.push_back(found->recycle_path);
                indexes.push_back(fs::RecycleIndexPath(found->recycle_path));
            }
        }
    }
    check(payloads.size() == originals.size(), "resolve the exact current-user recycle identities for these fixtures");
    if (payloads.size() == originals.size()) {
        ops::OpRequest permanent; permanent.type = ops::OpType::RealDelete; permanent.sources = payloads;
        for (size_t i = 0; i < payloads.size(); ++i)
            permanent.delete_targets.push_back({originals[i], {payloads[i]}, true});
        before = manager.Status().completed_ops; manager.Submit(permanent);
        bool pending = wait([&] { return manager.PendingDeleteConfirmation().has_value(); });
        check(pending, "permanent recycle-item deletion still requires confirmation");
        if (const auto confirmation = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(confirmation->token, false);
        check(wait([&] { return manager.Status().completed_ops > before; }) &&
            exists(payloads[0]) && exists(payloads[1]) && exists(indexes[0]) && exists(indexes[1]) &&
            manager.DrainCompletions().empty(), "cancel leaves both recycle payloads and metadata untouched");
        // The display/original path is not the authorized recycle payload.
        std::ofstream(root / L"item.txt") << "live replacement must survive";
        before = manager.Status().completed_ops; manager.Submit(permanent);
        pending = wait([&] { return manager.PendingDeleteConfirmation().has_value(); });
        if (const auto confirmation = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(confirmation->token, true);
        const bool done = wait([&] { return manager.Status().completed_ops > before; });
        const bool deleted = pending && done && manager.Status().phase == ops::OpPhase::Completed &&
            !exists(payloads[0]) && !exists(payloads[1]) && !exists(indexes[0]) && !exists(indexes[1]);
        check(deleted, "confirmed deletion removes the exact recycle file, nonempty directory and both metadata records");
        const auto completions = manager.DrainCompletions();
        check(deleted && completions.size() == 1 && completions[0].type == ops::OpType::RealDelete &&
            completions[0].sources == originals,
            "successful completion reports the logical recycle entries for UI removal");
        std::ifstream replacement(root / L"item.txt");
        std::string text; std::getline(replacement, text); replacement.close();
        check(text == "live replacement must survive", "same-name live file at the original location is preserved");
        if (!deleted) {
            std::cout << "[INFO] backend HRESULT=0x" << std::hex << ops::DeleteIntegrationProbe::LastBackendResult(manager) << std::dec << std::endl;
            std::cout << "[INFO] " << Utf8(manager.Status().last_error) << std::endl;
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (const auto& path : {payloads[0], indexes[0]}) {
                IShellItem* item = nullptr;
                const auto parsing_path = pulse::path::StripExtendedPathPrefix(path);
                HRESULT hr = SHCreateItemFromParsingName(parsing_path.c_str(), nullptr, IID_PPV_ARGS(&item));
                PWSTR parsed = nullptr;
                if (SUCCEEDED(hr)) hr = item->GetDisplayName(SIGDN_FILESYSPATH, &parsed);
                std::cout << "[INFO] Shell path hr=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec
                    << " requested=" << Utf8(path) << " resolved=" << Utf8(parsed ? parsed : L"") << std::endl;
                if (parsed) CoTaskMemFree(parsed);
                if (item) item->Release();
            }
            if (SUCCEEDED(initialized)) CoUninitialize();
            std::filesystem::remove(root / L"item.txt");
            before = manager.Status().completed_ops;
            manager.Undo();
            check(wait([&] { return manager.Status().completed_ops > before; }) &&
                exists(originals[0]) && exists(originals[1]), "restore the isolated fixtures after a failed backend check");
        }
    }
    manager.Stop(); if (owner) DestroyWindow(owner);
    if (failures == 0) {
        std::filesystem::remove(root / L"item.txt");
        std::filesystem::remove(root);
    }
    return failures ? 1 : 0;
}

int RunDeletionIdentity() {
    using pulse::shell::IsSameDeletionItem;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"deletion-identity-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"sensitive");
    std::ofstream(root / L"item.txt") << "identity fixture";
    std::ofstream(root / L"other.txt") << "different identity";
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl;
        failures += !ok;
    };
    const auto file = (root / L"item.txt").wstring();
    check(IsSameDeletionItem(L"\\\\?\\" + file, file), "extended prefix preserves the exact authorized path");
    check(IsSameDeletionItem(file, (root / L"ITEM.TXT").wstring()), "casing change resolves to the same file identity");
    check(!IsSameDeletionItem(file, (root / L"other.txt").wstring()), "different path is rejected");
    check(!IsSameDeletionItem((root / L"missing.txt").wstring(), (root / L"MISSING.TXT").wstring()),
        "unverifiable casing change is rejected");
    const auto sensitive = (root / L"sensitive").wstring();
    const HANDLE directory = CreateFileW(sensitive.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILE_CASE_SENSITIVE_INFO info{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const bool enabled = directory != INVALID_HANDLE_VALUE &&
        SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &info, sizeof(info));
    const DWORD error = enabled ? ERROR_SUCCESS : GetLastError();
    if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
    if (enabled) {
        const auto lower = (root / L"sensitive/case.txt").wstring();
        const auto upper = (root / L"sensitive/CASE.TXT").wstring();
        std::ofstream(lower) << "lower identity";
        std::ofstream(upper) << "upper identity";
        check(!IsSameDeletionItem(lower, upper), "distinct case-sensitive files cannot authorize each other");
        check(IsSameDeletionItem(lower, lower), "exact case-sensitive file remains authorized");
    } else {
        std::cout << "[SKIP] case-sensitive directory unavailable, Win32 error=" << error << std::endl;
    }
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}

int RunNetworkDeletion() {
    using namespace pulse;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"network-delete-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    const auto local = (root / L"local.txt").wstring();
    const auto network_file = (root / (L"network-fixture-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".txt")).wstring();
    std::ofstream(local) << "recycle this local file";
    std::ofstream(network_file) << "delete only this isolated network fixture";
    const auto unc = L"\\\\localhost\\" + network_file.substr(0, 1) + L"$" + network_file.substr(2);
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl;
        failures += !ok;
    };
    ops::OpRequest request; request.type = ops::OpType::RecycleDelete;
    request.sources = {local, unc};
    std::wstring error;
    const auto plan = ops::BuildDeletePlan(request, 1, error);
    check(error.empty() && plan.targets.size() == 2 &&
        plan.targets[0].disposition == ops::DeleteDisposition::RecycleRequested &&
        plan.targets[1].disposition == ops::DeleteDisposition::Permanent && !plan.targets[1].reason.empty(),
        "UNC permanent confirmation retains local recycling intent in a mixed batch");
    if (!error.empty() || plan.targets.size() != 2 || plan.targets[1].reason.empty())
        std::cout << "[INFO] preparation error=" << Utf8(error) << " targets=" << plan.targets.size()
            << " network reason=" << Utf8(l10n::Get(l10n::StringId::DeleteReasonNetwork)) << std::endl;
    request.sources = {L"Z:\\Pulse-read-only-plan-check.txt"};
    const auto mapped_plan = ops::BuildDeletePlan(request, 2, error);
    check(error.empty() && mapped_plan.targets.size() == 1 &&
        mapped_plan.targets[0].disposition == ops::DeleteDisposition::Permanent,
        "current mapped Z drive is classified without reading or deleting user files");
    if (GetFileAttributesW(unc.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cout << "[SKIP] localhost share unavailable, Win32 error=" << GetLastError() << std::endl;
        std::filesystem::remove_all(root); return failures ? 1 : 0;
    }
    wchar_t drive[] = {0, L':', 0};
    const DWORD drives = GetLogicalDrives();
    for (wchar_t letter = L'W'; letter >= L'G'; --letter)
        if ((drives & (1u << (letter - L'A'))) == 0) { drive[0] = letter; break; }
    const auto remote = L"\\\\localhost\\" + root.root_name().wstring().substr(0, 1) + L"$";
    NETRESOURCEW resource{}; resource.dwType = RESOURCETYPE_DISK;
    resource.lpLocalName = drive; resource.lpRemoteName = const_cast<wchar_t*>(remote.c_str());
    const DWORD connected = drive[0] ? WNetAddConnection2W(&resource, nullptr, nullptr, CONNECT_TEMPORARY) : ERROR_NO_MORE_ITEMS;
    if (connected == NO_ERROR) {
        request.sources = {std::wstring(drive) + network_file.substr(2)};
        const auto mapped = ops::BuildDeletePlan(request, 3, error);
        check(error.empty() && mapped.targets[0].disposition == ops::DeleteDisposition::Permanent,
            "temporary mapped share uses Pulse permanent confirmation");
    } else std::cout << "[SKIP] temporary drive mapping unavailable, Win32 error=" << connected << std::endl;
    request.sources = {local, connected == NO_ERROR ? std::wstring(drive) + network_file.substr(2) : unc};
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse network deletion fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    ops::OpsManager manager; manager.SetUiWindow(owner); manager.Start([] {});
    auto wait = [&](auto ready) {
        const auto deadline = GetTickCount64() + 15000;
        while (!ready() && GetTickCount64() < deadline) { Sleep(5); MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        return ready();
    };
    std::atomic<bool> stop{false}, native_warning{false};
    std::thread observer([&] {
        while (!stop.load()) {
            struct Context { const std::wstring& leaf; std::atomic<bool>& warned; } context{network_file, native_warning};
            EnumWindows([](HWND window, LPARAM param) -> BOOL {
                if (!IsWindowVisible(window)) return TRUE;
                DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                if (pid == GetCurrentProcessId()) return TRUE;
                std::wstring text;
                EnumChildWindows(window, [](HWND child, LPARAM result) -> BOOL {
                    wchar_t label[2048]{}; GetWindowTextW(child, label, ARRAYSIZE(label));
                    *reinterpret_cast<std::wstring*>(result) += label; return TRUE;
                }, reinterpret_cast<LPARAM>(&text));
                auto& value = *reinterpret_cast<Context*>(param);
                if (text.find(std::filesystem::path(value.leaf).filename().wstring()) != std::wstring::npos &&
                    (text.find(L"永久") != std::wstring::npos || text.find(L"permanent") != std::wstring::npos)) {
                    value.warned.store(true);
                    PostMessageW(window, WM_CLOSE, 0, 0); // Fail closed if native UI regresses.
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&context));
            Sleep(10);
        }
    });
    auto before = manager.Status().completed_ops; manager.Submit(request);
    check(wait([&] { return manager.PendingDeleteConfirmation().has_value(); }) &&
        manager.Status().phase == ops::OpPhase::WaitingForDeleteConfirmation,
        "Pulse confirmation precedes backend progress or mutation");
    if (const auto confirmation = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(confirmation->token, false);
    check(wait([&] { return manager.Status().completed_ops > before; }) &&
        manager.Status().phase == ops::OpPhase::Cancelled && std::filesystem::exists(local) &&
        std::filesystem::exists(network_file) && manager.DrainCompletions().empty(),
        "rejecting mixed network confirmation preserves every selected file");
    before = manager.Status().completed_ops; manager.Submit(request);
    const bool pending = wait([&] { return manager.PendingDeleteConfirmation().has_value(); });
    if (const auto confirmation = manager.PendingDeleteConfirmation()) manager.ResolveDeleteConfirmation(confirmation->token, true);
    const bool done = pending && wait([&] { return manager.Status().completed_ops > before; });
    check(done && manager.Status().phase == ops::OpPhase::Completed &&
        !std::filesystem::exists(local) && !std::filesystem::exists(network_file),
        "acceptance permanently deletes network fixture and recycles local fixture");
    const auto results = manager.DrainCompletions();
    check(results.size() == 2 && results[0].type == ops::OpType::RealDelete &&
        results[0].sources == std::vector<std::wstring>{request.sources[1]} &&
        results[1].type == ops::OpType::RecycleDelete && results[1].sources == std::vector<std::wstring>{local},
        "mixed completion preserves separate permanent and recycle outcomes");
    if (!done || manager.Status().phase != ops::OpPhase::Completed)
        std::cout << "[INFO] " << Utf8(manager.Status().last_error) << std::endl;
    if (manager.CanUndo()) {
        before = manager.Status().completed_ops; manager.Undo();
        check(wait([&] { return manager.Status().completed_ops > before; }) &&
            std::filesystem::exists(local) && !std::filesystem::exists(network_file),
            "Undo restores only local recycle member, never claims network recovery");
    } else check(false, "local recycle member remains undoable");
    stop.store(true); observer.join();
    check(!native_warning.load(), "no Windows permanent-delete dialog appeared during the network operation");
    manager.Stop(); if (owner) DestroyWindow(owner);
    if (connected == NO_ERROR) check(WNetCancelConnection2W(drive, 0, FALSE) == NO_ERROR, "remove only the temporary test drive mapping");
    if (!failures) std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    if (argc == 2 && std::string_view(argv[1]) == "--permanent-from-bin") return RunPermanentFromBin();
    if (argc == 2 && std::string_view(argv[1]) == "--identity-only") return RunDeletionIdentity();
    if (argc == 2 && std::string_view(argv[1]) == "--network-delete") return RunNetworkDeletion();
    wchar_t module[32768]{};
    if (!GetModuleFileNameW(nullptr, module, ARRAYSIZE(module))) return 2;
    const auto root = std::filesystem::path(module).parent_path().parent_path() /
        L"bench_data/review-changes/recycle-feedback-fixture";
    const bool accept_unc = argc == 2 && std::string_view(argv[1]) == "--unc-warning-accept";
    const bool cancel_unc = argc == 2 && std::string_view(argv[1]) == "--unc-warning-cancel";
    const auto file = (root / (accept_unc ? L"unc-accepted.txt" : L"recycle-me.txt")).lexically_normal().wstring();
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cout << "[FAIL] dedicated recycle input missing; no operation submitted\n"; return 2;
    }
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Pulse recycle fixture", WS_OVERLAPPEDWINDOW,
        0, 0, 600, 300, nullptr, nullptr, nullptr, nullptr);
    ops::OpsManager manager;
    manager.SetUiWindow(owner); manager.Start([] {});
    int failed = 0;
    auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << std::endl;
        failed += !ok;
    };
    auto wait = [&](uint64_t previous) {
        const auto deadline = GetTickCount64() + 30000;
        while (manager.Status().completed_ops == previous && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return manager.Status().completed_ops == previous + 1;
    };
    if (accept_unc || cancel_unc) {
        // Existing localhost administrative share only, dedicated test root only.
        // No new shares, credentials or bin configuration are created.
        const auto unc_file = L"\\\\localhost\\" + file.substr(0, 1) + L"$" + file.substr(2, file.size() - 2);
        if (GetFileAttributesW(unc_file.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::cout << "[FAIL] localhost UNC fixture unavailable; no deletion requested\n";
            manager.Stop(); DestroyWindow(owner); return 2;
        }
        std::atomic<bool> stop{false}, warned{false};
        std::thread warning_driver([&] {
            while (!stop.load()) {
                struct Context { const std::wstring& file; std::atomic<bool>& warned; bool accept; } context{file, warned, accept_unc};
                EnumWindows([](HWND window, LPARAM param) -> BOOL {
                    auto& value = *reinterpret_cast<Context*>(param);
                    if (!IsWindowVisible(window)) return TRUE;
                    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
                    if (pid == GetCurrentProcessId()) return TRUE;
                    std::wstring text;
                    EnumChildWindows(window, [](HWND child, LPARAM result) -> BOOL {
                        wchar_t content[2048]{}; GetWindowTextW(child, content, ARRAYSIZE(content));
                        *reinterpret_cast<std::wstring*>(result) += content;
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&text));
                    const std::wstring leaf = std::filesystem::path(value.file).filename().wstring();
                    if (text.find(leaf) != std::wstring::npos &&
                        (text.find(L"永久") != std::wstring::npos || text.find(L"permanent") != std::wstring::npos)) {
                        value.warned.store(true);
                        if (value.accept) {
                            struct Button { HWND yes = nullptr; } button;
                            EnumChildWindows(window, [](HWND child, LPARAM param) -> BOOL {
                                wchar_t caption[128]{}; GetWindowTextW(child, caption, ARRAYSIZE(caption));
                                const std::wstring label(caption);
                                if (label.find(L"是") != std::wstring::npos || label.find(L"Yes") != std::wstring::npos)
                                    reinterpret_cast<Button*>(param)->yes = child;
                                return TRUE;
                            }, reinterpret_cast<LPARAM>(&button));
                            if (button.yes) PostMessageW(button.yes, BM_CLICK, 0, 0);
                        } else {
                            PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
                            PostMessageW(window, WM_CLOSE, 0, 0);
                        }
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&context));
                Sleep(30);
            }
        });
        ops::OpRequest unc_request; unc_request.type = ops::OpType::RecycleDelete; unc_request.sources = {unc_file};
        const auto before = manager.Status().completed_ops;
        manager.Submit(std::move(unc_request));
        const bool finished = wait(before);
        if (!finished) manager.CancelCurrent();
        stop.store(true); warning_driver.join();
        check(warned.load(), "UNC non-recyclable item shows a Windows permanent-delete warning");
        const auto results = manager.DrainCompletions();
        if (accept_unc) {
            check(finished && GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
                manager.Status().phase == ops::OpPhase::Completed && results.size() == 1 &&
                results[0].type == ops::OpType::RealDelete && results[0].sources == std::vector<std::wstring>{unc_file} &&
                !manager.CanUndo(), "explicitly accepting the Windows warning records permanent deletion, never false recycle Undo");
        } else check(finished && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES && results.empty(),
            "declining the Windows permanent warning preserves the original dedicated file");
        manager.Stop(); DestroyWindow(owner); return failed ? 1 : 0;
    }
    ops::OpRequest request;
    request.type = ops::OpType::RecycleDelete; request.sources = {file};
    auto previous = manager.Status().completed_ops;
    manager.Submit(request);
    const bool finished = wait(previous);
    const auto completed = manager.DrainCompletions();
    const bool recycled = finished && manager.Status().phase == ops::OpPhase::Completed && completed.size() == 1 &&
        completed[0].type == ops::OpType::RecycleDelete && completed[0].sources == std::vector<std::wstring>{file};
    check(recycled && !manager.PendingDeleteConfirmation() && manager.CanUndo(),
        "ordinary Delete really recycles and offers Undo without Pulse permanent confirmation");
    if (recycled) {
        previous = manager.Status().completed_ops;
        manager.Undo();
        check(wait(previous) && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
            manager.Status().phase == ops::OpPhase::Completed && !manager.CanUndo(),
            "Undo restores exactly the dedicated recycled file");
    } else {
        std::wcerr << L"[INFO] " << manager.Status().last_error << std::endl;
        manager.CancelCurrent();
    }
    if (recycled && failed == 0) {
        // Deliberately retain an older generation in the bin while recycling
        // a newer file at the same path; Undo must use its exact $R identity.
        previous = manager.Status().completed_ops;
        manager.Submit(request);
        const bool old_done = wait(previous);
        const auto old_undo = manager.UndoToJson();
        check(old_done && manager.Status().phase == ops::OpPhase::Completed, "first same-path generation is recycled");
        manager.DrainCompletions();
        const std::string newer = "new generation must be restored, never the older bin item";
        const HANDLE output = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        const bool created = output != INVALID_HANDLE_VALUE && WriteFile(output, newer.data(), static_cast<DWORD>(newer.size()), &written, nullptr) && written == newer.size();
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
        check(created, "create dedicated newer generation at the same original path");
        if (created) {
            previous = manager.Status().completed_ops; manager.Submit(request);
            const bool newer_done = wait(previous);
            check(newer_done && manager.Status().phase == ops::OpPhase::Completed, "newer same-path generation is recycled");
            manager.DrainCompletions();
            previous = manager.Status().completed_ops; manager.Undo();
            const bool restored = wait(previous);
            const HANDLE input = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            char content[128]{}; DWORD read = 0;
            const bool have_content = input != INVALID_HANDLE_VALUE && ReadFile(input, content, sizeof(content), &read, nullptr);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            check(restored && have_content && std::string(content, read) == newer,
                "Undo selects the exact latest recycled identity despite older same-path bin content");
            manager.DrainCompletions();
            // Keep the newer restored fixture, remove it from this path by a
            // non-destructive rename, then restore the older generation too.
            const auto saved = (root / (L"newer-restored-" + std::to_wstring(GetCurrentProcessId()) + L".txt")).wstring();
            check(MoveFileW(file.c_str(), saved.c_str()) != FALSE, "retain newer restored fixture without deletion");
            check(manager.UndoFromJson(old_undo), "reimport preserved first generation exact Undo");
            previous = manager.Status().completed_ops; manager.Undo();
            check(wait(previous) && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES,
                "restore older dedicated generation as well; no user bin item is selected");
        }
    }
    manager.Stop(); DestroyWindow(owner);
    return failed ? 1 : 0;
}
