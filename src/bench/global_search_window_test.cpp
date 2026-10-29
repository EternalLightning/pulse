#include <windows.h>
#include <memory>
#include <string>
#include <string_view>
#include "../app/global_search_window.h"
#include "../app/global_search_window.cpp"
#include <wincodec.h>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace pulse {
struct GlobalSearchWindowTestPeer {
    using State = GlobalSearchWindow::Impl;
    static State& Get(GlobalSearchWindow& window) { return *window.impl_; }
};
}

namespace pulse::index {
struct ContentRefreshTestPeer {
    static void Queue(ContentSearchClient& client, ContentSearchUpdate update) {
        std::lock_guard lock(client.updates_mu_);
        client.updates_.push_back(std::move(update));
    }
};
}

namespace {
void Pump(DWORD milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(10);
    } while (GetTickCount64() < end);
}
bool SelectionMarkerMatches(pulse::GlobalSearchWindowTestPeer::State& state, int row) {
    state.selected = row;
    state.Paint();
    auto* dc = state.compositor.Dc();
    if (!dc) return false;
    Microsoft::WRL::ComPtr<ID2D1Image> source;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap, sample;
    dc->GetTarget(&source);
    HRESULT result = source.As(&bitmap);
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(result)) result = dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, properties, &sample);
    const UINT y = static_cast<UINT>(std::floor(136 + static_cast<float>(row) * 88 - state.scroll_y + 14));
    const D2D1_RECT_U area{6, y, 7, y + 1};
    if (SUCCEEDED(result)) result = sample->CopyFromBitmap(nullptr, bitmap.Get(), &area);
    D2D1_MAPPED_RECT pixel{};
    if (SUCCEEDED(result)) result = sample->Map(D2D1_MAP_OPTIONS_READ, &pixel);
    if (FAILED(result)) return false;
    const auto accent = state.Theme().accent;
    const bool matches = std::abs(static_cast<int>(pixel.bits[0]) - static_cast<int>(std::lround(accent.b * 255))) <= 5 &&
        std::abs(static_cast<int>(pixel.bits[1]) - static_cast<int>(std::lround(accent.g * 255))) <= 5 &&
        std::abs(static_cast<int>(pixel.bits[2]) - static_cast<int>(std::lround(accent.r * 255))) <= 5;
    sample->Unmap();
    return matches;
}
bool Capture(pulse::GlobalSearchWindowTestPeer::State& state, const std::wstring& path) {
    Pump(30);
    const UINT width = static_cast<UINT>(state.Px(state.width)), height = static_cast<UINT>(state.Px(state.height));
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmap> image;
    Microsoft::WRL::ComPtr<IWICStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &image);
    state.EnsureTarget();
    const auto live_target = state.target;
    const auto live_brush = state.brush;
    state.target.Reset(); state.brush.Reset();
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE);
    properties.dpiX = properties.dpiY = 96 * state.scale;
    if (SUCCEEDED(result)) result = state.factory->CreateWicBitmapRenderTarget(image.Get(), properties, &state.target);
    if (SUCCEEDED(result)) result = state.target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &state.brush);
    if (SUCCEEDED(result)) {
        state.Paint();
        RECT edit_rect{}; GetClientRect(state.edit, &edit_rect);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = edit_rect.right; info.bmiHeader.biHeight = -edit_rect.bottom;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        HDC memory = CreateCompatibleDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        const auto old = SelectObject(memory, bitmap);
        FillRect(memory, &edit_rect, state.background);
        if (!state.compositor.PaintLumaEdit(state.edit, memory, state.edit_format.Get(), state.Theme().text, state.EditBackground()))
            SendMessageW(state.edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT | PRF_ERASEBKGND);
        if (state.EditBackground().a == 0 && static_cast<const BYTE*>(pixels)[
            (static_cast<size_t>(edit_rect.bottom / 2) * edit_rect.right + edit_rect.right - 2) * 4 + 3] != 0) {
            std::cout << "[FAIL] Search edit background is not transparent\n";
            result = E_FAIL;
        }
        SelectObject(memory, old);
        Microsoft::WRL::ComPtr<IWICBitmap> edit_image;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> edit_bitmap;
        if (SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, &edit_image)) &&
            SUCCEEDED(state.target->CreateBitmapFromWicBitmap(edit_image.Get(), nullptr, &edit_bitmap))) {
            state.target->BeginDraw();
            state.target->DrawBitmap(edit_bitmap.Get(), {116, 23, state.width - 72, 58});
            const HRESULT painted_edit = state.target->EndDraw();
            if (SUCCEEDED(result)) result = painted_edit;
        }
        DeleteObject(bitmap); DeleteDC(memory);
    }
    state.target = live_target; state.brush = live_brush;
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result)) result = frame->WriteSource(image.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    if (FAILED(result)) std::cout << "Capture failure HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << std::dec << '\n';
    return SUCCEEDED(result);
}
}
int wmain(int argc, wchar_t** argv) {
    using SetAwareness = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const bool routing_only = (argc > 1 && std::wstring_view(argv[1]) == L"--routing-only") ||
        (argc > 2 && std::wstring_view(argv[2]) == L"--routing-only");
    const bool smooth_only = argc > 1 && std::wstring_view(argv[1]) == L"--smooth-scroll";
    std::filesystem::path output = argc > 1 && std::wstring_view(argv[1]) != L"--routing-only" && !smooth_only
        ? argv[1] : L"bench_data/global-search-window";
    std::filesystem::create_directories(output);
    const auto profile = output / L"profile";
    std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    int failed = 0;
    const auto check = [&](bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; if (!ok) ++failed; };
    if (smooth_only) {
        pulse::index::IndexClient filenames;
        pulse::index::NetworkAgentClient network;
        uint32_t request = 0;
        pulse::GlobalSearchWindow fixture;
        fixture.BindFilenameClients(filenames, network, request);
        auto& state = pulse::GlobalSearchWindowTestPeer::Get(fixture);
        // Use the production window procedure and compositor without Show(),
        // whose normal lifecycle starts the independent content search client.
        WNDCLASSW cls{};
        cls.lpfnWndProc = pulse::GlobalSearchWindowTestPeer::State::WindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"Pulse.GlobalSearch.SmoothFixture";
        RegisterClassW(&cls);
        state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"",
            WS_POPUP | WS_CLIPCHILDREN, 100, 100, 780, 488, nullptr, nullptr, cls.hInstance, &state);
        state.edit = pulse::ui::CreateChildEdit(state.hwnd);
        check(state.hwnd && state.edit, "create real popup controls without starting search workers");
        if (state.hwnd && state.edit) {
            ShowWindow(state.hwnd, SW_SHOWNOACTIVATE);
            state.Layout();
            state.motion_override = 1;
            for (int i = 0; i < 30; ++i)
                state.rows.push_back({L"Result " + std::to_wstring(i), L"C:\\fixture\\result.txt", {}, false});
            const auto start = pulse::ui::AnimationTimeMs();
            SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-15)), 0);
            check(state.scroll_motion.Pending() && state.scroll_y < 33.0f,
                "partial wheel starts continuous scrolling instead of discarding delta");
            state.TickScroll(start + 80);
            check(state.scroll_y > 14.0f && state.scroll_y < 33.0f && state.scroll_motion.Pending(),
                "popup has a fractional intermediate frame before the 33 DIP target");
            check(SelectionMarkerMatches(state, 1), "painted selection marker follows the actual fractional offset");
            const auto settle = [&](uint64_t now) {
                for (uint64_t step = 1; step <= 30 && state.scroll_motion.Pending(); ++step)
                    state.TickScroll(now + step * 50);
            };
            settle(start + 80);
            check(std::abs(state.scroll_y - 33.0f) < 0.1f && !state.scroll_motion.Pending() &&
                !KillTimer(state.hwnd, pulse::kScrollTimer), "partial wheel settles exactly and releases its timer");

            state.scroll_y = 0; state.StopScroll();
            SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-15)), 0);
            state.TickScroll(pulse::ui::AnimationTimeMs() + 80);
            state.Paint();
            const int expected = static_cast<int>((80.0f + state.scroll_y) / 88.0f);
            SendMessageW(state.hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(80, 216));
            check(expected == 1 && state.selected == expected && !state.scroll_motion.Pending(),
                "click selects the displayed partial row and cancels scrolling");

            state.scroll_y = state.MaxScroll() - 1.0f;
            state.StopScroll();
            SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
            settle(pulse::ui::AnimationTimeMs());
            check(state.scroll_y == state.MaxScroll() && !state.scroll_motion.Pending(),
                "scrolling clamps at the lower content boundary and stops");
            state.motion_override = 0;
            SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), 0);
            check(!state.scroll_motion.Pending() && std::abs(state.scroll_y - (state.MaxScroll() - 264.0f)) < 0.1f,
                "disabled motion applies the complete wheel delta immediately");

            state.motion_override = 1;
            SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-15)), 0);
            check(state.scroll_motion.Pending(), "start momentum before replacing search results");
            state.local_result.hits = {{L"C:\\fixture\\new.txt", L"new.txt", false}};
            state.local_result.total = 1;
            state.local_ready = state.network_ready = true;
            state.MergeFilenames();
            state.TickScroll(pulse::ui::AnimationTimeMs() + 1000);
            check(state.rows.size() == 1 && state.scroll_y == 0 && !state.scroll_motion.Pending() &&
                !KillTimer(state.hwnd, pulse::kScrollTimer), "replacing results clamps the offset and cancels momentum");
        }
        fixture.Shutdown();
        CoUninitialize();
        return failed ? 1 : 0;
    }
    pulse::GlobalSearchWindow window;
    const auto shared_lifecycle = [&] {
        pulse::index::IndexClient shared_filenames;
        pulse::index::NetworkAgentClient shared_network;
        uint32_t next_request = 100;
        window.BindFilenameClients(shared_filenames, shared_network, next_request);
        check(window.Show(nullptr, true, 1, output.wstring()), "Show supports borrowed filename clients");
        auto& shared_state = pulse::GlobalSearchWindowTestPeer::Get(window);
        check(shared_state.filenames == &shared_filenames && shared_state.network == &shared_network &&
            !shared_state.owned_filenames && !shared_state.owned_network,
            "Bound popup does not construct or Start duplicate filename clients");
        check(shared_state.generation == next_request && next_request > 100,
            "Popup consumes the owner's unique request id counter");
        const uint32_t previous = next_request;
        window.Hide();
        check(next_request > previous && shared_state.generation == next_request,
            "Hide invalidates the popup without resetting the owner's counter");
        const HWND popup = shared_state.hwnd;
        PostMessageW(popup, pulse::kIndexResult, previous, 0);
        PostMessageW(popup, pulse::kNetworkResult, previous, 0);
        window.Shutdown();
        MSG pending{};
        check(!PeekMessageW(&pending, popup, pulse::kIndexStatus, pulse::kNetworkResult, PM_REMOVE),
            "Shutdown drains queued search notifications before releasing the popup HWND");
    };
    if (routing_only) {
        shared_lifecycle();
        CoUninitialize();
        return failed ? 1 : 0;
    }
    check(window.Show(nullptr, true, 1, output.wstring()), "Show creates independent visible window");
    auto& state = pulse::GlobalSearchWindowTestPeer::Get(window);
    check(window.Visible() && GetFocus() == state.edit, "Show focuses native IME edit");
    check(GetWindow(state.hwnd, GW_OWNER) == nullptr, "Hidden main owner cannot hide popup");
    check((GetWindowLongPtrW(state.hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0,
        "Popup uses the main window no-redirection composition style");
    state.Cancel();
    check(Capture(state, (output / L"empty-dark.png").wstring()), "Capture empty dark");
    check(state.empty_art.Get() != nullptr, "Existing empty-state SVG resource renders successfully");

    SetWindowTextW(state.edit, L"季度预算");
    state.Cancel(); state.content_mode = true;
    state.rows = {
        {L"2026 年第三季度预算.xlsx", L"D:\\工作\\财务\\2026 年第三季度预算.xlsx", L"本次季度预算总额为 128 万元，主要用于产品研发…", false},
        {L"项目预算评审.pdf", L"D:\\工作\\项目资料\\项目预算评审.pdf", L"季度预算调整方案已通过评审，将于下周执行。", false},
        {L"预算会议纪要.docx", L"C:\\Users\\SS\\Documents\\预算会议纪要.docx", L"会议确认季度预算与部门年度计划保持一致。", false}
    };
    state.total = state.rows.size();
    check(Capture(state, (output / L"content-dark.png").wstring()), "Capture content dark");
    state.Key(VK_DOWN); check(state.selected == 1, "Arrow down selects next row");
    state.Key(VK_UP); check(state.selected == 0, "Arrow up selects previous row");
    state.composing = true;
    check(!state.Key(VK_RETURN) && window.Visible(), "IME Enter is not intercepted");
    state.composing = false;
    const auto rows = state.rows;
    window.Show(nullptr, false, 1.5f, output.wstring()); state.Cancel();
    state.scale = 1.5f;
    SetWindowPos(state.hwnd, nullptr, 0, 0, state.Px(780), state.Px(488), SWP_NOMOVE | SWP_NOZORDER);
    state.Layout();
    state.rows = rows; state.total = rows.size(); state.content_mode = true;
    check(Capture(state, (output / L"content-light-150.png").wstring()), "Capture content light 150 percent DPI");
    state.content_mode = false;
    for (int i = 0; i < 14; ++i) state.rows.push_back({L"预算文档 " + std::to_wstring(i) + L".txt", L"D:\\工作\\财务\\预算文档.txt", L"", false});
    state.total = state.rows.size(); state.selected = static_cast<int>(state.rows.size()) - 1; state.ClampSelection();
    check(state.scroll_y > 0 && static_cast<float>(state.selected + 1) * 88 <= state.scroll_y + state.ViewHeight() + 0.1f,
        "Keyboard selection scrolls into viewport");
    check(Capture(state, (output / L"filename-light-150.png").wstring()), "Capture filename scrolling");
    state.content_mode = true; state.selected = 0; state.scroll_y = 0; state.motion_override = 0;
    SendMessageW(state.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    const float scrolled_offset = state.scroll_y;
    check(scrolled_offset > 0, "Mouse wheel scrolls away from selected first result");
    for (int phase = 0; phase < 3; ++phase) {
        pulse::index::ContentSearchUpdate update;
        update.progress.generation = state.generation;
        update.progress.done = phase == 2;
        if (phase == 1) {
            pulse::index::ContentHit hit;
            hit.name = L"appended.txt"; hit.path = L"D:\\appended.txt";
            update.hits.push_back(std::move(hit));
        }
        pulse::index::ContentRefreshTestPeer::Queue(state.contents, std::move(update));
        SendMessageW(state.hwnd, pulse::kContentResult, 0, 0);
        check(state.scroll_y == scrolled_offset && state.selected == 0,
            "Content progress, appended hits and completion preserve mouse scroll");
    }
    state.Key(VK_DOWN);
    check(static_cast<float>(state.selected) * 88 >= state.scroll_y &&
        static_cast<float>(state.selected + 1) * 88 <= state.scroll_y + state.ViewHeight() + 0.1f,
        "Keyboard navigation still reveals selection after mouse scrolling");
    state.motion_override = -1;
    state.rows.clear(); state.total = 0; state.busy = true;
    check(Capture(state, (output / L"loading-light-150.png").wstring()), "Capture loading");
    state.busy = false;
    check(Capture(state, (output / L"no-results-light-150.png").wstring()), "Capture no results");
    state.error = pulse::l10n::Get(pulse::l10n::StringId::GlobalSearchFailed);
    check(Capture(state, (output / L"error-light-150.png").wstring()), "Capture error");
    state.error.clear();
    const auto test_accent = D2D1::ColorF(0xc08040);
    for (const auto effect : {pulse::ui::WindowEffect::None, pulse::ui::WindowEffect::Mica,
        pulse::ui::WindowEffect::MicaAlt, pulse::ui::WindowEffect::Acrylic}) {
        window.SetAppearance(true, effect, L"", test_accent);
        for (int frame = 0; frame < 3; ++frame) { state.Paint(); Pump(30); }
        check(state.target.Get() == state.compositor.Dc() && !state.compositor.NeedsRecovery() &&
            state.effect == effect && state.Theme().accent.r == test_accent.r,
            "Shared compositor renders selected material and accent");
        DWORD actual_backdrop = 0;
        const DWORD expected_backdrop = effect == pulse::ui::WindowEffect::None ? 1 :
            effect == pulse::ui::WindowEffect::Mica ? 2 : effect == pulse::ui::WindowEffect::Acrylic ? 3 : 4;
        check(SUCCEEDED(DwmGetWindowAttribute(state.hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
            &actual_backdrop, sizeof(actual_backdrop))) && actual_backdrop == expected_backdrop,
            "DWM window has the requested system backdrop type");
        check(state.compositor.UsesTransparentComposition(), "Popup composition preserves alpha");
        Microsoft::WRL::ComPtr<ID2D1Image> source;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> source_bitmap, sample;
        auto* dc = state.compositor.Dc();
        dc->GetTarget(&source);
        HRESULT sampled = source.As(&source_bitmap);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(sampled)) sampled = dc->CreateBitmap(D2D1::SizeU(1, 1), nullptr, 0, properties, &sample);
        const D2D1_RECT_U area{15, 300, 16, 301};
        if (SUCCEEDED(sampled)) sampled = sample->CopyFromBitmap(nullptr, source_bitmap.Get(), &area);
        D2D1_MAPPED_RECT pixel{};
        if (SUCCEEDED(sampled)) sampled = sample->Map(D2D1_MAP_OPTIONS_READ, &pixel);
        int alpha = -1;
        if (SUCCEEDED(sampled)) { alpha = pixel.bits[3]; sample->Unmap(); }
        check(effect == pulse::ui::WindowEffect::None ? alpha == 255 : alpha > 0 && alpha < 255,
            "Body stays translucent for DWM material and opaque for None");
    }
    const auto wallpaper = (output / L"empty-dark.png").wstring();
    window.SetAppearance(false, pulse::ui::WindowEffect::MicaAlt, wallpaper, test_accent);
    const auto image_deadline = GetTickCount64() + 5000;
    while (!state.material.SourceBitmap(wallpaper) && GetTickCount64() < image_deadline) Pump(30);
    for (int frame = 0; frame < 3; ++frame) { state.Paint(); Pump(30); }
    check(state.material.SourceBitmap(wallpaper) != nullptr && !state.backdrop,
        "Custom background uses shared async material instead of DWM");
    check(state.compositor.SaveSnapshot((output / L"material-custom.png").c_str()), "Capture shared custom background material");
    window.SetAppearance(false, pulse::ui::WindowEffect::None, wallpaper, test_accent);
    for (int frame = 0; frame < 3; ++frame) { state.Paint(); Pump(30); }
    check(state.compositor.SaveSnapshot((output / L"material-image.png").c_str()), "Capture image background without effect");
    window.SetAppearance(false, pulse::ui::WindowEffect::None, L"", pulse::ui::GetAccentColor());
    state.local_result.hits = {{L"C:\\fixture\\same.txt", L"same.txt", false}};
    state.local_result.total = 1;
    state.network_result.hits = {{L"c:\\FIXTURE\\SAME.txt", L"SAME.txt", false}, {L"\\\\server\\share\\network.txt", L"network.txt", false}};
    state.network_result.total = 2; state.local_ready = state.network_ready = true;
    state.MergeFilenames();
    check(state.rows.size() == 2 && state.total == 2 && !state.busy, "Local and network merge deduplicates Windows paths");
    state.error.clear(); state.content_mode = true; state.current_only = true;
    const auto files = output / L"files";
    std::filesystem::create_directories(files);
    std::ofstream(files / L"needle.txt") << "A real global-search-fixture-needle text document.";
    state.current_folder = files.wstring();
    pulse::index::ContentIndexConfig config; config.roots = {{files.wstring()}};
    state.contents.Resume(); state.contents.Configure(config);
    const auto config_deadline = GetTickCount64() + 5000;
    while (state.contents.GetConfig().roots.empty() && GetTickCount64() < config_deadline) Pump(30);
    SetWindowTextW(state.edit, L"global-search-fixture-needle");
    state.Search();
    const auto query_deadline = GetTickCount64() + 15000;
    while (state.busy && GetTickCount64() < query_deadline) Pump(30);
    check(!state.busy && state.rows.size() == 1 && state.rows[0].name == L"needle.txt" &&
        state.rows[0].snippet.find(L"global-search-fixture-needle") != std::wstring::npos, "Real Instant content query delivers hit and snippet without result store");
    check(Capture(state, (output / L"real-content.png").wstring()), "Capture real content result");
    SetWindowTextW(state.edit, L"pulse-global-search-cancellation-fixture");
    state.Search();
    check(state.contents.CurrentGeneration() == state.generation, "Content query dispatched to independent async client");
    const auto generation = state.generation;
    const auto started = GetTickCount64();
    state.Key(VK_ESCAPE);
    check(!window.Visible() && state.generation != generation && state.contents.CurrentGeneration() == 0, "Escape hides and invalidates/cancels content generation");
    check(GetTickCount64() - started < 250, "Hide does not join search worker");
    check(window.Show(nullptr, true, 1, output.wstring()), "Show resumes after cancellation");
    state.Cancel(); window.Shutdown();
    check(!window.Visible(), "Shutdown releases window");
    shared_lifecycle();
    CoUninitialize();
    return failed ? 1 : 0;
}
