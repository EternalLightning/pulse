// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ops/operation_presentation.h"
#include "../app/archive_operation_ui.h"
#include "../app/exit_confirmation.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/animation_clock.h"
#include "snapshot_output.h"
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace pulse::ui {
struct FileOperationFailureProbe {
    struct Pixels {
        UINT width = 0;
        UINT height = 0;
        UINT stride = 0;
        std::vector<BYTE> bytes;
    };
    static HWND Window(const FileOperationWindow& window) { return window.hwnd_; }
    static bool Detailed(const FileOperationWindow& window) { return window.detailed_; }
    static const ops::OpStatus& Status(const FileOperationWindow& window) { return window.status_; }
    static float Scroll(const FileOperationWindow& window) { return window.failure_scroll_; }
    static float MaxScroll(const FileOperationWindow& window) { return window.failure_scroll_max_; }
    static bool ScrollPending(const FileOperationWindow& window) { return window.failure_scroll_animation_.Pending(); }
    static const FileOperationProgressInspection& Inspect(FileOperationWindow& window) {
        window.Render(false);
        return window.progress_inspection_;
    }
    static void Click(FileOperationWindow& window, const D2D1_RECT_F& bounds) {
        const LPARAM point = MAKELPARAM(static_cast<WORD>((bounds.left + bounds.right) / 2),
            static_cast<WORD>((bounds.top + bounds.bottom) / 2));
        SendMessageW(window.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(window.hwnd_, WM_LBUTTONUP, 0, point);
    }
    static void SampleSpeed(FileOperationWindow& window) {
        window.speed_sample_tick_ = 0;
        SendMessageW(window.hwnd_, WM_TIMER, 1, 0);
    }
    static size_t SpeedSamples(const FileOperationWindow& window) { return window.speed_history_.size(); }
    static bool AnimationsEnabled(const FileOperationWindow& window) { return window.animations_enabled_; }
    static bool NeedsFrame(const FileOperationWindow& window) { return window.NeedsAnimationFrame(); }
    static float CompletionOpacity(const FileOperationWindow& window) { return window.completion_reveal_.opacity; }
    static bool CompletionPending(const FileOperationWindow& window) { return window.completion_reveal_.pending; }
    static void SetAnimationPolicy(FileOperationWindow& window, bool enabled) { window.animations_enabled_ = enabled; }
    static void HoldCompletionStart(FileOperationWindow& window) {
        window.completion_reveal_.Reset();
        window.completion_reveal_.Tick(true, AnimationTimeMs() + 1000, true);
    }
    static void FinishCompletion(FileOperationWindow& window) {
        window.completion_reveal_.Sample(AnimationTimeMs() + 1200, true);
    }
    static void StartCompletionHalfway(FileOperationWindow& window) {
        window.completion_reveal_.Reset();
        window.completion_reveal_.Tick(true, AnimationTimeMs() - 80, true);
    }
    static void SetZigzag(FileOperationWindow& window) {
        window.speed_history_.clear();
        for (int i = 0; i < 120; ++i) window.speed_history_.push_back(i % 2 ? 0.0 : 100.0);
        window.detailed_ = true;
        window.ResizeForDetails(false);
    }
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
    static bool Capture(FileOperationWindow& window, const std::filesystem::path& path,
                        Pixels* captured = nullptr) {
        if (captured) *captured = {};
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
        if (captured) {
            captured->width = size.width;
            captured->height = size.height;
            captured->stride = pixels.pitch;
            captured->bytes.assign(pixels.bits, pixels.bits + static_cast<size_t>(pixels.pitch) * size.height);
        }
        if (bench::StreamSnapshots())
            return bench::EmitSnapshot(path, size.width, size.height, pixels.pitch, pixels.bits);
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic)))) return false;
        ComPtr<IWICStream> stream;
        if (FAILED(wic->CreateStream(&stream))) return false;
        const auto absolute_path = std::filesystem::absolute(path);
        const HRESULT stream_hr = stream->InitializeFromFilename(absolute_path.c_str(), GENERIC_WRITE);
        if (FAILED(stream_hr)) {
            std::printf("[DIAG] snapshot file open failed: 0x%08lX\n", static_cast<unsigned long>(stream_hr));
            return false;
        }
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

static size_t PumpScrollMessages(HWND hwnd, ULONGLONG duration_ms) {
    const ULONGLONG start = GetTickCount64();
    size_t scroll_timers = 0;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.hwnd == hwnd && message.message == WM_TIMER && message.wParam == 2)
                ++scroll_timers;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() - start < duration_ms);
    return scroll_timers;
}

static int RunScrollTests(HWND owner) {
    using namespace pulse;
    using ui::FileOperationFailureProbe;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    ui::FileOperationWindow window;
    check(window.Create(owner, {}), "scroll fixture creates the actual failure window");
    const HWND hwnd = FileOperationFailureProbe::Window(window);
    if (!hwnd) return failures + 1;
    window.Show(false);
    FileOperationFailureProbe::SetAnimationPolicy(window, true);
    ops::OpStatus status;
    status.task_id = 1; status.type = ops::OpType::Copy;
    status.phase = ops::OpPhase::Failed; status.failure_hr = E_ACCESSDENIED;
    status.failed_path = L"\\\\?\\UNC\\server\\share";
    for (int i = 0; i < 1000; ++i) status.failed_path += L"\\long-directory";
    window.Update(status);
    FileOperationFailureProbe::Inspect(window);
    const float maximum = FileOperationFailureProbe::MaxScroll(window);
    check(maximum > 100, "failure fixture has a scrollable clipped text viewport");
    const auto wheel = [&](int direction) {
        SendMessageW(hwnd, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(direction * WHEEL_DELTA)), 0);
    };
    wheel(-1);
    check(FileOperationFailureProbe::ScrollPending(window) &&
        FileOperationFailureProbe::Scroll(window) < maximum,
        "wheel begins motion without jumping to the end of the text");
    check(PumpScrollMessages(hwnd, 45) > 0 && FileOperationFailureProbe::Scroll(window) > 0 &&
        FileOperationFailureProbe::ScrollPending(window),
        "actual scroll timer presents an intermediate pixel offset");
    const float middle = FileOperationFailureProbe::Scroll(window);
    PumpScrollMessages(hwnd, 750);
    check(FileOperationFailureProbe::Scroll(window) > middle &&
        !FileOperationFailureProbe::ScrollPending(window),
        "wheel motion advances from the middle and settles");
    check(PumpScrollMessages(hwnd, 80) == 0, "settled wheel motion stops its timer");
    SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 0);
    wheel(-1);
    PumpScrollMessages(hwnd, 45);
    const float before_reverse = FileOperationFailureProbe::Scroll(window);
    wheel(1);
    PumpScrollMessages(hwnd, 45);
    check(FileOperationFailureProbe::Scroll(window) < before_reverse,
        "reversing the wheel changes direction on the next frames");
    PumpScrollMessages(hwnd, 750);
    check(FileOperationFailureProbe::Scroll(window) == 0 && !FileOperationFailureProbe::ScrollPending(window),
        "reverse motion stops at the upper boundary");
    wheel(1);
    check(!FileOperationFailureProbe::ScrollPending(window) && PumpScrollMessages(hwnd, 60) == 0,
        "wheel input at the boundary does not start an idle timer");
    FileOperationFailureProbe::SetAnimationPolicy(window, false);
    wheel(-1);
    const float direct_offset = FileOperationFailureProbe::Scroll(window);
    check(direct_offset > 0 && !FileOperationFailureProbe::ScrollPending(window) &&
        PumpScrollMessages(hwnd, 60) == 0,
        "disabled animation applies the wheel endpoint without scheduling frames");
    FileOperationFailureProbe::SetAnimationPolicy(window, true);
    SendMessageW(hwnd, WM_KEYDOWN, VK_HOME, 0);
    wheel(-1);
    PumpScrollMessages(hwnd, 750);
    check(std::abs(FileOperationFailureProbe::Scroll(window) - direct_offset) < 0.6f,
        "animated and direct wheel input reach the same endpoint");
    wheel(-1);
    ++status.task_id;
    window.Update(status);
    check(FileOperationFailureProbe::Scroll(window) == 0 && !FileOperationFailureProbe::ScrollPending(window) &&
        PumpScrollMessages(hwnd, 60) == 0,
        "new task clears the offset and cancels pending frames");
    window.Destroy();
    return failures;
}

static int RunResourceTests(HWND owner) {
    using namespace pulse;
    using ui::FileOperationFailureProbe;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    ui::FileOperationWindow window;
    check(window.Create(owner, {}), "resource fixture creates the actual progress window");
    const HWND hwnd = FileOperationFailureProbe::Window(window);
    if (!hwnd) return failures + 1;
    window.Show(false);
    FileOperationFailureProbe::SetAnimationPolicy(window, false);
    ops::OpStatus status;
    status.task_id = 1; status.type = ops::OpType::Copy; status.percent = 50;
    status.current_item = L"fixture.bin";
    for (const auto phase : {ops::OpPhase::Completed, ops::OpPhase::Failed,
        ops::OpPhase::Cancelled, ops::OpPhase::Paused, ops::OpPhase::WaitingForConflict,
        ops::OpPhase::WaitingForDeleteConfirmation}) {
        status.phase = phase;
        status.active = phase == ops::OpPhase::Paused || phase == ops::OpPhase::WaitingForConflict ||
            phase == ops::OpPhase::WaitingForDeleteConfirmation;
        window.Update(status);
        SendMessageW(hwnd, WM_PAINT, 0, 0);
        ValidateRect(hwnd, nullptr);
        const auto samples = FileOperationFailureProbe::SpeedSamples(window);
        FileOperationFailureProbe::SampleSpeed(window);
        check(!GetUpdateRect(hwnd, nullptr, FALSE) &&
            FileOperationFailureProbe::SpeedSamples(window) == samples,
            "settled, paused and waiting phases neither repaint nor append idle speed samples");
        for (int i = 0; i < 20; ++i) window.Update(status);
        check(!GetUpdateRect(hwnd, nullptr, FALSE),
            "unchanged maintenance status notifications preserve a settled window");
        status.current_item += L"x";
        window.Update(status);
        check(GetUpdateRect(hwnd, nullptr, FALSE) != FALSE,
            "changed operation content still requests a repaint");
    }
    status.phase = ops::OpPhase::Running; status.active = true;
    window.Update(status);
    FileOperationFailureProbe::Collapse(window);
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    ValidateRect(hwnd, nullptr);
    const auto samples = FileOperationFailureProbe::SpeedSamples(window);
    FileOperationFailureProbe::SampleSpeed(window);
    check(FileOperationFailureProbe::SpeedSamples(window) == samples + 1 &&
        !GetUpdateRect(hwnd, nullptr, FALSE),
        "active transfer records statistics without repainting a collapsed unchanged view");
    FileOperationFailureProbe::SetZigzag(window);
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    ValidateRect(hwnd, nullptr);
    FileOperationFailureProbe::SampleSpeed(window);
    check(GetUpdateRect(hwnd, nullptr, FALSE) != FALSE,
        "an expanded active speed graph repaints when a sample is recorded");
    window.Hide();
    ValidateRect(hwnd, nullptr);
    FileOperationFailureProbe::SampleSpeed(window);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "hidden transfer timer does not repaint");
    return failures;
}

static int RunCompletionTests(HWND owner) {
    using namespace pulse;
    using ui::FileOperationFailureProbe;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    const std::filesystem::path output = L"bench_data/operation-completion";
    std::filesystem::create_directories(output);
    int dismisses = 0, cancels = 0;
    ui::FileOperationWindow window;
    check(window.Create(owner, {[&] { ++cancels; }, {}, {}, [&] { ++dismisses; }}),
        "completion fixture creates the actual progress window");
    const HWND hwnd = FileOperationFailureProbe::Window(window);
    if (!hwnd) return failures + 1;
    window.Show(false);
    const bool system_animations = FileOperationFailureProbe::AnimationsEnabled(window);
    ops::OpStatus status;
    status.task_id = 1; status.type = ops::OpType::Copy;
    status.source_label = L"fixture.bin"; status.destination_label = L"output";
    status.total_items = status.completed_items = 4;
    status.phase = ops::OpPhase::Completed; status.percent = 100;
    window.Update(status);
    auto inspection = FileOperationFailureProbe::Inspect(window);
    check(inspection.percent == L"100%" && inspection.time == l10n::Get(l10n::StringId::OpCompleted) &&
        inspection.speed == L"0 B/s" && !inspection.show_pause,
        "completion preserves final progress, stable statistics and existing controls");
    if (system_animations) {
        FileOperationFailureProbe::HoldCompletionStart(window);
        SendMessageW(hwnd, WM_PAINT, 0, 0);
        check(FileOperationFailureProbe::CompletionOpacity(window) == 0 &&
            FileOperationFailureProbe::NeedsFrame(window) && GetUpdateRect(hwnd, nullptr, FALSE),
            "visible completion starts transparent and queues another presented frame");
        FileOperationFailureProbe::StartCompletionHalfway(window);
        window.Update(status);
        FileOperationFailureProbe::Inspect(window);
        check(FileOperationFailureProbe::CompletionOpacity(window) >= 0.875f,
            "repeated completion status does not restart its reveal");
        FileOperationFailureProbe::FinishCompletion(window);
        SendMessageW(hwnd, WM_PAINT, 0, 0);
        check(FileOperationFailureProbe::CompletionOpacity(window) == 1 &&
            !FileOperationFailureProbe::CompletionPending(window) &&
            !FileOperationFailureProbe::NeedsFrame(window) && !GetUpdateRect(hwnd, nullptr, FALSE),
            "completion settles fully and stops the continuous animation loop");
        ++status.task_id;
        window.Update(status);
        const ULONGLONG started = GetTickCount64();
        size_t reveal_frames = 0;
        while (GetTickCount64() - started < 220) {
            MSG message{};
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { Sleep(1); continue; }
            if (message.hwnd == hwnd && message.message == WM_PAINT) ++reveal_frames;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        check(reveal_frames >= 2 && FileOperationFailureProbe::CompletionOpacity(window) == 1 &&
            !FileOperationFailureProbe::CompletionPending(window) && !FileOperationFailureProbe::NeedsFrame(window),
            "the real completion paint loop advances and settles within the short reveal interval");
        for (const int dpi : {96, 168}) {
            const float scale = dpi / 96.0f;
            RECT rect{0, 0, static_cast<LONG>(460 * scale), static_cast<LONG>(268 * scale)};
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
            for (const bool dark : {false, true}) {
                window.SetTheme(dark, ui::HexColor(0x69CFFF));
                const auto name = std::wstring(dark ? L"dark-" : L"light-") + std::to_wstring(dpi) + L".png";
                FileOperationFailureProbe::HoldCompletionStart(window);
                check(FileOperationFailureProbe::Capture(window, output / (L"start-" + name)) &&
                    FileOperationFailureProbe::CompletionOpacity(window) == 0,
                    "capture completed content at the start of its reveal");
                FileOperationFailureProbe::FinishCompletion(window);
                check(FileOperationFailureProbe::Capture(window, output / (L"settled-" + name)) &&
                    FileOperationFailureProbe::CompletionOpacity(window) == 1,
                    "capture completed content fully visible at the same layout");
            }
        }
        FileOperationFailureProbe::HoldCompletionStart(window);
        window.Hide();
        SendMessageW(hwnd, WM_PAINT, 0, 0);
        check(!FileOperationFailureProbe::CompletionPending(window) &&
            !FileOperationFailureProbe::NeedsFrame(window) && !GetUpdateRect(hwnd, nullptr, FALSE),
            "hiding settles the completion reveal and stops frame scheduling");
        window.Show(false);
        FileOperationFailureProbe::Inspect(window);
        check(FileOperationFailureProbe::CompletionOpacity(window) == 1 &&
            !FileOperationFailureProbe::CompletionPending(window),
            "showing the same completed task preserves its settled content");
        FileOperationFailureProbe::HoldCompletionStart(window);
        FileOperationFailureProbe::SetAnimationPolicy(window, false);
        FileOperationFailureProbe::Inspect(window);
        check(FileOperationFailureProbe::CompletionOpacity(window) == 1 &&
            !FileOperationFailureProbe::CompletionPending(window) && !FileOperationFailureProbe::NeedsFrame(window),
            "disabling animation during completion shows final content immediately");
    } else {
        check(FileOperationFailureProbe::CompletionOpacity(window) == 1 &&
            !FileOperationFailureProbe::CompletionPending(window) && !FileOperationFailureProbe::NeedsFrame(window),
            "system reduced motion or high contrast shows completion immediately");
    }
    ++status.task_id; status.phase = ops::OpPhase::Running; status.active = true; status.percent = 20;
    window.Update(status);
    check(FileOperationFailureProbe::CompletionOpacity(window) == 0 &&
        !FileOperationFailureProbe::CompletionPending(window),
        "a new task clears completed content animation state");
    SendMessageW(hwnd, WM_SETTINGCHANGE, SPI_SETCLIENTAREAANIMATION, 0);
    check(FileOperationFailureProbe::AnimationsEnabled(window) == system_animations,
        "settings notification refreshes the cached system motion policy");
    status.phase = ops::OpPhase::Completed; status.active = false; status.percent = 100;
    window.Update(status);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    check(dismisses == 1 && cancels == 0 && !window.IsVisible(),
        "completed close retains dismissal behavior without cancelling a task");
    return failures;
}

static bool ZigzagPeaksConnected(const pulse::ui::FileOperationFailureProbe::Pixels& pixels, float scale) {
    if (pixels.stride < static_cast<size_t>(pixels.width) * 4 ||
        pixels.bytes.size() < static_cast<size_t>(pixels.stride) * pixels.height) return false;
    // The outside of each sharp peak must be covered by the joined cyan stroke.
    // Separate butt-ended segments leave this row above the vertex unpainted.
    const UINT y = static_cast<UINT>(std::floor(214.0f * scale - 0.5f * scale));
    for (UINT i = 2; i < 118; i += 2) {
        const UINT x = static_cast<UINT>((30.0f + 400.0f * i / 119.0f) * scale);
        if (x >= pixels.width || y >= pixels.height) return false;
        const auto* pixel = pixels.bytes.data() + static_cast<size_t>(y) * pixels.stride + x * 4;
        if (static_cast<int>(pixel[0]) - pixel[2] < 50 ||
            static_cast<int>(pixel[1]) - pixel[2] < 30) return false;
    }
    return true;
}

static int RunMotionTests(HWND owner) {
    using namespace pulse;
    using ui::FileOperationFailureProbe;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    const std::filesystem::path output = L"bench_data/operation-motion";
    std::filesystem::create_directories(output);
    ui::FileOperationWindow window;
    check(window.Create(owner, {}), "motion fixture creates the actual progress window");
    const HWND hwnd = FileOperationFailureProbe::Window(window);
    if (!hwnd) return failures + 1;
    check(!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MINIMIZEBOX),
        "operation popup has no redundant minimize control");
    window.Show(false);
    ops::OpStatus status;
    status.task_id = 1; status.type = ops::OpType::Copy; status.active = true;
    status.phase = ops::OpPhase::Scanning; status.percent = -1;
    window.Update(status);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (animations && !ui::IsHighContrast()) {
        SendMessageW(hwnd, WM_PAINT, 0, 0);
        check(GetUpdateRect(hwnd, nullptr, FALSE) != FALSE,
            "preparation queues the next presented frame without waiting for a timer");
        std::vector<double> frames;
        size_t timer_messages = 0;
        const auto start = std::chrono::steady_clock::now();
        const auto deadline = start + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { Sleep(1); continue; }
            if (message.hwnd == hwnd && message.message == WM_PAINT)
                frames.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count());
            if (message.hwnd == hwnd && message.message == WM_TIMER) ++timer_messages;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        std::vector<double> gaps;
        for (size_t i = 1; i < frames.size(); ++i) gaps.push_back(frames[i] - frames[i - 1]);
        std::sort(gaps.begin(), gaps.end());
        std::printf("[PERF] preparation: frames=%zu fps=%.1f median_gap_ms=%.2f p95_gap_ms=%.2f timers=%zu\n",
            frames.size(), frames.size() / 1.5, gaps.empty() ? 0 : gaps[gaps.size() / 2],
            gaps.empty() ? 0 : gaps[gaps.size() * 95 / 100], timer_messages);
        check(frames.size() >= 2, "preparation produces successive display-paced frames");
        check(timer_messages > 0, "continuous preparation does not starve timer delivery");
    } else std::printf("[INFO] system motion policy disables animated frame scheduling\n");
    status.phase = ops::OpPhase::Running; status.percent = 50;
    window.Update(status);
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "known progress stops the continuous animation loop");
    for (const int dpi : {96, 168}) {
        const float scale = dpi / 96.0f;
        RECT rect{0, 0, static_cast<LONG>(460 * scale), static_cast<LONG>(424 * scale)};
        SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
        for (const bool dark : {false, true}) {
            window.SetTheme(dark, ui::HexColor(0x69CFFF));
            FileOperationFailureProbe::SetZigzag(window);
            const auto image = output / (std::wstring(L"rate-zigzag-") + (dark ? L"dark-" : L"light-") +
                std::to_wstring(dpi) + L".png");
            FileOperationFailureProbe::Pixels pixels;
            check(FileOperationFailureProbe::Capture(window, image, &pixels) && ZigzagPeaksConnected(pixels, scale),
                "dense sharp rate peaks remain connected in the actual rendered image");
        }
    }
    status.phase = ops::OpPhase::Scanning; status.percent = -1;
    window.Update(status); window.Hide();
    SendMessageW(hwnd, WM_PAINT, 0, 0);
    check(!GetUpdateRect(hwnd, nullptr, FALSE), "a hidden progress window does not continue the animation loop");
    return failures;
}

static int PreviewExitConfirmation(HWND owner) {
    using namespace pulse;
    static std::wstring snapshot;
    static bool captured = false;
    const auto output = std::filesystem::absolute(L"bench_data/operation-progress/exit-confirmation.png");
    std::filesystem::create_directories(output.parent_path());
    snapshot = output.wstring();
    captured = false;
    const auto timer = SetTimer(nullptr, 0, 250, [](HWND, UINT, UINT_PTR id, DWORD) {
        EnumThreadWindows(GetCurrentThreadId(), [](HWND dialog, LPARAM parameter) -> BOOL {
            wchar_t name[64]{}; GetClassNameW(dialog, name, ARRAYSIZE(name));
            if (std::wcscmp(name, L"PulseConfirmWindow") || !IsWindowVisible(dialog)) return TRUE;
            KillTimer(nullptr, static_cast<UINT_PTR>(parameter));
            captured = SendMessageW(dialog, ui::kConfirmSnapshotMessage, 0,
                reinterpret_cast<LPARAM>(snapshot.c_str())) != 0;
            SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
            return FALSE;
        }, static_cast<LPARAM>(id));
    });
    if (!timer) return 1;
    const bool accepted = ui::ShowConfirmDialog(owner, app::ExitConfirmationSpec(), true, ui::HexColor(0x69CFFF));
    KillTimer(nullptr, timer);
    std::printf("[%s] exit confirmation preview captured; Enter keeps tasks running\n",
        captured && !accepted ? "PASS" : "FAIL");
    return captured && !accepted ? 0 : 1;
}

static int RunProgressTests(HWND owner) {
    using namespace pulse;
    using ui::FileOperationFailureProbe;
    using ops::OpPhase;
    int failures = 0;
    const auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
        failures += !value;
    };
    const std::filesystem::path output = L"bench_data/operation-progress";
    std::filesystem::create_directories(output);
    for (const auto type : {ops::OpType::Copy, ops::OpType::Move}) {
        int pauses = 0, resumes = 0;
        ui::FileOperationWindow window;
        check(window.Create(owner, {{}, [&] { ++pauses; }, [&] { ++resumes; }, {}}),
            "default transfer dialog initializes");
        if (!FileOperationFailureProbe::Window(window)) continue;
        window.Show(false);
        ops::OpStatus status;
        status.task_id = 1; status.type = type; status.phase = OpPhase::Running; status.active = true;
        status.source_label = L"fixture.bin"; status.destination_label = L"output";
        status.total_items = 4; status.completed_items = 1; status.percent = 25;
        window.Update(status);
        auto inspection = FileOperationFailureProbe::Inspect(window);
        check(inspection.show_pause && inspection.remaining_label == l10n::Get(l10n::StringId::OpRemainingItems),
            "default copy/move retain pause and remaining item count");
        check(FileOperationFailureProbe::Capture(window, output /
            (type == ops::OpType::Copy ? L"copy-running-zh-CN.png" : L"move-running-zh-CN.png")),
            "capture running transfer status badge");
        FileOperationFailureProbe::Click(window, inspection.pause);
        status.phase = OpPhase::Paused; window.Update(status);
        inspection = FileOperationFailureProbe::Inspect(window);
        check(FileOperationFailureProbe::Capture(window, output /
            (type == ops::OpType::Copy ? L"copy-paused-zh-CN.png" : L"move-paused-zh-CN.png")),
            "capture default paused transfer with existing controls");
        FileOperationFailureProbe::Click(window, inspection.pause);
        check(pauses == 1 && resumes == 1, "real copy/move buttons invoke pause and resume");
    }
    for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        for (int dpi : {96, 168}) {
            for (bool dark : {false, true}) {
                int cancels = 0, dismisses = 0;
                ui::FileOperationWindow window;
                check(window.Create(owner, {[&] { ++cancels; }, {}, {}, [&] { ++dismisses; }},
                    {true, true, false}), "external progress reuses actual transfer dialog");
                const auto hwnd = FileOperationFailureProbe::Window(window);
                if (!hwnd) continue;
                const float scale = static_cast<float>(dpi) / 96;
                RECT rect{0, 0, static_cast<LONG>(460 * scale), static_cast<LONG>(268 * scale)};
                SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
                window.SetTheme(dark, ui::HexColor(0x69CFFF));
                window.Show(false);
                const auto name = std::wstring(language) + (dark ? L"-dark-" : L"-light-")
                    + std::to_wstring(dpi) + L".png";
                ops::OpStatus status;
                status.task_id = 10; status.phase = OpPhase::Running; status.active = true;
                status.summary = l10n::Get(l10n::StringId::ArchiveExtracting);
                status.source_label = L"project-package.zip"; status.destination_label = L"E:\\output\\project";
                status.percent = 42; status.total_bytes = 100 * 1024 * 1024;
                status.transferred_bytes = 42 * 1024 * 1024;
                status.bytes_per_second = 4 * 1024 * 1024;
                status.peak_bytes_per_second = 6 * 1024 * 1024; status.eta_seconds = 14;
                window.Update(status);
                auto inspection = FileOperationFailureProbe::Inspect(window);
                wchar_t native_title[256]{};
                GetWindowTextW(hwnd, native_title, 256);
                check(inspection.title == status.summary && inspection.title == native_title &&
                    inspection.percent == L"42%" && !inspection.show_pause &&
                    inspection.file_line == status.source_label &&
                    inspection.subtitle == l10n::Get(l10n::StringId::OpFromPrefix) + status.source_label
                        + L" → " + status.destination_label,
                    "external title, source and percent have no copy or current-file claims");
                check(inspection.remaining_label == l10n::Get(l10n::StringId::OpRemainingSize) &&
                    inspection.remaining == l10n::Get(l10n::StringId::OpApprox)
                        + format::ByteSize(status.total_bytes - status.transferred_bytes) &&
                    inspection.speed.find(l10n::Get(l10n::StringId::OpApprox)) == 0 &&
                    inspection.details.right + 8 * scale <= inspection.cancel.left,
                    "unknown item count uses estimated bytes and controls fit 460 DIP");
                for (int i = 0; i < 16; ++i) {
                    status.bytes_per_second = (2 + i % 5) * 1024 * 1024;
                    window.Update(status);
                    FileOperationFailureProbe::SampleSpeed(window);
                }
                FileOperationFailureProbe::Click(window, inspection.details);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(FileOperationFailureProbe::Detailed(window) &&
                    FileOperationFailureProbe::SpeedSamples(window) >= 16 &&
                    inspection.peak.find(l10n::Get(l10n::StringId::OpApprox)) != std::wstring::npos &&
                    FileOperationFailureProbe::Capture(window, output / (L"running-" + name)),
                    "details show actual nonempty rate history and estimated peak");
                status.phase = OpPhase::Scanning; status.percent = -1; status.total_bytes = 0;
                window.Update(status);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(inspection.indeterminate && inspection.percent.empty() &&
                    inspection.remaining == l10n::Get(l10n::StringId::OpCalculating) &&
                    FileOperationFailureProbe::Capture(window, output / (L"scanning-" + name)),
                    "unknown scanning progress animates existing indeterminate bar");
                status.phase = OpPhase::Running; status.percent = 42;
                status.total_bytes = 100 * 1024 * 1024; window.Update(status);
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
                check(cancels == 0 && dismisses == 1 && !window.IsVisible() &&
                    FileOperationFailureProbe::Status(window).active,
                    "closing active progress hides it without cancelling the task");
                window.Show(false);
                inspection = FileOperationFailureProbe::Inspect(window);
                FileOperationFailureProbe::Click(window, inspection.cancel);
                FileOperationFailureProbe::Click(window, inspection.cancel);
                status.phase = OpPhase::Cancelling;
                status.summary = l10n::Get(l10n::StringId::ArchiveCancelling); window.Update(status);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(cancels == 1 && dismisses == 1 && window.IsVisible() &&
                    inspection.speed == L"0 B/s" && inspection.time == l10n::Get(l10n::StringId::OpNoEstimate) &&
                    FileOperationFailureProbe::Capture(window, output / (L"cancelling-" + name)),
                    "explicit cancel waits visibly, ignores repeated clicks and stops active estimates");
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
                check(cancels == 1 && dismisses == 2 && !window.IsVisible(),
                    "closing during cancellation can hide the progress window");
                window.Show(false);
                SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
                check(cancels == 1 && dismisses == 3 && !window.IsVisible(),
                    "Escape dismisses without sending another cancellation");
                status.phase = OpPhase::Cancelled; status.active = false;
                window.Update(status);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(inspection.speed == L"0 B/s" && inspection.time == l10n::Get(l10n::StringId::Cancel),
                    "cancelled progress has no active time or rate estimate");
                status.task_id = 11; status.phase = OpPhase::Failed;
                status.summary = l10n::Get(l10n::StringId::ArchiveExtractFailed);
                status.total_bytes = status.transferred_bytes = 0;
                status.failed_path = L"E:\\output\\project\\readme.txt";
                status.failure_hr = E_ACCESSDENIED;
                window.Update(status);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(FileOperationFailureProbe::Detailed(window) && FileOperationFailureProbe::Fits(window) &&
                    inspection.remaining == L"—" &&
                    !FileOperationFailureProbe::Status(window).last_error.empty() &&
                    FileOperationFailureProbe::Capture(window, output / (L"failed-" + name)),
                    "external failure uses existing readable failure details");
                ops::ArchiveOperationResult archive_failure;
                archive_failure.exit_code = 2;
                archive_failure.password_state = ops::ArchivePasswordState::Required;
                archive_failure.error = L"ERROR: Data Error in encrypted file. Wrong password? : 童欣文.pptx";
                const auto previous_status = status;
                status = app::ArchiveFailureStatus(archive_failure, L"D:\\test\\加密压缩包.zip", L"E:\\output");
                status.task_id = 13;
                window.Update(status);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(FileOperationFailureProbe::Detailed(window) && FileOperationFailureProbe::Fits(window) &&
                    ops::OperationFailureDetails(FileOperationFailureProbe::Status(window)) ==
                        l10n::Get(l10n::StringId::ArchivePasswordRetryHint) + L"\n\n" + archive_failure.error &&
                    inspection.file_line.starts_with(l10n::Get(l10n::StringId::ArchivePasswordRetryHint)) &&
                    inspection.title == l10n::Get(l10n::StringId::ArchivePasswordOrDataError) &&
                    FileOperationFailureProbe::Capture(window, output / (L"archive-password-failed-" + name)),
                    "archive password diagnostics use the shared failure layout without losing their member name");
                status = previous_status;
                status.task_id = 12; status.phase = OpPhase::Completed; status.failure_hr = S_OK;
                status.failed_path.clear(); status.last_error.clear();
                status.summary = l10n::Get(l10n::StringId::ArchiveExtractComplete);
                status.transferred_bytes = status.total_bytes;
                window.Update(status);
                window.Show(false);
                inspection = FileOperationFailureProbe::Inspect(window);
                check(inspection.percent == L"100%" && inspection.speed == L"0 B/s" &&
                    FileOperationFailureProbe::Capture(window, output / (L"completed-" + name)),
                    "completed extraction reaches 100 percent in shared window");
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
                check(dismisses == 4 && !window.IsVisible(), "terminal close dismisses the shared dialog");
            }
        }
    }
    return failures;
}

int wmain(int argc, wchar_t** argv) {
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
    if (argc == 2 && std::wcscmp(argv[1], L"--exit-preview") == 0) {
        failures = PreviewExitConfirmation(owner);
        DestroyWindow(owner);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--motion-only") == 0) {
        failures = RunMotionTests(owner);
        DestroyWindow(owner);
        CoUninitialize();
        std::printf("[RESULT] failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--scroll-only") == 0) {
        failures = RunScrollTests(owner);
        DestroyWindow(owner);
        CoUninitialize();
        std::printf("[RESULT] failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--resource-only") == 0) {
        failures = RunResourceTests(owner);
        DestroyWindow(owner);
        CoUninitialize();
        std::printf("[RESULT] failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--completion-only") == 0) {
        failures = RunCompletionTests(owner);
        DestroyWindow(owner);
        CoUninitialize();
        std::printf("[RESULT] failures=%d\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--progress-only") == 0) {
        try {
            failures = RunProgressTests(owner);
        } catch (const std::filesystem::filesystem_error& exception) {
            std::printf("[FAIL] operation progress fixture: %s\n", exception.what());
            failures = 1;
        }
        DestroyWindow(owner);
        CoUninitialize();
        std::printf("[RESULT] failures=%d\n", failures);
        return failures ? 1 : 0;
    }
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
                    PumpScrollMessages(hwnd, 45);
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
