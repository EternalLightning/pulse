// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/ui_renderer_internal.h"
#include "content_search_fixture.h"
#include "../common/command_line.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string_view>

namespace {
class SharedSettingsFeed {
public:
    ~SharedSettingsFeed() { Stop(); }
    void Stop() {
        if (process_) {
            HANDLE pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                const auto header = pulse::index::MakeIndexHdr(pulse::index::REQ_IDX_TEST_SHUTDOWN, 0, 0);
                DWORD written = 0; WriteFile(pipe, &header, sizeof(header), &written, nullptr); CloseHandle(pipe);
            }
            if (WaitForSingleObject(process_, 5000) != WAIT_OBJECT_0) {
                TerminateJobObject(job_, ERROR_CANCELLED); WaitForSingleObject(process_, 5000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
        if (overridden_) { SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", previous_.empty() ? nullptr : previous_.c_str()); overridden_ = false; }
    }
    bool Start(const std::filesystem::path& root, const std::filesystem::path& cache) {
        std::filesystem::create_directory(cache);
        wchar_t executable[32768]{}, previous[32768]{};
        if (!GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable))) return false;
        const auto count = GetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", previous, ARRAYSIZE(previous));
        if (count >= ARRAYSIZE(previous)) return false;
        if (count) previous_.assign(previous, count);
        const auto token = L"shared-settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        pipe_name_ = L"\\\\.\\pipe\\PulseIndex.Test." + token;
        if (!SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", pipe_name_.c_str())) return false;
        overridden_ = true;
        const auto helper = std::filesystem::path(executable).parent_path() / L"Pulse.Index.exe";
        std::wstring command = pulse::QuoteWindowsArgument(helper.wstring()) + L" --test-host " + token + L" " +
            pulse::QuoteWindowsArgument(root.wstring()) + L" " + pulse::QuoteWindowsArgument(cache.wstring());
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION created{};
        if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &created)) return false;
        if (!AssignProcessToJobObject(job_, created.hProcess)) {
            TerminateProcess(created.hProcess, ERROR_CANCELLED); CloseHandle(created.hThread); CloseHandle(created.hProcess); return false;
        }
        process_ = created.hProcess; ResumeThread(created.hThread); CloseHandle(created.hThread);
        return true;
    }
private:
    HANDLE process_ = nullptr, job_ = nullptr;
    std::wstring pipe_name_, previous_;
    bool overridden_ = false;
};
}

int main(int argc, char** argv) {
    using namespace pulse;
    const bool storage_isolation_only = argc == 2 && std::string_view(argv[1]) == "--storage-isolation-only";
    const bool layout_only = argc == 2 && std::string_view(argv[1]) == "--layout-only";
    if (argc > 2 || (argc == 2 && !storage_isolation_only && !layout_only)) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    const auto output = std::filesystem::absolute(L"../bench_data/shared-settings");
    bench::ContentSearchFixture fixture;
    if (!fixture.Initialize(output, L"isolation")) {
        check(false, "initialize private content database and storage sentinels");
        std::cerr << "[INFO] fixture stage=" << fixture.Stage() << " error=" << GetLastError() << std::endl;
        std::wcerr << L"[INFO] attempted=" << fixture.AttemptedPath() << L" fs_error=" << fixture.FilesystemError() << std::endl;
        CoUninitialize(); return 1;
    }
    std::wcout << L"[INFO] private content fixture: " << fixture.Root() << std::endl;
    SharedSettingsFeed feed;
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.isolatedTest = true; s.appPrefs.persist = false;
    check(!s.contentSearch.SetDatabasePathForTesting(L"relative.sqlite") && GetLastError() == ERROR_INVALID_PARAMETER,
        "database override rejects relative paths while stopped");
    check(s.contentSearch.SetDatabasePathForTesting({}), "empty stopped database override restores default selection");
    if (!s.contentSearch.SetDatabasePathForTesting(fixture.DatabasePath())) {
        check(false, "bind settings client to private database");
        CoUninitialize(); return 1;
    }
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseSharedSettingsFixture"; RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 1200, 900, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = L"pulse:settings:search";
    s.settings.SelectPage(1);
    ui::fluent::Painter painter(&s.compositor);
    if (!storage_isolation_only) for (const auto* language : {L"zh-CN", L"en-US"}) for (float scale : {1.0f, 1.5f, 2.0f}) {
        l10n::SetLanguage(language); s.scale = scale;
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale); painter.SetScale(scale);
        for (int width : {760, 1280}) for (bool populated : {false, true}) {
            const auto rect = D2D1::RectF(0, 0, width*scale, 1000*scale);
            s.compositor.Resize(static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom));
            auto vm = BuildVm(s, false);
            vm.settings_open = true; vm.settings_page = 1; vm.settings_expanded = 0;
            vm.settings_scroll = 0; vm.settings_content_folders.clear();
            wchar_t summary[160]{};
            swprintf_s(summary,l10n::Get(l10n::StringId::SettingsContentSummaryFormat).c_str(),2ull,82951ull);
            vm.settings_content_summary = summary;
            vm.settings_index_status = l10n::Get(l10n::StringId::ContentIndexReady);
            if (populated) vm.settings_content_folders = {{L"C:\\", L"Ready", false}, {L"D:\\", L"Ready", false}};
            const auto layout = ui::MakeSettingsLayout(vm, rect, scale, s.renderer.TitleBarHeight(), 28*scale, &painter);
            auto hit = [&](const auto& r) { return s.renderer.HitTest(vm, rect, (r.left+r.right)/2, (r.top+r.bottom)/2); };
            check(hit(layout.content_header).region != ui::HitTestResult::SettingsContentAction,
                "shared scope header has no maintenance action");
            check(layout.section[2].bottom == 0 && (!populated || layout.content_rebuild.bottom <= layout.group[1].bottom),
                "rebuild belongs to content card without a separate more-options group");
            check(layout.content_types.left >= layout.content_header.left && layout.content_types.right <= layout.content_header.right,
                "supported types stay inside the content card");
            if (populated) {
                auto action_vm = vm;
                const float maximum = s.renderer.SettingsMaxScroll(action_vm, rect.right, rect.bottom);
                const float needed = layout.content_rebuild.bottom > layout.content.bottom ?
                    layout.content_rebuild.bottom - layout.content.bottom + 8*scale : 0.0f;
                action_vm.settings_scroll = std::clamp(needed, 0.0f, maximum);
                const auto action_layout = ui::MakeSettingsLayout(action_vm, rect, scale,
                    s.renderer.TitleBarHeight(), 28*scale, &painter);
                auto visible = [&](const auto& r) {
                    return r.right > r.left && r.bottom > r.top &&
                        r.left >= action_layout.content.left && r.right <= action_layout.content.right &&
                        r.top >= action_layout.content.top && r.bottom <= action_layout.content.bottom;
                };
                const bool actions_visible = visible(action_layout.content_options) && visible(action_layout.content_rebuild);
                check(actions_visible, "encoding and rebuild are fully visible after scrolling");
                auto action_hit = [&](const auto& r) {
                    return s.renderer.HitTest(action_vm, rect, (r.left+r.right)/2, (r.top+r.bottom)/2);
                };
                const auto options_hit = action_hit(action_layout.content_options);
                const auto rebuild_hit = action_hit(action_layout.content_rebuild);
                const bool actions_hit = options_hit.region == ui::HitTestResult::SettingsContentAction && options_hit.index == 2 &&
                    rebuild_hit.region == ui::HitTestResult::SettingsContentAction && rebuild_hit.index == 3;
                check(actions_hit, "encoding and rebuild have separate working hit targets");
                if (!actions_visible || !actions_hit) {
                    std::cout << "[INFO] settings actions language=" << (std::wstring_view(language) == L"zh-CN" ? "zh-CN" : "en-US")
                        << " scale=" << scale << " width=" << width << " scroll=" << action_vm.settings_scroll
                        << " maximum=" << maximum << " viewport=" << action_layout.content.top << "," << action_layout.content.bottom
                        << " options=" << action_layout.content_options.top << "," << action_layout.content_options.bottom
                        << " rebuild=" << action_layout.content_rebuild.top << "," << action_layout.content_rebuild.bottom
                        << " hits=" << static_cast<int>(options_hit.region) << ":" << options_hit.index << ","
                        << static_cast<int>(rebuild_hit.region) << ":" << rebuild_hit.index << std::endl;
                }
            }
            if (scale == 1.0f && populated) for (bool dark : {false, true}) {
                const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, rect, theme); s.compositor.Dc()->EndDraw();
                const auto file = output / (std::wstring(language) + L"-" + std::to_wstring(width) + (dark ? L"-dark.png" : L"-light.png"));
                check(s.compositor.SaveSnapshot(file.c_str()), "shared settings screenshot captured");
            }
        }
    }
    if (layout_only) {
        check(fixture.DefaultConfigurationUnchanged(), "private default configuration sentinel stays unchanged");
        check(fixture.LocatorUnchanged(), "private file locator sentinel stays unchanged");
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        CoUninitialize(); return failures ? 1 : 0;
    }
    ui::HitTestResult action; action.region = ui::HitTestResult::SettingsContentAction; action.index = 0;
    HandleSettingsControl(s, action);
    check((s.settingsExpanded & 2u) != 0, "scope button opens existing index maintenance");
    ShowSearchOptions(s, true);
    check(s.settings.page() == 1, "search management opens the same settings page");
    const auto files = fixture.Root() / L"files";
    std::filesystem::create_directories(files);
    std::ofstream(files / L"one.txt") << "shared settings content";
    if (!feed.Start(files, fixture.Root() / L"name-index")) {
        check(false, "start private filename feed for shared content scope");
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
        CoUninitialize(); return 1;
    }
    s.contentSearch.Start(nullptr, 0);
    check(!s.contentSearch.SetDatabasePathForTesting(fixture.DatabasePath() + L".other") && GetLastError() == ERROR_BUSY,
        "running client rejects database replacement");
    index::ContentIndexConfig config; config.roots = {{files.wstring()}}; config.shared_scope = true;
    s.contentSearch.Configure(config);
    auto wait = [&](auto ready) {
        const auto deadline = GetTickCount64() + 10000;
        while (!ready() && GetTickCount64() < deadline) Sleep(10);
        return ready();
    };
    check(wait([&] { return s.contentSearch.GetStatus().indexed_files == 1 && !s.contentSearch.GetStatus().indexing; }),
        "isolated content settings connect to the real index agent");
    action.index = 1; HandleSettingsControl(s, action);
    check(wait([&] { return s.contentSearch.GetStatus().paused; }), "pause action reaches index agent");
    HandleSettingsControl(s, action);
    check(wait([&] { return !s.contentSearch.GetStatus().paused; }), "resume action reaches index agent");
    EnsureMenu(s); s.menu->SetHoverFirstOnOpen(true);
    PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_DOWN, 0);
    PostThreadMessageW(GetCurrentThreadId(), WM_KEYDOWN, VK_RETURN, 0);
    action.index = 2; HandleSettingsControl(s, action);
    check(wait([&] { auto current = s.contentSearch.GetConfig(); return current.roots.size() == 1 &&
        current.roots.front().encoding == text::Encoding::Utf8 && current.shared_scope; }),
        "encoding menu changes parsing without changing shared scope");
    const auto revision = s.contentSearch.GetStatus().revision;
    action.index = 3; HandleSettingsControl(s, action);
    check(wait([&] { const auto status = s.contentSearch.GetStatus(); return status.revision > revision &&
        status.indexed_files == 1 && !status.indexing && !status.pending_files; }),
        "rebuild action completes against the same scope");
    s.contentSearch.Stop();
    feed.Stop();
    config.roots.front().encoding = text::Encoding::Utf8;
    config.default_encoding = text::Encoding::Utf8;
    check(fixture.ConfigurationMatches(config), "settings agent writes only the selected database configuration");
    check(fixture.DefaultConfigurationUnchanged(), "private default configuration sentinel stays unchanged");
    check(fixture.LocatorUnchanged(), "private file locator sentinel stays unchanged");
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    CoUninitialize(); return failures ? 1 : 0;
}
