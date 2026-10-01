#include "../common/windows_compat.h"
#include "../ui/ui_renderer_internal.h"
#include <iostream>

namespace {
bool CaptureSnapshot(pulse::ui::Compositor& compositor, std::string_view name) {
    using pulse::ui::ComPtr;
    ComPtr<ID2D1Image> image;
    compositor.Dc()->GetTarget(&image);
    ComPtr<ID2D1Bitmap1> target;
    if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return false;
    const auto size = target->GetPixelSize();
    ComPtr<ID2D1Bitmap1> readable;
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        target->GetPixelFormat(), 96, 96);
    if (FAILED(compositor.Dc()->CreateBitmap(size, nullptr, 0, props, &readable)) ||
        FAILED(readable->CopyFromBitmap(nullptr, target.get(), nullptr))) return false;
    D2D1_MAPPED_RECT pixels{};
    if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &pixels))) return false;
    struct Unmap {
        ID2D1Bitmap1* bitmap;
        ~Unmap() { bitmap->Unmap(); }
    } unmap{readable.get()};
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(size.width, size.height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WritePixels(size.height, pixels.pitch, pixels.pitch * size.height, pixels.bits)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;
    STATSTG stats{};
    HGLOBAL memory = nullptr;
    if (FAILED(stream->Stat(&stats, STATFLAG_NONAME)) ||
        FAILED(GetHGlobalFromStream(stream.get(), &memory))) return false;
    const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
    if (!bytes) return false;
    struct Unlock {
        HGLOBAL memory;
        ~Unlock() { GlobalUnlock(memory); }
    } unlock{memory};
    // The runner persists PNGs; the fixture never requests filesystem writes.
    constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::cout << "[SNAPSHOT] " << name << ' ';
    const auto length = static_cast<size_t>(stats.cbSize.QuadPart);
    for (size_t i = 0; i < length; i += 3) {
        const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
            (i + 1 < length ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < length ? bytes[i + 2] : 0);
        const char encoded[] = {digits[(value >> 18) & 63], digits[(value >> 12) & 63],
            i + 1 < length ? digits[(value >> 6) & 63] : '=', i + 2 < length ? digits[value & 63] : '='};
        std::cout.write(encoded, 4);
    }
    std::cout << '\n';
    return std::cout.good();
}
}

int RunFixture() {
    using namespace pulse;
    std::cout << "[INFO] isolated UI fixture started" << std::endl;
    compat::EnableDpiAwareness();
    std::cout << "[INFO] DPI initialized" << std::endl;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    std::cout << "[INFO] COM and strings initialized" << std::endl;
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++failures; std::cout << "[FAIL] " << label << '\n'; }
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseDuplicateLayoutFixture";
    RegisterClassW(&wc);
    const HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 1280, 900, nullptr, nullptr, wc.hInstance, nullptr);
    std::cout << "[INFO] fixture window created" << std::endl;
    ui::Compositor compositor;
    if (!window || !compositor.Init(window)) {
        std::wcerr << L"[FAIL] isolated compositor: " << compositor.InitializationError() << '\n';
        return 2;
    }
    ui::MainRenderer renderer;
    renderer.SetCompositor(&compositor);
    ui::fluent::Painter painter(&compositor);
    ui::WindowViewModel vm;
    vm.settings_open = true; vm.settings_page = 4; vm.dup_can_scan = true;
    vm.settings_version = L"fixture";
    vm.dup_folder = L"C:\\Fixture\\中文与 emoji \U0001F4C1\\long-directory-name\\copies";
    vm.dup_drives = {{L"C: System", L"C:\\", true},
        {L"D: Very long volume label / 超长卷标 \U0001F4BE for layout", L"D:\\", false},
        {L"E: Data", L"E:\\", false}, {L"F: Archive", L"F:\\", false}};
    vm.dup_groups = {{L"4 MB · 3 copies", {
        {L"季度预算 \U0001F4C4.txt", L"C:\\Fixture\\one.txt", L"C:\\Fixture\\long\\one.txt · 2026/01/02", true},
        {L"second-copy-with-a-long-file-name.txt", L"C:\\Fixture\\two.txt", L"C:\\Fixture\\long\\two.txt · 2026/01/01", false},
        {L"第三份.txt", L"C:\\Fixture\\three.txt", L"C:\\Fixture\\three.txt · 2025/12/31", false}}}};
    vm.dup_groups.push_back(vm.dup_groups.front());
    vm.dup_groups.back().title = L"8 MB · 3 copies";
    const auto result_groups = vm.dup_groups;

    const auto nearly_equal = [](float a, float b) { return std::abs(a - b) < 0.1f; };
    const auto valid = [](D2D1_RECT_F r, D2D1_RECT_F outer) {
        return r.right > r.left && r.bottom > r.top && r.left >= outer.left - 0.1f &&
            r.right <= outer.right + 0.1f && r.top >= outer.top - 0.1f && r.bottom <= outer.bottom + 0.1f;
    };
    for (const wchar_t* language : {L"zh-CN", L"en-US"}) {
        l10n::SetLanguage(language);
        vm.dup_hint = l10n::Get(l10n::StringId::DupHint);
        vm.dup_delete_all = l10n::Get(l10n::StringId::DupDeleteAllExtras) + L" · 1234567890";
        for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
            compositor.RecreateTextFormats(scale); renderer.SetScale(scale); painter.SetScale(scale);
            for (int width : {320, 480, 720, 759, 760, 999, 1000, 1280}) {
                const auto bounds = D2D1::RectF(0, 0, width * scale, 900 * scale);
                compositor.Resize(static_cast<int>(bounds.right), static_cast<int>(bounds.bottom));
                for (bool dark : {false, true}) {
                    vm.dark = dark;
                    const auto theme = ui::MakeTheme(dark, ui::HexColor(0x0078D4));
                    painter.BeginFrame(theme);
                    // Initialize the renderer's own painter before using real hit testing.
                    compositor.Dc()->BeginDraw(); renderer.Render(vm, bounds, theme);
                    check(SUCCEEDED(compositor.Dc()->EndDraw()), "initial renderer frame");
                    for (int scope = 0; scope < 3; ++scope) {
                        vm.dup_scope = scope;
                        for (int state = 0; state < 5; ++state) {
                            vm.settings_scroll = 0;
                            vm.dup_groups = state == 2 || state == 3
                                ? decltype(vm.dup_groups){} : result_groups;
                            vm.dup_scanning = state == 1;
                            vm.dup_show_progress = state == 1;
                            vm.dup_progress_indeterminate = true;
                            vm.dup_status = L"Listing · 128 files";
                            vm.dup_empty = state == 2 ? l10n::Get(l10n::StringId::DupNoResults) :
                                state == 3 ? L"Unable to read the selected location / 无法读取选择的位置" :
                                state == 4 ? l10n::Get(l10n::StringId::DupTruncated) : L"";
                            vm.dup_show_delete_all = state == 0 || state == 4;
                            auto layout = ui::MakeSettingsLayout(vm, bounds, scale, renderer.TitleBarHeight(),
                                28 * scale, &painter);
                            const float right = layout.duplicate_options.right - 16 * scale;
                            check(valid(layout.dup_scope_label, layout.dup_scope_row), "scope label stays within row");
                            check(valid(layout.dup_hint, layout.dup_scope_row), "hint stays within scope row");
                            check(valid(layout.dup_min_description, layout.dup_min_row), "minimum-size description fits");
                            check(nearly_equal(layout.dup_scope[2].right, right), "scope control is right aligned");
                            check(nearly_equal(layout.dup_min_size[2].right, right), "size control is right aligned");
                            check(nearly_equal(layout.dup_scan.right, right), "primary scan is right aligned");
                            check(layout.dup_cancel.bottom <= layout.dup_scan.top ||
                                (nearly_equal(layout.dup_cancel.top, layout.dup_scan.top) &&
                                 layout.dup_cancel.right <= layout.dup_scan.left), "cancel precedes scan without overlap");
                            for (auto r : layout.dup_scope) check(valid(r, layout.dup_scope_row), "scope rectangles fit");
                            for (auto r : layout.dup_min_size) check(valid(r, layout.dup_min_row), "size rectangles fit");
                            check(valid(layout.dup_scan, layout.dup_actions_row), "scan stays within action row");
                            check(valid(layout.dup_cancel, layout.dup_actions_row), "cancel stays within action row");
                            if (scope == 0) check(valid(layout.dup_browse, layout.dup_target_row) &&
                                nearly_equal(layout.dup_browse.right, right), "browse stays within target row at right");
                            for (auto r : layout.dup_drives) check(valid(r, layout.dup_target_row), "long drive chips fit");
                            if (!vm.dup_empty.empty()) check(layout.dup_empty.top >= layout.duplicate_options.bottom &&
                                (!vm.dup_show_progress || layout.dup_empty.top >= layout.dup_progress.bottom),
                                "empty/error/truncated message follows complete action/progress area");
                            const auto hit = [&](D2D1_RECT_F r, ui::HitTestResult::Region region, int index = -1, int sub = -1) {
                                if (r.top < layout.content.top || r.bottom > layout.content.bottom) return;
                                const auto actual = renderer.HitTest(vm, bounds, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
                                check(actual.region == region && (index < 0 || actual.index == index) &&
                                    (sub < 0 || actual.sub_index == sub), "rendered geometry matches real hit mapping");
                            };
                            for (int i = 0; i < 3; ++i) {
                                hit(layout.dup_scope[i], ui::HitTestResult::SettingsDupScope, i);
                                hit(layout.dup_min_size[i], ui::HitTestResult::SettingsDupMinSize, i);
                            }
                            hit(layout.dup_scan, ui::HitTestResult::SettingsDupScan);
                            hit(layout.dup_cancel, ui::HitTestResult::SettingsDupCancel);
                            if (scope == 0) hit(layout.dup_browse, ui::HitTestResult::SettingsDupBrowse);
                            for (size_t i = 0; i < layout.dup_drives.size(); ++i)
                                hit(layout.dup_drives[i], ui::HitTestResult::SettingsDupDrive, static_cast<int>(i));
                            for (size_t i = 0; i < layout.dup_keep.size(); ++i) {
                                check(nearly_equal(layout.dup_keep[i].right, right), "keep radio is right aligned");
                                check(layout.dup_open[i].right <= layout.dup_keep[i].left ||
                                    layout.dup_open[i].bottom <= layout.dup_keep[i].top, "filename and keep do not overlap");
                                hit(layout.dup_keep[i], ui::HitTestResult::SettingsDupKeep, layout.dup_keep_group[i], layout.dup_keep_file[i]);
                                hit(layout.dup_open[i], ui::HitTestResult::SettingsDupOpen, layout.dup_open_group[i], layout.dup_open_file[i]);
                            }
                            const float extent = layout.content_h;
                            vm.settings_scroll = renderer.SettingsMaxScroll(vm, bounds.right, bounds.bottom);
                            const auto scrolled = ui::MakeSettingsLayout(vm, bounds, scale, renderer.TitleBarHeight(), 28 * scale, &painter);
                            check(nearly_equal(extent, scrolled.content_h), "scroll does not change content extent");
                            for (size_t i = 0; i < scrolled.dup_keep.size(); ++i) {
                                hit(scrolled.dup_keep[i], ui::HitTestResult::SettingsDupKeep,
                                    scrolled.dup_keep_group[i], scrolled.dup_keep_file[i]);
                                hit(scrolled.dup_open[i], ui::HitTestResult::SettingsDupOpen,
                                    scrolled.dup_open_group[i], scrolled.dup_open_file[i]);
                            }
                            for (size_t g = 0; g < scrolled.dup_group_delete.size(); ++g)
                                hit(scrolled.dup_group_delete[g], ui::HitTestResult::SettingsDupGroupDelete, static_cast<int>(g));
                            if (vm.dup_show_delete_all) {
                                check(nearly_equal(scrolled.dup_delete_all.right, right), "dynamic delete-all is right aligned");
                                check(scrolled.dup_delete_all.bottom <= scrolled.content.bottom, "last action is reachable at max scroll");
                                hit(scrolled.dup_delete_all, ui::HitTestResult::SettingsDupDeleteAll);
                            }
                            vm.settings_scroll = 0;
                            compositor.Dc()->BeginDraw(); renderer.Render(vm, bounds, theme);
                            check(SUCCEEDED(compositor.Dc()->EndDraw()), "actual duplicate settings renderer completes");
                            if (scale == 1.0f && state == 0 && (width == 320 || width == 720 || width == 1280)) {
                                const std::string language_name = language[0] == L'z' ? "zh-CN" : "en-US";
                                const auto name = language_name + "-" + std::to_string(width) +
                                    "-scope" + std::to_string(scope) + (dark ? "-dark.png" : "-light.png");
                                check(CaptureSnapshot(compositor, name), "duplicate page native PNG snapshot captured");
                                vm.settings_scroll = renderer.SettingsMaxScroll(vm, bounds.right, bounds.bottom);
                                compositor.Dc()->BeginDraw(); renderer.Render(vm, bounds, theme);
                                check(SUCCEEDED(compositor.Dc()->EndDraw()), "scrolled duplicate renderer completes");
                                const auto tail_name = language_name + "-" + std::to_string(width) +
                                    "-scope" + std::to_string(scope) + (dark ? "-dark-bottom.png" : "-light-bottom.png");
                                check(CaptureSnapshot(compositor, tail_name), "scrolled result actions native PNG snapshot captured");
                                vm.settings_scroll = 0;
                            }
                        }
                    }
                }
            }
        }
    }
    check(vm.dup_groups[0].files[0].keep && !vm.dup_groups[0].files[1].keep, "layout preserves keeper state");
    // Release painter resources before shutting down the compositor.
    painter.SetCompositor(nullptr);
    renderer.SetCompositor(nullptr); compositor.Shutdown(); DestroyWindow(window);
    CoUninitialize();
    std::cout << (failures ? "[FAIL] " : "[PASS] ") << checks << " scoped layout/render/hit checks, "
        << failures << " failures; no scan/delete/prefs backend started\n";
    return failures ? 1 : 0;
}

int main() {
    try { return RunFixture(); }
    catch (const std::exception& error) {
        std::cerr << "[FAIL] fixture exception: " << error.what() << std::endl;
        return 3;
    }
    catch (...) {
        std::cerr << "[FAIL] fixture unknown exception" << std::endl;
        return 4;
    }
}
