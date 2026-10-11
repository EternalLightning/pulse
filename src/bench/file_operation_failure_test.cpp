#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ops/operation_presentation.h"
#include "../ui/file_operation_dialog.h"
#include <filesystem>
#include <cstdio>
#include <cstring>

namespace pulse::ui {
struct FileOperationFailureProbe {
    static HWND Window(const FileOperationWindow& window) { return window.hwnd_; }
    static bool Detailed(const FileOperationWindow& window) { return window.detailed_; }
    static const ops::OpStatus& Status(const FileOperationWindow& window) { return window.status_; }
    static float Scroll(const FileOperationWindow& window) { return window.failure_scroll_; }
    static float MaxScroll(const FileOperationWindow& window) { return window.failure_scroll_max_; }
    static void Collapse(FileOperationWindow& window) {
        window.detailed_ = false;
        window.ResizeForDetails(false);
    }
    static bool Fits(const FileOperationWindow& window) {
        const auto text = ops::OperationFailureDetails(window.status_);
        const float text_height = window.painter_.MeasureWrappedCaptionHeight(text,
            static_cast<float>(window.compositor_.Width()) - 64 * window.scale_);
        return 216 * window.scale_ + text_height <=
            static_cast<float>(window.compositor_.Height()) - 64 * window.scale_;
    }
    static bool Capture(FileOperationWindow& window, const std::filesystem::path& path) {
        window.Render(false);
        // Capture before Present rotates the swap chain; reject blank frames.
        ComPtr<ID2D1Image> image;
        window.compositor_.Dc()->GetTarget(&image);
        ComPtr<ID2D1Bitmap1> target;
        if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return false;
        const auto size = target->GetPixelSize();
        ComPtr<ID2D1Bitmap1> readable;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            target->GetPixelFormat(), 96, 96);
        if (FAILED(window.compositor_.Dc()->CreateBitmap(size, nullptr, 0, props, &readable)) ||
            FAILED(readable->CopyFromBitmap(nullptr, target.get(), nullptr))) return false;
        D2D1_MAPPED_RECT pixels{};
        if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &pixels))) return false;
        struct Unmap { ID2D1Bitmap1* bitmap; ~Unmap() { bitmap->Unmap(); } } unmap{readable.get()};
        bool has_content = false;
        for (UINT y = 0; y < size.height && !has_content; ++y) {
            const auto* row = pixels.bits + y * pixels.pitch;
            for (UINT x = 0; x < size.width; ++x) {
                if (std::memcmp(row + x * 4, pixels.bits, 4) != 0) { has_content = true; break; }
            }
        }
        if (!has_content) return false;
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic)))) return false;
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
        ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(size.width, size.height))) return false;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        return SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WritePixels(size.height, pixels.pitch, pixels.pitch * size.height, pixels.bits)) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
};
}

int wmain() {
    pulse::compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    using pulse::ui::FileOperationFailureProbe;
    using pulse::ops::OpPhase;
    int failures = 0;
    const auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
        failures += !value;
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseOperationFailureFixture";
    RegisterClassW(&wc);
    const HWND owner = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 960, 720, nullptr, nullptr, wc.hInstance, nullptr);
    const std::filesystem::path output = L"bench_data/operation-failure";
    std::filesystem::create_directories(output);
    {
        pulse::ui::FileOperationWindow window;
        check(window.Create(owner, {}), "real operation dialog initializes");
        const auto hwnd = FileOperationFailureProbe::Window(window);
        window.Show(false);
        pulse::ops::OpStatus status;
        status.task_id = 1; status.type = pulse::ops::OpType::RecycleDelete;
        status.phase = OpPhase::Running; status.active = true; status.total_items = 1;
        status.source_label = L"E:\\Study\\示范项目";
        window.Update(status);
        check(!FileOperationFailureProbe::Detailed(window), "running task starts in summary view");
        for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
            pulse::l10n::SetLanguage(language);
            for (int dpi : {96, 144}) {
                const float scale = static_cast<float>(dpi) / 96;
                RECT rect{0, 0, static_cast<LONG>(460 * scale), static_cast<LONG>(268 * scale)};
                SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
                for (bool dark : {false, true}) {
                    window.SetTheme(dark, pulse::ui::HexColor(0x69CFFF));
                    status.active = false; status.phase = OpPhase::Failed; ++status.task_id;
                    status.source_label.clear(); status.current_item.clear();
                    status.failed_path = L"\\\\?\\E:\\Study\\示范项目";
                    status.failure_hr = static_cast<HRESULT>(0x80270027u); status.last_error.clear();
                    window.Update(status);
                    check(FileOperationFailureProbe::Detailed(window), "failure opens full reason automatically");
                    check(FileOperationFailureProbe::Status(window).source_label == L"E:\\Study\\示范项目" &&
                        FileOperationFailureProbe::Status(window).last_error.find(L"0x80270027") != std::wstring::npos,
                        "screenshot failure has a real path and readable HRESULT explanation");
                    check(FileOperationFailureProbe::Fits(window), "failure details fit above footer");
                    const auto name = std::wstring(language) + (dark ? L"-dark-" : L"-light-") +
                        std::to_wstring(dpi) + L".png";
                    check(FileOperationFailureProbe::Capture(window, output / name), "capture actual failure dialog");
                    FileOperationFailureProbe::Collapse(window);
                    RECT collapsed{}, repeated{};
                    GetWindowRect(hwnd, &collapsed);
                    for (int i = 0; i < 32; ++i) window.Update(status);
                    GetWindowRect(hwnd, &repeated);
                    check(!FileOperationFailureProbe::Detailed(window) && EqualRect(&collapsed, &repeated),
                        "repeated status notifications preserve the user's collapsed view and window size");
                    ++status.task_id;
                    status.failed_path = L"\\\\?\\UNC\\server\\share";
                    for (int i = 0; i < 18; ++i) status.failed_path += L"\\very-long-directory-name";
                    status.failed_path += L"\\文件.txt";
                    window.Update(status);
                    check(FileOperationFailureProbe::Detailed(window) && FileOperationFailureProbe::Fits(window),
                        "long UNC failure path expands fully without overlapping footer");
                    check(FileOperationFailureProbe::Capture(window, output / (L"long-" + name)),
                        "capture wrapped long-path failure");
                    const auto reason_before = FileOperationFailureProbe::Status(window).last_error;
                    SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(600 * scale), static_cast<int>(424 * scale), SWP_NOMOVE | SWP_NOZORDER);
                    SetWindowPos(hwnd, nullptr, 0, 0, static_cast<int>(460 * scale), static_cast<int>(424 * scale), SWP_NOMOVE | SWP_NOZORDER);
                    check(FileOperationFailureProbe::Fits(window) &&
                        FileOperationFailureProbe::Status(window).last_error == reason_before,
                        "narrowing a long failure preserves all explanation text");
                    ++status.task_id;
                    status.failed_path = L"\\\\?\\UNC\\server\\share";
                    for (int i = 0; i < 1000; ++i) status.failed_path += L"\\very-long-directory-name";
                    status.failed_path += L"\\final-item.txt";
                    window.Update(status);
                    check(FileOperationFailureProbe::Capture(window, output / (L"scroll-top-" + name)),
                        "capture screen-limited failure details");
                    RECT actual{};
                    GetWindowRect(hwnd, &actual);
                    MONITORINFO monitor{sizeof(monitor)};
                    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
                    check(actual.top >= monitor.rcWork.top && actual.bottom <= monitor.rcWork.bottom &&
                        FileOperationFailureProbe::MaxScroll(window) > 0,
                        "very long errors keep close button on screen and enable scrolling");
                    SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
                    check(FileOperationFailureProbe::Scroll(window) > 0, "mouse wheel scrolls the full failure reason");
                    SendMessageW(hwnd, WM_KEYDOWN, VK_END, 0);
                    check(FileOperationFailureProbe::Scroll(window) == FileOperationFailureProbe::MaxScroll(window) &&
                        FileOperationFailureProbe::Capture(window, output / (L"scroll-bottom-" + name)),
                        "End reveals the complete final path endpoint without covering footer");
                    SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 0);
                    check(FileOperationFailureProbe::Scroll(window) == 0, "Home returns to failure reason");
                    status.phase = OpPhase::Cancelled; status.failure_hr = S_OK;
                    status.last_error.clear(); status.failed_path.clear();
                    window.Update(status);
                    check(FileOperationFailureProbe::Status(window).last_error.empty(),
                        "cancellation does not inherit the previous failure reason");
                }
            }
        }
    }
    DestroyWindow(owner);
    CoUninitialize();
    std::printf("[RESULT] failures=%d\n", failures);
    return failures ? 1 : 0;
}
