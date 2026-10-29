#include "../app/app_internal.h"
#include "../common/localization.h"
#include "../ui/edit_host.h"
#include "../ui/typography.h"
#include <iostream>
#include <fstream>
#include <dwmapi.h>
#include <thread>
#include <cstdlib>

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace pulse {
void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
    int left_margin_dip, int right_margin_dip);
}

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
}
std::vector<std::uint32_t> Pixels(HWND hwnd, bool desktop = false) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    POINT origin{};
    if (desktop) { ClientToScreen(hwnd, &origin); DwmFlush(); }
    HDC source = GetDC(desktop ? nullptr : hwnd), target = CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::vector<std::uint32_t> pixels;
    if (source && target && bitmap) {
        HGDIOBJ old = SelectObject(target, bitmap);
        if (BitBlt(target, 0, 0, rect.right, rect.bottom, source, origin.x, origin.y,
            SRCCOPY | (desktop ? CAPTUREBLT : 0))) {
            GdiFlush();
            const auto* start = static_cast<const std::uint32_t*>(bits);
            pixels.assign(start, start + rect.right * rect.bottom);
        }
        SelectObject(target, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (target) DeleteDC(target);
    if (source) ReleaseDC(desktop ? nullptr : hwnd, source);
    return pixels;
}
size_t DifferentPixels(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b,
    unsigned channel_tolerance = 0) {
    if (a.empty() || a.size() != b.size()) return SIZE_MAX;
    size_t count = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        bool different = false;
        for (unsigned shift : {0u, 8u, 16u}) {
            const int before = (a[i] >> shift) & 0xff;
            const int after = (b[i] >> shift) & 0xff;
            different |= std::abs(before - after) > static_cast<int>(channel_tolerance);
        }
        count += different;
    }
    return count;
}
void SavePixels(const std::vector<std::uint32_t>& pixels, int width, int height, const std::string& filename) {
    if (pixels.empty()) return;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(pixels[0]));
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = -height;
    info.biPlanes = 1;
    info.biBitCount = 32;
    const auto mode = pulse::ui::typography::CurrentTextRenderMode();
    const std::string mode_name = mode == pulse::ui::typography::TextRenderMode::Sharp ? "sharp-" :
        mode == pulse::ui::typography::TextRenderMode::Smooth ? "smooth-" : "auto-";
    std::ofstream output("bench_data/edit-surface-20261024/" + mode_name + filename, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(pixels[0])));
}
void SaveSurface(HWND hwnd, const std::string& filename, bool desktop = false) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    SavePixels(Pixels(hwnd, desktop), rect.right, rect.bottom, filename);
}
struct Messages {
    int positions = 0, changing = 0, paints = 0, erases = 0, size = 0, redraw = 0, hidden = 0, selections = 0;
    bool capture_key = false;
    std::vector<std::uint32_t> key_pixels;
};
LRESULT CALLBACK ParentColors(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_CTLCOLOREDIT || msg == WM_ERASEBKGND || msg == WM_PAINT || msg == WM_COMMAND)
        return WndProcImpl(hwnd, msg, wp, lp);
    return DefSubclassProc(hwnd, msg, wp, lp);
}
LRESULT CALLBACK Observe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& seen = *reinterpret_cast<Messages*>(data);
    if (msg == WM_WINDOWPOSCHANGED) ++seen.positions;
    if (msg == WM_WINDOWPOSCHANGING) ++seen.changing;
    if (msg == WM_PAINT) ++seen.paints;
    if (msg == WM_ERASEBKGND) ++seen.erases;
    if (msg == WM_SIZE) ++seen.size;
    if (msg == WM_SETREDRAW) ++seen.redraw;
    if (msg == EM_SETSEL) ++seen.selections;
    const bool visible = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
    const auto result = DefSubclassProc(hwnd, msg, wp, lp);
    if (seen.capture_key && msg == WM_KEYDOWN) seen.key_pixels = Pixels(hwnd);
    if (visible && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE)) ++seen.hidden;
    return result;
}
void Pump(bool scripted_mouse = false) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        // This loop sends its captured drag points synchronously. Windows also
        // queues a move for the real cursor when capture changes; that unrelated
        // point can be outside this isolated field and alter the test selection.
        if (scripted_mouse && message.message == WM_MOUSEMOVE && GetCapture() == message.hwnd) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    std::cout << std::unitbuf;
    bool chinese = false, desktop = false;
    std::string kind_filter, text_mode = "auto";
    int scale_filter = 0, theme_filter = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        chinese |= argument == "--zh";
        desktop |= argument == "--desktop-smoke";
        if (argument == "--sharp") {
            ui::typography::SetTextRenderMode(ui::typography::TextRenderMode::Sharp);
            text_mode = "sharp";
        } else if (argument == "--smooth") {
            ui::typography::SetTextRenderMode(ui::typography::TextRenderMode::Smooth);
            text_mode = "smooth";
        } else if (argument.starts_with("--kind=")) kind_filter = argument.substr(7);
        else if (argument.starts_with("--scale=")) scale_filter = std::atoi(argument.c_str() + 8);
        else if (argument == "--light") theme_filter = 0;
        else if (argument == "--dark") theme_filter = 1;
    }
    const char* names[] = {"address-path", "address-search", "filter", "rename", "tag-rename"};
    if ((!kind_filter.empty() && std::none_of(std::begin(names), std::end(names),
        [&](const char* name) { return kind_filter == name; })) ||
        (scale_filter != 0 && scale_filter != 100 && scale_filter != 150 && scale_filter != 200)) {
        std::cout << "[ERROR] expected --kind=address-path|address-search|filter|rename|tag-rename and --scale=100|150|200\n";
        return 2;
    }
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), chinese ? L"zh-CN" : L"en-US");
    const HWND foreground = GetForegroundWindow();
    for (const bool dark : {false, true}) for (const float scale : {1.0f, 1.5f, 2.0f}) {
        if (desktop && scale != 1.0f) continue;
        if (theme_filter >= 0 && dark != (theme_filter != 0)) continue;
        if (scale_filter && scale_filter != static_cast<int>(std::lround(scale * 100.0f))) continue;
        auto owned = std::make_unique<AppState>();
        auto& s = *owned;
        s.isolatedTest = true;
        s.appPrefs.persist = s.places.persist = s.ctxMenuPrefs.persist = s.searchHistory.persist = false;
        s.darkMode = dark; s.scale = scale;
        s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"isolated edit frame fixture",
            WS_POPUP | WS_VISIBLE, desktop ? 40 : -30000, desktop ? 40 : -30000,
            static_cast<int>(1100 * scale), static_cast<int>(720 * scale), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(s.hwnd && s.compositor.Init(s.hwnd), "isolated edit frame compositor initializes");
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        SetWindowSubclass(s.hwnd, ParentColors, 98, 0);
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetCompositor(&s.compositor); s.renderer.SetScale(scale);
        Check(s.compositor.CustomEditEnabled(), "stability fixture uses production custom editing in the selected text mode");
        s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
        s.addressIgnoreKillFocus = s.filterIgnoreKillFocus = s.renameIgnoreKillFocus = s.tagRenameIgnoreKillFocus = true;
        const std::wstring text = L"C:\\long-path\\" + std::wstring(160, L'W') + L"\\example.txt";
        const SUBCLASSPROC procedures[] = {AddressEditProc, AddressEditProc, FilterEditProc, RenameEditProc, TagRenameEditProc};
        for (size_t kind = 0; kind < std::size(procedures); ++kind) {
            if (desktop && kind != 0 && kind != 3) continue;
            if (!kind_filter.empty() && kind_filter != names[kind]) continue;
            HWND edit = CreateHostedEdit(s, procedures[kind]);
            Check(edit != nullptr, "create production hosted editor");
            if (!edit) continue;
            if (kind < 2) {
                s.hwndAddressEdit = edit; s.addressEditing = true; s.addressSearching = kind == 1;
                s.addressSearchAnimation = 1;
            } else if (kind == 2) {
                s.hwndFilterEdit = edit; s.filterEditing = true; s.pane->filter_expand = 1;
            } else if (kind == 3) s.hwndRenameEdit = edit;
            else s.hwndTagRenameEdit = edit;
            SetWindowTextW(edit, text.c_str());
            auto layout = [&] {
                if (kind < 2) LayoutAddressEditor(s);
                else if (kind == 2) LayoutFilterEditor(s);
                else PlaceHostedEdit(edit, s.hwnd, D2D1::RectF(25, 120, 245 * scale, 150 * scale), scale, 4, 4);
            };
            layout(); ShowWindow(edit, SW_SHOWNOACTIVATE); SetFocus(edit);
            {
                HDC dc = GetDC(edit);
                HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)));
                wchar_t face[LF_FACESIZE]{};
                GetTextFaceW(dc, ARRAYSIZE(face), face);
                std::wcout << L"[FONT] actual=" << face << L" scale=" << scale << L" chinese=" << chinese << L'\n';
                SelectObject(dc, old);
                ReleaseDC(edit, dc);
            }
            SendMessageW(edit, EM_SETSEL, 4, 12); Pump();
            Messages seen;
            RemoveWindowSubclass(edit, procedures[kind], 1);
            SetWindowSubclass(edit, Observe, 99, reinterpret_cast<DWORD_PTR>(&seen));
            SetWindowSubclass(edit, procedures[kind], 1, reinterpret_cast<DWORD_PTR>(&s));
            for (int frame = 0; frame < 40; ++frame) { Render(s); layout(); Pump(); }
            Check(seen.hidden == 0 && IsWindowVisible(edit), "expansion frames preserve the presented child surface");
            seen = {};
            for (int frame = 0; frame < 10; ++frame) { Render(s); layout(); Pump(); }
            DWORD first = 0, last = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
            std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark << " scale=" << scale
                << " idle_positions=" << seen.positions << " idle_size=" << seen.size
                << " idle_redraw=" << seen.redraw << " idle_hidden=" << seen.hidden
                << " idle_changing=" << seen.changing << " idle_paints=" << seen.paints
                << " idle_erases=" << seen.erases << '\n';
            Check(seen.positions == 0 && seen.size == 0 && seen.redraw == 0 && seen.hidden == 0,
                "unchanged parent frames never relayout or hide a hosted edit");
            Check(first == 4 && last == 12 && GetFocus() == edit && IsWindowVisible(edit),
                "unchanged parent frames preserve text selection focus and visibility");
            RECT cell{}; GetClientRect(edit, &cell);
            const int middle = cell.right / 2;
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(middle, cell.bottom / 2));
            seen = {};
            for (int frame = 0; frame < 20; ++frame) {
                SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON,
                    MAKELPARAM(middle + (frame % 2 ? -40 : 40), cell.bottom / 2));
                Render(s); layout(); Pump();
            }
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
            std::cout << "[METRIC] kind=" << names[kind] << " scale=" << scale << " drag_width=" << cell.right
                << " drag_first=" << first << " drag_last=" << last << " capture=" << (GetCapture() == edit) << '\n';
            Check(seen.positions == 0 && seen.size == 0 && seen.redraw == 0 && seen.hidden == 0,
                "drag selection plus parent frames never churn the child surface");
            Check(seen.selections == 0 && GetCapture() == edit && IsWindowVisible(edit),
                "drag presents the selection through parent frames without native selection writes");
            SendMessageW(edit, WM_LBUTTONUP, 0, MAKELPARAM(middle - 40, cell.bottom / 2));
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
            Check(last > first && seen.selections == 1 && GetCapture() != edit && IsWindowVisible(edit),
                "release commits the dragged range exactly once and keeps the field visible");
            SendMessageW(edit, EM_SETSEL, text.size(), text.size());
            SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(middle, cell.bottom / 2));
            SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(1, cell.bottom / 2));
            const auto held_pixels = Pixels(edit);
            SendMessageW(edit, WM_LBUTTONUP, 0, MAKELPARAM(1, cell.bottom / 2));
            const auto released_pixels = Pixels(edit);
            const auto released_difference = DifferentPixels(held_pixels, released_pixels);
            std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark << " scale=" << scale
                << " release_pixels=" << released_difference << '\n';
            Check(released_difference <= static_cast<size_t>(cell.bottom * 4),
                "releasing an edge selection changes only caret pixels, not the text origin");
            seen = {};
            RECT current{}; GetWindowRect(edit, &current);
            MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&current), 2);
            PlaceHostedEdit(edit, s.hwnd,
                D2D1::RectF(static_cast<float>(current.left), static_cast<float>(current.top),
                    static_cast<float>(current.right + 25), static_cast<float>(current.bottom)), scale, 4, 4);
            Check(seen.positions == 1 && IsWindowVisible(edit), "real geometry change still positions the field once");
            Check(IsWindowVisible(edit), "geometry update restores the presented child surface after native redraw suppression");
            seen = {};
            SendMessageW(edit, EM_SETSEL, 0, 3);
            SendMessageW(edit, WM_CHAR, L'Q', 0);
            Check(IsWindowVisible(edit), "keyboard and programmatic selection restore the presented child surface after native redraw suppression");
            SetWindowTextW(edit, L"C:\\Pulse\\File-name-123.txt");
            SendMessageW(edit, EM_SETSEL, 0, -1);
            RedrawWindow(edit, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            seen.capture_key = true;
            const auto before_key = Pixels(edit);
            Check(!before_key.empty() && std::count_if(before_key.begin(), before_key.end(), [&](auto pixel) {
                return (pixel & 0xffffff) != (before_key.front() & 0xffffff);
            }) > 30, "surface sampling captures actual text pixels");
            SendMessageW(edit, WM_KEYDOWN, VK_LEFT, 0);
            const auto after_key = Pixels(edit);
            const auto from_before = DifferentPixels(seen.key_pixels, before_key);
            const auto from_after = DifferentPixels(seen.key_pixels, after_key);
            std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark << " scale=" << scale
                << " native_key_pixels_before=" << from_before << " native_key_pixels_after=" << from_after << '\n';
            Check(from_before <= 64 || from_after <= 64,
                "key processing never publishes a third rendering between the old and final text surface");
            seen.capture_key = false;
            const std::wstring short_text = L"Pulse_中文_123.txt";
            SetWindowTextW(edit, short_text.c_str());
            SendMessageW(edit, EM_SETSEL, 3, 3);
            Render(s); layout(); Pump();
            GetClientRect(edit, &cell);
            RedrawWindow(edit, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            const auto key_origin = Pixels(edit);
            const std::string sample_name = std::string(chinese ? "zh-" : "") + names[kind] + (dark ? "-dark-" : "-light-") +
                std::to_string(static_cast<int>(scale * 100));
            SaveSurface(edit, sample_name + "-key-origin.bmp");
            bool stable_key_pixels = !key_origin.empty();
            size_t max_key_difference = 0;
            for (int frame = 0; frame < 16; ++frame) {
                SendMessageW(edit, WM_KEYDOWN, frame % 2 ? VK_LEFT : VK_RIGHT, 0);
                Render(s); layout(); Pump();
                // Layered GDI captures can round antialiased channels by one
                // level. Ignore that quantization, but retain caret/glyph motion.
                const auto key_pixels = Pixels(edit);
                const auto difference = DifferentPixels(key_origin, key_pixels, 1);
                max_key_difference = std::max(max_key_difference, difference);
                const bool failed_frame = difference > static_cast<size_t>(cell.bottom * 4);
                stable_key_pixels &= !failed_frame;
                if (frame < 2 || failed_frame) {
                    POINT caret{}; GetCaretPos(&caret);
                    DWORD selection_begin = 0, selection_end = 0;
                    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_begin), reinterpret_cast<LPARAM>(&selection_end));
                    std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark << " scale=" << scale
                        << " key_frame=" << frame << " difference=" << difference << " width=" << cell.right
                        << " height=" << cell.bottom << " caret_x=" << caret.x << " caret_y=" << caret.y
                        << " selection=" << selection_begin << ',' << selection_end
                        << " focus=" << (GetFocus() == edit) << " capture=" << (GetCapture() == edit)
                        << " tick=" << GetTickCount() << " blink=" << GetCaretBlinkTime() << '\n';
                    SavePixels(key_pixels, cell.right, cell.bottom,
                        sample_name + "-key-" + std::to_string(frame) + ".bmp");
                    if (failed_frame) {
                        const auto failure_name = text_mode + '-' + sample_name + "-failure-key-" + std::to_string(frame);
                        SavePixels(key_pixels, cell.right, cell.bottom, failure_name + ".bmp");
                        SavePixels(key_origin, cell.right, cell.bottom, failure_name + "-origin.bmp");
                        std::cout << "[ARTIFACT] bench_data/edit-surface-20261024/" << failure_name << ".bmp\n";
                    }
                }
            }
            std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark << " scale=" << scale
                << " max_arrow_changed_pixels=" << max_key_difference
                << " redraw_messages=" << seen.redraw << " redraw_visibility_changes=" << seen.hidden << '\n';
            Check(stable_key_pixels, "arrow movement through short text preserves glyph pixels across parent frames");
            if (desktop) {
                SetWindowPos(s.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                Render(s); layout(); Pump();
                const auto presented = Pixels(edit, true);
                const auto surface = Pixels(edit);
                const auto presentation_difference = DifferentPixels(presented, surface, 1);
                std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark
                    << " desktop_surface_pixels=" << presentation_difference << '\n';
                Check(presentation_difference <= static_cast<size_t>(cell.bottom * 4),
                    "actual desktop shows the presented editor surface after parent composition");
                SaveSurface(edit, sample_name + "-desktop.bmp", true);
                const auto desktop_origin = Pixels(edit, true);
                SendMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
                Render(s); layout(); Pump();
                Check(DifferentPixels(desktop_origin, Pixels(edit, true), 1) <= static_cast<size_t>(cell.bottom * 4),
                    "desktop arrow movement changes only caret pixels");
                for (const bool drag : {false, true}) {
                    const auto point_at = [&](WPARAM index) {
                        return MAKELPARAM(LOWORD(SendMessageW(edit, EM_POSFROMCHAR, index, 0)), cell.bottom / 2);
                    };
                    const auto left_point = point_at(2), right_point = point_at(8);
                    const auto move = [&](int frame) {
                        if (drag) SendMessageW(edit, WM_MOUSEMOVE, MK_LBUTTON, frame % 2 ? left_point : right_point);
                        else SendMessageW(edit, WM_KEYDOWN, frame % 2 ? VK_LEFT : VK_RIGHT, 0);
                    };
                    if (drag) {
                        SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, point_at(4));
                        move(0);
                    }
                    const auto right_frame = Pixels(edit, true);
                    move(1);
                    const auto left_frame = Pixels(edit, true);
                    std::atomic_bool stop_capture{false};
                    int samples = 0, unexpected = 0;
                    size_t max_frame_difference = 0;
                    std::vector<std::uint32_t> worst_frame;
                    std::thread sampler([&] {
                        while (!stop_capture.load()) {
                            auto frame = Pixels(edit, true);
                            const auto difference = std::min(DifferentPixels(frame, left_frame, 1),
                                DifferentPixels(frame, right_frame, 1));
                            ++samples;
                            unexpected += difference > static_cast<size_t>(cell.bottom * 4);
                            if (difference > max_frame_difference) {
                                max_frame_difference = difference;
                                worst_frame = std::move(frame);
                            }
                        }
                    });
                    for (int frame = 0; frame < 48; ++frame) {
                        move(frame);
                        InvalidateRect(s.hwnd, nullptr, FALSE);
                        Pump(drag);
                    }
                    stop_capture.store(true);
                    sampler.join();
                    std::cout << "[METRIC] kind=" << names[kind] << " dark=" << dark << " drag=" << drag << " desktop_samples=" << samples
                        << " unexpected_frames=" << unexpected << " max_frame_difference=" << max_frame_difference << '\n';
                    SavePixels(worst_frame, cell.right, cell.bottom, sample_name + (drag ? "-drag-worst.bmp" : "-desktop-worst.bmp"));
                    Check(samples >= 10 && unexpected == 0,
                        drag ? "independent desktop sampling sees no third drag-selection frame during parent paints"
                             : "independent desktop sampling sees no third text frame during queued parent paints");
                    if (drag) SendMessageW(edit, WM_LBUTTONUP, 0, left_point);
                }
            }
            SendMessageW(edit, EM_SETSEL, 3, 3);
            SendMessageW(edit, WM_CHAR, L'中', 0);
            wchar_t typed[128]{};
            GetWindowTextW(edit, typed, ARRAYSIZE(typed));
            Check(typed == short_text.substr(0, 3) + L"中" + short_text.substr(3),
                "native Unicode insertion preserves text on both sides of the caret");
            SendMessageW(edit, WM_UNDO, 0, 0);
            GetWindowTextW(edit, typed, ARRAYSIZE(typed));
            Check(typed == short_text, "native undo restores text after Unicode insertion");
            if (kind == 0 || kind == 3)
                SaveSurface(edit, std::string(names[kind]) + (dark ? "-dark-" : "-light-") +
                    std::to_string(static_cast<int>(scale * 100)) + ".bmp");
            BYTE saved_keys[256]{}, keys[256]{};
            const bool have_keys = GetKeyboardState(saved_keys) != FALSE;
            keys[VK_SHIFT] = 0x80;
            bool left_active = false, right_active = false, tab_consumed = false;
            if (have_keys && SetKeyboardState(keys)) {
                SetWindowTextW(edit, text.c_str());
                SendMessageW(edit, EM_SETSEL, text.size(), text.size());
                SendMessageW(edit, WM_KEYDOWN, VK_HOME, 0);
                POINT caret{};
                SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
                left_active = first == 0 && last == text.size() && GetCaretPos(&caret) && caret.x >= 0 && caret.x < cell.right;
                SendMessageW(edit, WM_KEYDOWN, VK_END, 0);
                SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
                right_active = first == text.size() && last == text.size() && GetCaretPos(&caret) && caret.x >= 0 && caret.x < cell.right;
                keys[VK_CONTROL] = 0x80;
                SetKeyboardState(keys);
                SendMessageW(edit, WM_CHAR, VK_TAB, 0);
                tab_consumed = HandleHostedEditShortcut(s, WM_CHAR, VK_TAB, 0) &&
                    GetWindowTextLengthW(edit) == static_cast<int>(text.size());
                SetKeyboardState(saved_keys);
            }
            Check(left_active && right_active, "Shift+Home and Shift+End keep the real active caret visible in long text");
            Check(tab_consumed, "custom presentation preserves the hosted Ctrl+Shift+Tab character shortcut");
            RemoveWindowSubclass(edit, Observe, 99);
            DestroyWindow(edit);
            s.hwndAddressEdit = s.hwndFilterEdit = s.hwndRenameEdit = s.hwndTagRenameEdit = nullptr;
            s.addressEditing = s.addressSearching = s.filterEditing = false;
        }
        s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    }
    const HWND final_foreground = GetForegroundWindow();
    DWORD foreground_pid = 0;
    if (final_foreground) GetWindowThreadProcessId(final_foreground, &foreground_pid);
    std::cout << "[INFO] initial_foreground=" << foreground << " final_foreground=" << final_foreground
        << " final_foreground_pid=" << foreground_pid << '\n';
    Check(foreground_pid != GetCurrentProcessId(), "offscreen fixture leaves no test window in the foreground");
    OleUninitialize();
    std::cout << "[SUMMARY] " << checks - failures << " PASS / " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
