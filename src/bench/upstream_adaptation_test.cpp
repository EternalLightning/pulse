#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../ui/home_layout.h"
#include "../index/content_search_session.h"
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace pulse {
int RunWindowTransferRegressionChecks();
int RunNetworkAccessApplicationChecks();
}

namespace {
int SearchRebindChecks() {
    using namespace pulse::index;
    int failures = 0;
    const auto check = [&](bool passed, const char* description) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
        if (!passed) ++failures;
    };
    const auto temp = std::filesystem::absolute(L"bench_data/upstream-adaptation-20261025/spool");
    std::filesystem::create_directories(temp);
    SetEnvironmentVariableW(L"TEMP", temp.c_str());
    SetEnvironmentVariableW(L"TMP", temp.c_str());
    auto previous = std::make_shared<ContentResultStore>(nullptr, 0);
    ContentHit hit;
    hit.path = L"C:\\fixture\\retained.txt";
    hit.name = L"retained.txt";
    hit.snippet = L"retained result";
    const std::vector<ContentHit> hits{hit};
    check(previous->Append(hits), "prepare retained content results in isolated spool");
    ContentSearchRequest request;
    request.generation = 77;
    request.paged_results = true;
    request.task_scan = true;
    request.previous_results = previous;
    request.resume_previous = true;
    ContentSearchSession resumed(request, nullptr, 0);
    ContentSearchUpdate progress;
    progress.progress.generation = 77;
    check(!resumed.Accept(std::move(progress)), "rebound search keeps published rows while its new owner is scanning");
    ContentSearchUpdate done;
    done.progress.generation = 77;
    done.progress.done = true;
    done.hits = hits;
    const auto completion = resumed.Accept(std::move(done));
    check(completion && completion->results == previous,
        "unchanged rebound content query retains the original store, selection and filter identity");
    ContentSearchSession failed(request, nullptr, 0);
    const auto failure = failed.Fail(ERROR_CANCELLED);
    check(failure && failure->results == previous && failure->progress.error == ERROR_CANCELLED,
        "failed rebound request retains published results without hiding the error");
    request.resume_previous = false;
    ContentSearchSession ordinary(request, nullptr, 0);
    ContentSearchUpdate batch;
    batch.progress.generation = 77;
    batch.hits = hits;
    const auto streamed = ordinary.Accept(std::move(batch));
    check(streamed && streamed->results && streamed->results != previous,
        "ordinary task searches keep their existing streaming behavior");
    return failures ? 1 : 0;
}

int HomeNoticeChecks() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool passed, const char* description) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
        if (!passed) ++failures;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.shot.active = true;
    s.accentColor = ui::HexColor(0x0078D4);
    s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
    s.sidebarCollapsedMask = 0;
    s.sidebar = {};
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Home network notice fixture",
        WS_POPUP, -20000, -20000, 1000, 800, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
        check(false, "initialize isolated Home notice renderer");
        if (s.hwnd) DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
        return 1;
    }
    s.window_tabs.NewTab(app::MakeHomePath());
    BindCurrentLayout(s);
    s.network_access = std::make_shared<app::NetworkAccessState>();
    s.network_access->discovery = app::NetworkDiscoveryState::Disabled;
    s.renderer.SetCompositor(&s.compositor);
    SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
    const auto output = std::filesystem::absolute(L"bench_data/upstream-adaptation-20261025");
    std::filesystem::create_directories(output);
    for (const bool dark : {false, true}) {
        for (const float scale : {1.0f, 1.5f}) {
            s.darkMode = dark;
            s.scale = scale;
            s.renderer.SetScale(scale);
            s.compositor.RecreateTextFormats(scale);
            const int width = scale > 1 ? 760 : 1000;
            SetWindowPos(s.hwnd, nullptr, 0, 0, width, 800, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            s.compositor.Resize(width, 800);
            const auto vm = BuildVm(s, false);
            bool hit = false;
            const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), 800);
            for (float y = 0; y < 800 && !hit; y += 4)
                for (float x = 0; x < static_cast<float>(width) && !hit; x += 4)
                    hit = s.renderer.HitTest(vm, bounds, x, y).region == ui::HitTestResult::HomeNetworkSettings;
            check(hit, "production Home network notice is visible and hit-testable in light/dark and narrow/scaled views");
            Render(s);
            const auto filename = output / ((dark ? std::wstring(L"dark-") : L"light-") +
                (scale > 1 ? L"150.png" : L"100.png"));
            check(s.compositor.SaveSnapshot(filename.c_str()), "capture production Home network notice");
        }
    }
    CancelNetworkAccess(s);
    s.renderer.SetCompositor(nullptr);
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(OleInitialize(nullptr))) return 2;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int result = 2;
    if (argc == 2 && std::string_view(argv[1]) == "--transfer-only") result = pulse::RunWindowTransferRegressionChecks();
    else if (argc == 2 && std::string_view(argv[1]) == "--network-only") result = pulse::RunNetworkAccessApplicationChecks();
    else if (argc == 2 && std::string_view(argv[1]) == "--home-notice-only") result = HomeNoticeChecks();
    else if (argc == 2 && std::string_view(argv[1]) == "--search-rebind-only") result = SearchRebindChecks();
    OleUninitialize();
    return result;
}
