#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include "../ops/shell_command.h"

namespace fixture {
bool mock_process = false, mock_shell = false;
DWORD process_error = ERROR_ELEVATION_REQUIRED, shell_error = ERROR_CANCELLED;
bool shell_success = true;
int shell_calls = 0, process_calls = 0;
std::wstring shell_file, shell_args, shell_directory, shell_verb, process_file;
BOOL WINAPI ProbeCreateProcess(LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process) {
    if (!mock_process) return CreateProcessW(app, command, process_attributes, thread_attributes,
        inherit, flags, environment, directory, startup, process);
    ++process_calls;
    process_file = app ? app : L"";
    if (command && command[0]) command[0] = L'\0';
    SetLastError(process_error); return FALSE;
}
BOOL WINAPI ProbeShellExecute(SHELLEXECUTEINFOW* shell) {
    if (!mock_shell) return ShellExecuteExW(shell);
    ++shell_calls;
    shell_file = shell->lpFile ? shell->lpFile : L"";
    shell_args = shell->lpParameters ? shell->lpParameters : L"";
    shell_directory = shell->lpDirectory ? shell->lpDirectory : L"";
    shell_verb = shell->lpVerb ? shell->lpVerb : L"";
    SetLastError(shell_error); return shell_success;
}
}
#define CreateProcessW fixture::ProbeCreateProcess
#define ShellExecuteExW fixture::ProbeShellExecute
// Compile the production launch path with API interception confined to this test.
#include "../shell_host/shell_launch_worker.cpp"
#undef CreateProcessW
#undef ShellExecuteExW
#include "../app/context_menu_controller.h"

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* label) {
    ++checks; if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
}
const std::vector<pulse::ui::FluentMenuItem>& Rows(const std::vector<pulse::ui::FluentMenuItem>& rows) {
    for (const auto& row : rows) if (row.glyph == L"\xE712") return row.children;
    static const std::vector<pulse::ui::FluentMenuItem> empty;
    return empty;
}
void Cache() {
    using namespace pulse::app;
    ContextMenuPrefs prefs; prefs.persist = false;
    ContextMenuController controller;
    uint32_t token = 1;
    ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, auto, auto, auto, auto) { return token++; };
    controller.SetShellOperations(std::move(operations));
    pulse::ops::ShellMenuItem first, second;
    first.id = 1; first.text = L"Stable first"; first.verb = L"fixture-first";
    second.id = 2; second.text = L"Stable second"; second.verb = L"fixture-second";
    const auto start = [&] {
        controller.Close();
        controller.StartQuery(prefs, nullptr, {L"C:\\one.fixture"}, false, L".fixture", false, {}, {});
    };
    start(); controller.CompleteComQuery(1, {first, second}, GetTickCount64());
    start(); controller.CompleteComQuery(2, {first}, GetTickCount64(), true);
    bool changed = false;
    Check(Rows(controller.BuildDisplay(prefs, {}, changed)).size() == 1,
        "F01 partial reply remains usable in its current session");
    start(); Check(controller.SeedComItemsFromCache(), "F01 previous complete snapshot still seeds");
    const auto cached = controller.BuildDisplay(prefs, {}, changed);
    Check(Rows(cached).size() == 2, "F01 partial reply cannot replace the next session's complete cache");
    Check(Rows(cached).size() == 2 && !Rows(cached).back().enabled,
        "F01 cached commands stay disabled until current session validates them");
    controller.CompleteComQuery(3, {}, GetTickCount64());
    start(); Check(!controller.SeedComItemsFromCache(), "F01 empty final result clears old complete cache");
    controller.InvalidateCaches();
    start(); controller.CompleteComQuery(5, {first}, GetTickCount64(), true);
    start(); Check(!controller.SeedComItemsFromCache(), "F01 cold partial reply creates no reusable cache");
}
void AdminMenu() {
    using namespace pulse::app;
    ContextMenuPrefs prefs; prefs.persist = false;
    ContextMenuController controller;
    uint32_t token = 0, invoked_token = 0, invoked_id = 0;
    std::wstring executed_path, executed_verb;
    ContextMenuController::ShellOperations operations;
    operations.query = [&](auto, auto, auto, auto, auto) { return ++token; };
    operations.invoke = [&](uint32_t session, uint32_t id, auto, auto) {
        invoked_token = session; invoked_id = id;
    };
    operations.execute_verb = [&](const auto& path, const auto& verb) {
        executed_path = path; executed_verb = verb;
    };
    controller.SetShellOperations(std::move(operations));
    pulse::ui::FluentMenuItem open, properties;
    open.command = CmdOpen; open.text = L"Open"; open.separator_after = true;
    properties.command = CmdProperties; properties.text = L"Properties";
    const std::vector<pulse::ui::FluentMenuItem> base{open, properties};
    bool changed = false;
    const auto start = [&](const std::wstring& extension = L".exe", bool background = false) {
        controller.Close();
        controller.StartQuery(prefs, nullptr, {L"C:\\fixture.exe"}, background, extension, false, {}, {});
        controller.OpenMenu(base);
    };
    start();
    auto display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() >= 2 && display[1].command == CmdProperties,
        "Admin menu is absent when no runas verb is registered");
    StaticVerb admin, chooser, software;
    admin.verb = L"RuNaS"; admin.display = L"Run as administrator";
    admin.command = L"untrusted expanded command";
    chooser.verb = L"openas"; chooser.display = L"Open with";
    software.verb = L"fixture-software"; software.display = L"Fixture software";
    controller.CompleteStaticVerbs(L".exe", {software, admin, chooser});
    start();
    display = controller.BuildDisplay(prefs, base, changed);
    const int admin_command = display.size() > 1 ? display[1].command : 0;
    Check(display.size() > 3 && display[1].text == admin.display &&
        display[2].text == chooser.display && display[1].children.empty(),
        "Admin menu appears immediately after Open and before Open with");
    Check(display[1].glyph == L"\xE83D" && !display[0].separator_after &&
        !display[1].separator_after && display[2].separator_after,
        "Admin menu has shield and retains the Open group separator");
    Check(Rows(display).size() == 1 && Rows(display).front().text == software.display,
        "Admin menu is removed from the overflow while software remains there");
    controller.CloseMenu();
    Check(controller.ExecuteShellCommand(admin_command, {}, {}) &&
        executed_path == L"C:\\fixture.exe" && executed_verb == L"runas",
        "Admin menu delegates to the existing runas operation without executing registry commands");
    start();
    prefs.SetItemEnabled(pulse::ipc::CatalogKey(admin.display, false), false);
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() > 1 && display[1].text == chooser.display,
        "Admin menu respects the explicit per-item disabled preference");
    prefs.item_enabled.clear(); prefs.software = false;
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() > 1 && display[1].text == chooser.display,
        "Admin menu respects the software category preference");
    prefs.software = true; prefs.explorer_cap = 1;
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() > 1 && display[1].text == admin.display,
        "Admin menu remains available when the Explorer section cap is one");
    prefs.explorer_cap = 48;
    pulse::ops::ShellMenuItem com;
    com.id = 11; com.text = L"COM administrator"; com.verb = L"runas";
    com.clsid = L"{fixture-admin}";
    controller.CompleteComQuery(token, {com}, GetTickCount64());
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display[1].text == admin.display && Rows(display).size() == 1,
        "Static and COM runas entries produce one top-level action without overflow duplicates");
    start(L".com-only");
    controller.CompleteComQuery(token, {com}, GetTickCount64());
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() > 1 && display[1].command == CmdShellComBase + 11 &&
        display[1].enabled && display[1].live_update,
        "A live COM-only runas action is promoted and retains its live command identity");
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 11, {}, {});
    Check(invoked_token == token && invoked_id == 11,
        "Promoted COM runas invokes the exact displayed live session");
    invoked_token = invoked_id = 0;
    controller.Close();
    controller.StartQuery(prefs, nullptr, {L"C:\\fixture.exe"}, false, L".com-only", false, {}, {});
    Check(controller.SeedComItemsFromCache(), "Completed COM runas can seed presentation cache");
    controller.OpenMenu(base);
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() > 1 && display[1].text == com.text && !display[1].enabled,
        "Cached COM runas remains disabled until the current query validates it");
    controller.NotePatchedDisplay(); controller.CloseMenu();
    controller.ExecuteShellCommand(CmdShellComBase + 11, {}, {});
    Check(!invoked_token && !invoked_id, "Cached COM runas cannot launch a stale command");
    start(L".exe", true);
    display = controller.BuildDisplay(prefs, base, changed);
    Check(display.size() > 1 && display[1].text == chooser.display,
        "Background menus do not promote a file-only runas action");
    controller.Close();
}
HRESULT Execute(const pulse::ipc::ShellLaunchRequest& request) {
    Microsoft::WRL::ComPtr<IDataObject> data;
    std::vector<std::unique_ptr<pulse::ops::ImageOpenSession>> sessions;
    std::wstring detail;
    return pulse::shell::Execute(request, nullptr, data, sessions, detail, [] { return true; });
}
void CommandExpansion() {
    const std::wstring file = L"C:\\fixture\\literal%1%L%l%V%v%*.txt";
    const auto expanded = pulse::ops::ExpandShellCommand(L"viewer.exe \"%1\" %L %*", file);
    int count = 0;
    auto* arguments = CommandLineToArgvW(expanded.c_str(), &count);
    Check(arguments && count == 4 && arguments[1] == file && arguments[2] == file && arguments[3] == file,
        "P13 inserted file-name placeholders stay literal during one expansion pass");
    LocalFree(arguments);
    const auto root = pulse::ops::ExpandShellCommand(L"viewer.exe \"%1\"", L"C:\\");
    arguments = CommandLineToArgvW(root.c_str(), &count);
    Check(arguments && count == 2 && std::wstring_view(arguments[1]) == L"C:\\",
        "P13 directory argument preserves its final backslash through Windows parsing");
    LocalFree(arguments);
    Check(pulse::ops::ExpandShellCommand(L"viewer.exe \"%*\" %unknown", L"\\\\?\\UNC\\server\\share\\file.txt") ==
        L"viewer.exe \"\\\\server\\share\\file.txt\" %unknown",
        "P13 quoted all-files placeholder uses the same UNC normalization and preserves unknown tokens");
    Check(pulse::ops::ExpandShellCommand(L"%SystemRoot%\\viewer.exe --literal=100%%", file) ==
        L"%SystemRoot%\\viewer.exe --literal=100%%", "P13 unrelated percent text is unchanged");
}
void OpenDirectories(const std::filesystem::path& root) {
    wchar_t previous[32768]{};
    GetCurrentDirectoryW(ARRAYSIZE(previous), previous);
    const bool isolated = SetCurrentDirectoryW(root.c_str()) != FALSE;
    Check(isolated, "F03 batch launch inherits only an isolated fixture directory");
    if (!isolated) return;
    const auto left = root / L"script one", right = root / L"script two";
    std::filesystem::create_directories(left); std::filesystem::create_directories(right);
    for (const auto& directory : {left, right}) {
        std::ofstream(directory / L"data.txt") << "fixture";
        std::ofstream(directory / L"helper.bat") << "@echo HELPER_OK>>result.txt\n";
        std::ofstream(directory / L"run.bat") << "@echo off\nif exist data.txt echo FOUND>result.txt\ncall .\\helper.bat\n";
    }
    pulse::ipc::ShellLaunchRequest request;
    request.flags = 0;
    request.paths = {(left / L"run.bat").wstring(), (right / L"run.bat").wstring()};
    const auto result = Execute(request);
    const auto deadline = GetTickCount64() + 5000;
    bool complete = false;
    do {
        complete = true;
        for (const auto& directory : {left, right}) {
            std::ifstream input(directory / L"result.txt");
            std::string text((std::istreambuf_iterator<char>(input)), {});
            complete &= text.find("FOUND") != std::string::npos && text.find("HELPER_OK") != std::string::npos;
        }
        if (complete) break;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    Check(SUCCEEDED(result) && complete, "F03 real ordinary batch opens resolve sibling data and helper in each target folder");
    fixture::mock_shell = true;
    request.paths = {L"C:\\fixture\\a.txt", L"D:\\other\\b.txt"};
    Execute(request);
    Check(fixture::shell_directory == L"D:\\other", "F03 multi-path ordinary launch chooses each path's parent");
    request.directory = L"C:\\explicit working directory";
    Execute(request);
    Check(fixture::shell_directory == request.directory, "F03 explicitly requested working directory takes precedence");
    request.directory.clear(); request.paths = {L"shell:Downloads"}; Execute(request);
    Check(fixture::shell_directory.empty(), "F03 virtual shell parsing names receive no filesystem working directory");
    request.paths = {L"\\\\?\\C:\\fixture\\a.txt"}; Execute(request);
    Check(fixture::shell_directory == L"C:\\fixture", "F03 extended path prefix is removed for Shell working directory");
    request.paths = {L"\\\\server\\share\\folder\\a.txt"}; Execute(request);
    Check(fixture::shell_directory == L"\\\\server\\share\\folder", "F03 UNC parent is selected lexically without accessing share");
    request.paths = {L"C:\\"}; Execute(request);
    Check(fixture::shell_directory == L"C:\\", "F03 drive root remains a valid working directory");
    request.paths = {L"\\\\server\\share\\"}; Execute(request);
    Check(fixture::shell_directory == L"\\\\server\\share\\", "F03 UNC share root remains a valid working directory");
    request.paths = {L"\\\\server"}; Execute(request);
    Check(fixture::shell_directory.empty(), "F03 UNC server browsing receives no invalid filesystem working directory");
    request.paths = {L"C:\\fixture\\folder\\"}; Execute(request);
    Check(fixture::shell_directory == L"C:\\fixture\\folder\\", "F03 trailing directory separator selects that directory");
    request.paths = {L"relative.txt"}; Execute(request);
    wchar_t current[32768]{}; GetCurrentDirectoryW(ARRAYSIZE(current), current);
    Check(fixture::shell_directory == current, "F03 relative filesystem item is resolved before selecting its parent");
    fixture::mock_shell = false;
    Check(SetCurrentDirectoryW(previous) != FALSE, "F03 restores caller working directory after isolated batch check");
}
void Elevation(const std::filesystem::path& root) {
    fixture::mock_process = fixture::mock_shell = true;
    fixture::process_error = ERROR_ELEVATION_REQUIRED; fixture::shell_success = true;
    pulse::ipc::ShellLaunchRequest request;
    request.kind = pulse::ipc::ShellLaunchKind::Command;
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    request.executable = L"\"" + std::wstring(module) + L"\" --monitor \"C:\\selected file.exe\"";
    request.parameters = L"--extra \"literal value\"";
    request.directory = root.wstring();
    fixture::shell_calls = 0;
    const auto result = Execute(request);
    Check(SUCCEEDED(result) && fixture::shell_calls == 1 && fixture::shell_verb == L"runas",
        "F14 command error 740 retries exactly once through runas without real elevation");
    Check(fixture::shell_calls == 1 && fixture::shell_file == module &&
        fixture::shell_args == L"--monitor \"C:\\selected file.exe\" --extra \"literal value\"" &&
        fixture::shell_directory == request.directory,
        "F14 fallback preserves quoted executable, original arguments and working directory");
    Check(fixture::process_file == module, "P13 CreateProcess receives an explicit executable path");
    fixture::shell_success = false; fixture::shell_error = ERROR_CANCELLED;
    Check(Execute(request) == HRESULT_FROM_WIN32(ERROR_CANCELLED), "F14 cancelled elevation preserves cancellation result");
    fixture::process_error = ERROR_FILE_NOT_FOUND; fixture::shell_calls = 0;
    Check(Execute(request) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && fixture::shell_calls == 0,
        "F14 non-740 command errors never trigger elevation");
    fixture::process_error = ERROR_ELEVATION_REQUIRED; fixture::shell_success = true;
    request.executable = std::wstring(module) + L" --unquoted \"C:\\selected file.exe\"";
    request.parameters.clear();
    Check(SUCCEEDED(Execute(request)) && fixture::shell_file == module &&
        fixture::shell_args == L"--unquoted \"C:\\selected file.exe\"",
        "F14 unquoted executable preserves argument tail after CreateProcess mutates its input");
    const auto spaced = root / L"executable with spaces.exe";
    std::filesystem::copy_file(module, spaced, std::filesystem::copy_options::overwrite_existing);
    const auto prefix = root / L"executable.exe";
    std::filesystem::copy_file(module, prefix, std::filesystem::copy_options::overwrite_existing);
    request.executable = spaced.wstring() + L" --spaced \"unchanged argument\"";
    fixture::process_calls = fixture::shell_calls = 0;
    Check(Execute(request) == E_INVALIDARG && fixture::process_calls == 0 && fixture::shell_calls == 0,
        "P13 ambiguous unquoted path cannot launch its existing executable prefix");
    request.executable = L"\"" + spaced.wstring() + L"\" --spaced \"unchanged argument\"";
    Check(SUCCEEDED(Execute(request)) && fixture::shell_file == spaced.wstring() &&
        fixture::shell_args == L"--spaced \"unchanged argument\"",
        "P13 quoted executable with spaces preserves its exact target through elevation");
    request.executable = L"\"" + std::wstring(module) + L"\"";
    Check(SUCCEEDED(Execute(request)) && fixture::shell_args.empty(), "F14 command with no parameters keeps an empty argument tail");
    fixture::shell_success = false; fixture::shell_error = ERROR_ACCESS_DENIED;
    Check(Execute(request) == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), "F14 failed elevation preserves the Shell error");
    fixture::shell_success = true; fixture::shell_calls = 0;
    request.executable = L"\"unterminated command";
    Check(Execute(request) == E_INVALIDARG && fixture::shell_calls == 0,
        "P13 unparseable command fails before launching a target");
    fixture::mock_process = fixture::mock_shell = false;
}
}
int wmain(int argc, wchar_t** argv) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc == 2 && wcscmp(argv[1], L"--admin-menu-only") == 0) {
        AdminMenu();
        if (SUCCEEDED(apartment)) CoUninitialize();
        std::printf("RESULT checks=%d failures=%d\n", checks, failures);
        return failures ? 1 : 0;
    }
    const auto root = std::filesystem::absolute(L"bench_data/issue-confirm-shell-fixes/run-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root.parent_path());
    if (!std::filesystem::create_directory(root)) return 2;
    const bool commands_only = argc == 2 && wcscmp(argv[1], L"--commands-only") == 0;
    if (!commands_only) { Cache(); OpenDirectories(root); }
    CommandExpansion(); Elevation(root);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    Check(!error && !std::filesystem::exists(root), "isolated launch regression fixtures are removed");
    if (SUCCEEDED(apartment)) CoUninitialize();
    std::printf("RESULT checks=%d failures=%d fixture=%ls\n", checks, failures, root.c_str());
    return failures ? 1 : 0;
}
