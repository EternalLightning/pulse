// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "global_search_window.h"
#include "display_alias_search.h"
#include "../index/live_filename_search.h"
#include "resource.h"
#include "../ui/empty_state_layout.h"
#include "../ui/fluent_components.h"
#include "../ui/typography.h"
#include "../ui/svg_bitmap.h"
#include "../ui/edit_host.h"
#include "../ui/smooth_scroll.h"
#include "../ui/animation_clock.h"
#include "../common/path_utils.h"
#include <uxtheme.h>
#include "../common/localization.h"
#include "../index/index_client.h"
#include "../index/content_search_client.h"
#include "../index/network_agent_client.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "shcore.lib")

namespace pulse {
namespace {
using Microsoft::WRL::ComPtr;
using l10n::StringId;
using ui::typography::FontRole;
constexpr UINT kIndexStatus = WM_APP + 181, kIndexResult = WM_APP + 182, kContentResult = WM_APP + 183;
constexpr UINT kNetworkResult = WM_APP + 184;
constexpr UINT kDisplayAliasResult = WM_APP + 185;
constexpr UINT_PTR kDebounce = 1;
constexpr UINT_PTR kConnectTimeout = 2;
constexpr UINT_PTR kScopeTooltipTimer = 3;
constexpr UINT_PTR kScrollTimer = 4;
constexpr uint64_t kSession = 0x50554c534547534full;
constexpr float kHeader = 76, kTabs = 52, kFooter = 48, kRow = 88;
constexpr float kEditCornerRadius = 4;
constexpr float kEditLeft = 116, kEditRight = 72, kEditTextInset = 8;
constexpr size_t kMaximumResults = 200;
const std::wstring& Text(StringId id) { return l10n::Get(id); }
std::wstring ParentPath(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash == 2 ? 3 : slash);
}
}

struct GlobalSearchWindow::Impl {
    std::function<void(const std::wstring&, bool)> open_callback;
    HWND hwnd = nullptr, edit = nullptr;
    HFONT edit_font = nullptr;
    ComPtr<IDWriteTextFormat> edit_format;
    HBRUSH background = nullptr;
    bool dark = true, content_mode = false, current_only = false, composing = false, busy = false, truncated = false, search_pinyin = true;
    float scale = 1, width = 780, height = 488;
    std::wstring current_folder, current_folder_tip, query, error;
    struct Row { std::wstring name, path, snippet; bool directory = false; };
    std::vector<Row> rows;
    enum class Focus { Edit, Name, Content, Everywhere, Current, Results };
    Focus focus = Focus::Edit;
    int selected = 0;
    float scroll_y = 0;
    ui::SmoothScroll scroll_motion;
    int motion_override = -1; // Isolated fixture override; production follows Windows.
    uint32_t generation = 1;
    size_t total = 0;
    std::unique_ptr<index::IndexClient> owned_filenames;
    std::unique_ptr<index::NetworkAgentClient> owned_network;
    index::IndexClient* filenames = nullptr;
    index::NetworkAgentClient* network = nullptr;
    uint32_t* next_request = nullptr;
    index::SearchResult local_result, network_result;
    app::WorkerPool* display_alias_worker = nullptr;
    model::DisplayAliasSnapshot display_aliases;
    std::shared_ptr<app::DisplayAliasSearchMailbox> display_alias_mailbox;
    std::shared_ptr<const app::DisplayAliasSearchRequest> display_alias_request;
    std::vector<index::SupplementalFilenameHit> display_alias_hits;
    bool local_ready = false, network_ready = false;
    index::ContentSearchClient contents;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> write_factory;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> logo;
    ID2D1RenderTarget* logo_target = nullptr;
    ComPtr<ID2D1Bitmap> empty_art;
    ID2D1RenderTarget* empty_art_target = nullptr;
    float empty_art_scale = 0;
    ui::Compositor compositor;
    ui::fluent::Painter painter;
    ui::WindowMaterial material;
    ui::WindowEffect effect = ui::WindowEffect::None;
    std::wstring background_image;
    D2D1_COLOR_F accent_color = ui::GetAccentColor();
    bool backdrop = false;
    bool scope_hover = false, scope_tooltip_visible = false, tracking_mouse = false;
    float scope_pointer_x = 0, scope_pointer_y = 0;

    ui::Theme Theme() const { return ui::IsHighContrast() ? ui::MakeHighContrastTheme() : ui::MakeTheme(dark, accent_color); }
    D2D1_COLOR_F EditBackground() const {
        return Theme().header_bg;
    }
    static UINT32 Rgb(D2D1_COLOR_F color) {
        return (static_cast<UINT32>(std::lround(color.r * 255)) << 16) |
            (static_cast<UINT32>(std::lround(color.g * 255)) << 8) | static_cast<UINT32>(std::lround(color.b * 255));
    }
    void ApplyAppearance() {
        if (!hwnd) return;
        material.Invalidate();
        backdrop = ui::ApplyWindowEffect(hwnd, background_image.empty() ? effect : ui::WindowEffect::None, dark);
        if (background) DeleteObject(background);
        background = CreateSolidBrush(Background());
        Invalidate();
        if (edit) InvalidateRect(edit, nullptr, TRUE);
    }

    int Px(float dip) const { return static_cast<int>(std::lround(dip * scale)); }
    COLORREF Background() const {
        const auto color = Theme().header_bg;
        return RGB(static_cast<BYTE>(color.r * 255), static_cast<BYTE>(color.g * 255), static_cast<BYTE>(color.b * 255));
    }
    D2D1_COLOR_F Color(UINT32 value) const { return D2D1::ColorF(value); }
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    int PageSize() const { return std::max(1, static_cast<int>((height - kHeader - kTabs - kFooter) / kRow)); }
    float ViewHeight() const { return std::max(1.0f, height - kFooter - 136); }
    float MaxScroll() const { return std::max(0.0f, static_cast<float>(rows.size()) * kRow - ViewHeight()); }
    int ResultRowAt(float x, float y) const {
        if (x < 0 || x >= width || y < 136 || y >= height - kFooter) return -1;
        const int row = static_cast<int>((y - 136 + scroll_y) / kRow);
        return row >= 0 && row < static_cast<int>(rows.size()) ? row : -1;
    }
    bool AnimationsEnabled() const {
        if (motion_override >= 0) return motion_override != 0;
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        return enabled && !ui::IsHighContrast();
    }
    void StopScroll() {
        scroll_motion.Reset();
        if (hwnd) KillTimer(hwnd, kScrollTimer);
        scroll_y = std::clamp(scroll_y, 0.0f, MaxScroll());
    }
    void Scroll(float delta) {
        scroll_motion.ScrollBy(scroll_y, delta, MaxScroll(), ViewHeight(), ui::AnimationTimeMs(), AnimationsEnabled());
        if (scroll_motion.Pending() && !SetTimer(hwnd, kScrollTimer, USER_TIMER_MINIMUM, nullptr)) {
            scroll_motion.Tick(scroll_y, MaxScroll(), ui::AnimationTimeMs(), false);
        }
        if (!scroll_motion.Pending()) KillTimer(hwnd, kScrollTimer);
        Invalidate();
    }
    void TickScroll(uint64_t now) {
        if (scroll_motion.Tick(scroll_y, MaxScroll(), now, AnimationsEnabled())) Invalidate();
        if (!scroll_motion.Pending()) KillTimer(hwnd, kScrollTimer);
    }
    void ClampSelection(bool reveal_selection = true) {
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(rows.size()) - 1));
        StopScroll();
        if (reveal_selection) {
            const float top = static_cast<float>(selected) * kRow;
            if (top < scroll_y) scroll_y = top;
            if (top + kRow > scroll_y + ViewHeight()) scroll_y = top + kRow - ViewHeight();
        }
        scroll_y = std::clamp(scroll_y, 0.0f, MaxScroll());
    }
    void Cancel() {
        StopScroll();
        generation = next_request ? ++*next_request : generation + 1;
        if (filenames) filenames->CancelSession(kSession);
        if (network) network->CancelSession(kSession);
        display_alias_request.reset();
        // Content has its own Instant agent: suspension must never stop main-window queries.
        contents.Suspend();
        busy = false;
        if (hwnd) { KillTimer(hwnd, kDebounce); KillTimer(hwnd, kConnectTimeout); }
    }
    void Hide() {
        Cancel();
        HideScopeTooltip();
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
    }
    void Changed() {
        Cancel();
        const int count = GetWindowTextLengthW(edit);
        query.resize(static_cast<size_t>(count) + 1);
        GetWindowTextW(edit, query.data(), count + 1);
        query.resize(static_cast<size_t>(count));
        rows.clear(); selected = 0; scroll_y = 0; total = 0; error.clear(); truncated = false;
        local_result = {}; network_result = {}; local_ready = network_ready = false;
        display_alias_hits.clear();
        if (!query.empty() && !composing) { busy = true; SetTimer(hwnd, kDebounce, 180, nullptr); }
        Invalidate();
    }
    void Search() {
        KillTimer(hwnd, kDebounce);
        if (!IsWindowVisible(hwnd) || composing || query.empty()) return;
        if (content_mode) {
            index::ContentSearchRequest request;
            request.session_id = kSession; request.generation = generation;
            request.needle = query; request.maximum_hits = kMaximumResults;
            request.indexed = true; request.task_scan = true;
            request.skip_system_locations = true;
            const auto config = contents.GetConfig();
            request.maximum_file_bytes = config.maximum_file_bytes;
            request.maximum_document_bytes = config.maximum_document_bytes;
            if (current_only && !current_folder.empty()) request.root = current_folder;
            contents.SearchAsync(std::move(request));
        } else {
            index::Query request;
            request.session_id = kSession; request.needle = query; request.limit = kMaximumResults;
            if (!search_pinyin) request.needle = L"nopinyin: " + request.needle;
            if (current_only) request.path_prefix = current_folder;
            local_ready = !index::FilenameSearchRoot(request.path_prefix).empty();
            if (!local_ready) filenames->SearchAsync(request, generation, hwnd, kIndexResult);
            network->SearchAsync(request, generation, hwnd, kNetworkResult);
            if (display_alias_worker && display_aliases) {
                if (!display_alias_mailbox) display_alias_mailbox =
                    std::make_shared<app::DisplayAliasSearchMailbox>(hwnd, kDisplayAliasResult);
                display_alias_request = app::QueueDisplayAliasSearch(*display_alias_worker, display_aliases,
                    request, generation, display_alias_mailbox);
            }
            SetTimer(hwnd, kConnectTimeout, 10000, nullptr);
        }
    }
    void MergeFilenames() {
        rows.clear();
        size_t duplicates = 0;
        index::SearchResult candidates;
        candidates.hits.reserve(local_result.hits.size() + network_result.hits.size());
        for (const auto* source : {&local_result, &network_result}) for (const auto& hit : source->hits) {
            const bool duplicate = std::any_of(candidates.hits.begin(), candidates.hits.end(), [&](const auto& other) {
                return path::EqualInsensitive(other.path, hit.path);
            });
            if (duplicate) { ++duplicates; continue; }
            if (display_alias_hits.empty() && candidates.hits.size() >= kMaximumResults) continue;
            candidates.hits.push_back(hit);
        }
        total = local_result.total + network_result.total;
        total -= std::min(total, duplicates);
        if (!display_alias_hits.empty()) {
            candidates.total = total;
            index::Query request;
            request.needle = search_pinyin ? query : L"nopinyin: " + query;
            request.limit = kMaximumResults;
            candidates = index::MergeSearchResults(request, candidates, {}, display_alias_hits);
            total = candidates.total;
        }
        for (auto& hit : candidates.hits) {
            if (rows.size() >= kMaximumResults) break;
            rows.push_back({std::move(hit.name), std::move(hit.path), {}, hit.is_dir});
        }
        truncated = total > rows.size();
        busy = !(local_ready && network_ready) || network_result.scanning || display_alias_request;
        if (!busy) KillTimer(hwnd, kConnectTimeout);
        ClampSelection(false); Invalidate();
    }
    void Open(bool location) {
        if (!open_callback || rows.empty() || selected < 0 || selected >= static_cast<int>(rows.size())) return;
        auto path = rows[static_cast<size_t>(selected)].path;
        Hide(); open_callback(path, location);
    }
    void HideScopeTooltip() {
        if (hwnd) KillTimer(hwnd, kScopeTooltipTimer);
        const bool redraw = scope_tooltip_visible;
        scope_hover = false;
        scope_tooltip_visible = false;
        if (redraw) Invalidate();
    }
    void UpdateScopeHover(float x, float y) {
        const bool hovered = current_only && !current_folder_tip.empty() &&
            x >= width - 126 && x < width - 12 && y >= 76 && y < 129;
        scope_pointer_x = x;
        scope_pointer_y = y;
        if (hovered == scope_hover) {
            if (hovered && scope_tooltip_visible) Invalidate();
            return;
        }
        HideScopeTooltip();
        scope_hover = hovered;
        if (scope_hover) SetTimer(hwnd, kScopeTooltipTimer, 400, nullptr);
    }
    void FocusControl(Focus next) {
        focus = next;
        SetFocus(next == Focus::Edit ? edit : hwnd);
        Invalidate();
    }
    void TraverseFocus(bool reverse) {
        int next = static_cast<int>(focus);
        do {
            next = (next + (reverse ? 5 : 1)) % 6;
        } while (next == static_cast<int>(Focus::Current) && current_folder.empty());
        FocusControl(static_cast<Focus>(next));
    }
    void ActivateFocusedControl() {
        if (focus == Focus::Name || focus == Focus::Content) {
            const bool next = focus == Focus::Content;
            if (content_mode != next) { content_mode = next; Changed(); }
        } else if (focus == Focus::Everywhere || focus == Focus::Current) {
            const bool next = focus == Focus::Current && !current_folder.empty();
            if (current_only != next) { current_only = next; Changed(); }
        }
    }
    bool Key(WPARAM key) {
        if (composing) return false;
        if (key == VK_ESCAPE) { Hide(); return true; }
        if (key == VK_RETURN) { Open((GetKeyState(VK_CONTROL) & 0x8000) != 0); return true; }
        if (key == VK_TAB) { TraverseFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0); return true; }
        if (focus != Focus::Edit && focus != Focus::Results) {
            if (key == VK_SPACE) { ActivateFocusedControl(); return true; }
            if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN) {
                if (focus == Focus::Name || focus == Focus::Content)
                    FocusControl(focus == Focus::Name ? Focus::Content : Focus::Name);
                else if (!current_folder.empty())
                    FocusControl(focus == Focus::Everywhere ? Focus::Current : Focus::Everywhere);
                ActivateFocusedControl();
                return true;
            }
            return false;
        }
        if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
            selected += key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -PageSize() : PageSize();
            ClampSelection(); Invalidate(); return true;
        }
        return false;
    }
    static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_IME_STARTCOMPOSITION) { self->composing = true; self->Cancel(); }
        if (message == WM_IME_ENDCOMPOSITION) {
            self->composing = false;
            const auto result = self->DefaultEdit(window, message, wp, lp);
            self->Changed(); return result;
        }
        if (message == WM_SETFOCUS) { self->focus = Focus::Edit; self->Invalidate(); }
        if (message == WM_KILLFOCUS) self->Invalidate();
        if (message == WM_KEYDOWN && self->Key(wp)) return 0;
        if (message == WM_CHAR && !self->composing && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
        LRESULT result = 0;
        if (ui::HandleChildEditMessage(self->compositor, self->edit_format.Get(), self->Theme().text,
            self->EditBackground(), self->background, window, message, wp, lp, result)) return result;
        return self->DefaultEdit(window, message, wp, lp);
    }
    LRESULT DefaultEdit(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        return ui::DefPresentedChildEditProc(compositor, edit_format.Get(), Theme().text,
            EditBackground(), window, message, wp, lp);
    }
    void Layout() {
        RECT rect{}; GetClientRect(hwnd, &rect);
        width = static_cast<float>(rect.right) / scale; height = static_cast<float>(rect.bottom) / scale;
        StopScroll();
        if (target) {
            if (target.Get() == compositor.Dc()) compositor.Resize(rect.right, rect.bottom);
            target->SetDpi(96 * scale, 96 * scale);
            ComPtr<ID2D1HwndRenderTarget> window_target;
            if (SUCCEEDED(target.As(&window_target))) window_target->Resize(D2D1::SizeU(static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)));
        }
        if (edit_font) DeleteObject(edit_font);
        if (EnsureTarget()) {
            edit_format.Reset();
            ui::typography::CreateTextFormat(compositor.DwriteFactory(),
                {FontRole::Text, 23 * scale}, &edit_format);
        }
        edit_font = ui::typography::CreateEditFont(scale, 23);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
        const float edit_left = kEditLeft + kEditTextInset;
        const float edit_width = width - kEditRight - edit_left;
        MoveWindow(edit, Px(edit_left), Px(23), Px(edit_width), Px(35), TRUE);
        // Clip the redirected child as well as its parent-painted background.
        const HRGN edit_region = CreateRoundRectRgn(0, 0, Px(edit_width) + 1, Px(35) + 1,
            Px(kEditCornerRadius * 2), Px(kEditCornerRadius * 2));
        if (edit_region && !SetWindowRgn(edit, edit_region, TRUE)) DeleteObject(edit_region);
        if (compositor.CustomEditEnabled() && edit_format)
            compositor.PresentLumaEdit(edit, edit_format.Get(), Theme().text, EditBackground());
        ClampSelection(); Invalidate();
    }
    bool EnsureTarget() {
        if (target) return true;
        if (!write_factory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_factory.GetAddressOf())))) return false;
        RECT rect{}; GetClientRect(hwnd, &rect);
        if (compositor.NeedsRecovery()) {
            if (!compositor.Recover()) return false;
            material.Invalidate();
        } else if (!compositor.Dc() && !compositor.Init(hwnd)) return false;
        material.SetCompositor(&compositor);
        target = compositor.Dc();
        target->SetDpi(96 * scale, 96 * scale);
        painter.SetCompositor(&compositor);
        // This surface uses logical DIPs through its render-target DPI.
        painter.SetScale(1.0f);
        return SUCCEEDED(target->CreateSolidColorBrush(Color(0xffffff), &brush));
    }
    void Fill(D2D1_RECT_F rect, UINT32 color, float radius = 0) {
        brush->SetColor(Color(color));
        if (radius) target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        else target->FillRectangle(rect, brush.Get());
    }
    void Label(const std::wstring& text, D2D1_RECT_F rect, float size, UINT32 color, bool bold = false, FontRole role = FontRole::Text, bool highlight = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(ui::typography::CreateTextFormat(compositor.DwriteFactory(),
            {role, size, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL}, &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(write_factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) format->SetTrimming(&trim, ellipsis.Get());
        brush->SetColor(Color(color));
        ComPtr<IDWriteTextLayout> layout;
        // Labels stay in DIPs; the target DPI supplies the physical glyph scale.
        // Fluent icon glyphs keep their existing DirectWrite rendering.
        if (role != FontRole::Icon && SUCCEEDED(ui::typography::CreateTextLayout(write_factory.Get(), text.data(), static_cast<UINT32>(text.size()), format.Get(),
            std::max(0.0f, rect.right - rect.left), std::max(0.0f, rect.bottom - rect.top), &layout, scale))) {
            if (highlight && !query.empty()) {
                ComPtr<ID2D1SolidColorBrush> accent;
                if (SUCCEEDED(target->CreateSolidColorBrush(Theme().accent, &accent))) {
                    for (size_t start = 0; start + query.size() <= text.size(); ++start) {
                        if (CompareStringOrdinal(text.data() + start, static_cast<int>(query.size()), query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL)
                            layout->SetDrawingEffect(accent.Get(), {static_cast<UINT32>(start), static_cast<UINT32>(query.size())});
                    }
                }
            }
            if (target.Get() == compositor.Dc())
                compositor.DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            else target->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format.Get(), rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, ui::typography::MeasuringMode());
    }
    void DrawLogo() {
        if (logo_target != target.Get()) { logo.Reset(); logo_target = target.Get(); }
        if (!logo) {
            const auto module = GetModuleHandleW(nullptr);
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_GLOBAL_SEARCH_LOGO), RT_RCDATA);
            const auto data = resource ? LoadResource(module, resource) : nullptr;
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (data && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)), SizeofResource(module, resource))) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &logo);
        }
        if (logo) target->DrawBitmap(logo.Get(), {27, 20, 65, 58});
    }
    float TextWidth(const std::wstring& text, float size = 12, bool bold = false) {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(ui::typography::CreateTextFormat(compositor.DwriteFactory(),
            {FontRole::Text, size, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL}, &format)) &&
            SUCCEEDED(ui::typography::CreateTextLayout(write_factory.Get(), text.data(), static_cast<UINT32>(text.size()), format.Get(), 2000, 40, &layout, scale)))
            layout->GetMetrics(&metrics);
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    }
    void Keycap(const std::wstring& text, D2D1_RECT_F rect, UINT32 color) {
        brush->SetColor(Theme().fill_input);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect, 4, 4), brush.Get());
        Label(text, rect, 12, color, false, FontRole::Text, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    void DrawKeys(float bottom, UINT32 color) {
        const std::wstring keys[] = {L"↑", L"↓", L"Enter", L"Ctrl + Enter"};
        const std::wstring actions[] = {L"", Text(StringId::GlobalSearchSelect), Text(StringId::Open), Text(StringId::OpenLocation)};
        float right = width - 22;
        for (int i = 3; i >= 0; --i) {
            if (!actions[i].empty()) {
                const float label_width = TextWidth(actions[i]);
                Label(actions[i], {right - label_width, bottom + 10, right, bottom + 38}, 12, color);
                right -= label_width + 6;
            }
            const float key_width = std::max(24.0f, TextWidth(keys[i]) + 14);
            Keycap(keys[i], {right - key_width, bottom + 12, right, bottom + 36}, color);
            right -= key_width + (i == 1 ? 4 : 16);
        }
    }
    void DrawScopeTooltip(const ui::Theme& theme) {
        if (!scope_tooltip_visible || !current_only || current_folder_tip.empty()) return;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(ui::typography::CreateTextFormat(compositor.DwriteFactory(),
            {ui::typography::FontRole::Text, 12 * scale}, &format))) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(ui::typography::CreateTextLayout(write_factory.Get(), current_folder_tip.data(),
            static_cast<UINT32>(current_folder_tip.size()), format.Get(),
            std::max(1.0f, (width - 32) * scale), 10000, &layout))) return;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return;
        const float tooltip_width = std::min(
            ui::typography::MeasureLine(&compositor, format.Get(), current_folder_tip) + 16 * scale,
            (width - 16) * scale);
        const float tooltip_height = std::ceil(metrics.height) + 16 * scale;
        const float x = std::round(std::clamp((scope_pointer_x + 12) * scale,
            8 * scale, width * scale - tooltip_width - 8 * scale));
        const float y = std::round(std::clamp((scope_pointer_y + 18) * scale,
            8 * scale, std::max(8 * scale, height * scale - tooltip_height - 8 * scale)));
        const D2D1_RECT_F bounds{x, y, x + tooltip_width, y + tooltip_height};
        // Painter/LumaText rasterize at physical pixel sizes on a 96-DPI target.
        // Restore the floating window's DIP coordinate system after the tooltip.
        target->SetDpi(96, 96);
        painter.SetScale(scale);
        if (painter.BeginFrame(theme, ui::IsHighContrast())) {
            painter.DrawTooltip(bounds, metrics.lineCount > 1 ? std::wstring_view{} : current_folder_tip);
            if (metrics.lineCount > 1) {
                brush->SetColor(theme.text);
                if (target.Get() == compositor.Dc())
                    compositor.DrawTextLayout({x + 8 * scale, y + 8 * scale}, layout.Get(), brush.Get());
                else target->DrawTextLayout({x + 8 * scale, y + 8 * scale}, layout.Get(), brush.Get());
            }
        }
        target->SetDpi(96 * scale, 96 * scale);
    }
    void Paint(bool present = true) {
        PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint);
        if (EnsureTarget()) {
            const auto theme = Theme();
            const bool hc = ui::IsHighContrast();
            const UINT32 fg = Rgb(theme.text), muted = Rgb(theme.text_secondary);
            const UINT32 line = Rgb(theme.stroke_divider), accent = Rgb(theme.accent);
            target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0.0f));
            const bool live = target.Get() == compositor.Dc();
            const bool image_mode = !ui::IsHighContrast() && !background_image.empty() && effect == ui::WindowEffect::None;
            bool drawn = false;
            if (live && !ui::IsHighContrast()) {
                const D2D1_RECT_F bounds{0, 0, width, height};
                drawn = image_mode ? material.DrawSourceCover(compositor.Dc(), bounds, background_image) :
                    material.DrawBackdrop(compositor.Dc(), bounds, effect, dark, background_image);
            }
            auto tint = theme.bg;
            if (image_mode) tint.a = drawn ? (dark ? 0.55f : 0.60f) : 1.0f;
            else if (drawn) tint.a = 0;
            else if (live && !ui::IsHighContrast() && backdrop && compositor.UsesTransparentComposition()) tint.a = dark ? 0.72f : 0.78f;
            brush->SetColor(tint); target->FillRectangle({0, 0, width, height}, brush.Get());
            Fill({kEditLeft, 23, width - kEditRight, 58}, Rgb(theme.header_bg), kEditCornerRadius);
            DrawLogo();
            Fill({80, 24, 81, 54}, line);
            Label(L"\xE721", {91, 22, 115, 57}, 21, muted, false, FontRole::Icon);
            Keycap(L"Esc", {width - 60, 23, width - 22, 53}, muted);
            Fill({0, kHeader, width, kHeader + 1}, line);
            Label(Text(StringId::SearchModeName), {28, 83, 110, 120}, 16, content_mode ? muted : fg, !content_mode);
            Label(Text(StringId::SearchModeContent), {124, 83, 208, 120}, 16, content_mode ? fg : muted, content_mode);
            const float tab_center = (content_mode ? 124.0f : 28.0f) +
                TextWidth(Text(content_mode ? StringId::SearchModeContent : StringId::SearchModeName), 16, true) * 0.5f;
            Fill({tab_center - 12, 124, tab_center + 12, 127}, accent, 1.5f);
            const UINT32 disabled = Rgb(theme.text_disabled);
            Label(Text(StringId::GlobalSearchEverywhere), {width - 230, 83, width - 130, 120}, 15,
                current_only ? muted : fg, !current_only, FontRole::Text, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            Label(Text(StringId::GlobalSearchCurrentFolder), {width - 122, 83, width - 22, 120}, 15,
                current_folder.empty() ? disabled : current_only ? fg : muted,
                current_only && !current_folder.empty(), FontRole::Text, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            const float scope_center = current_only ? width - 72 : width - 180;
            Fill({scope_center - 12, 124, scope_center + 12, 127}, accent, 1.5f);
            Fill({0, 128, width, 129}, line);
            if (GetFocus() == hwnd && focus != Focus::Edit) {
                D2D1_RECT_F ring{};
                switch (focus) {
                case Focus::Name: ring = {24, 80, 114, 122}; break;
                case Focus::Content: ring = {120, 80, 212, 122}; break;
                case Focus::Everywhere: ring = {width - 234, 80, width - 126, 122}; break;
                case Focus::Current: ring = {width - 126, 80, width - 18, 122}; break;
                case Focus::Results: ring = {3, 133, width - 3, height - kFooter - 2}; break;
                default: break;
                }
                brush->SetColor(theme.text);
                target->DrawRoundedRectangle(D2D1::RoundedRect(ring, 4, 4), brush.Get(), 2);
            }
            const float bottom = height - kFooter;
            target->PushAxisAlignedClip({0, 136, width, bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (rows.empty()) {
                const auto& status = !error.empty() ? error : busy ? Text(StringId::GlobalSearchLoading) : query.empty() ? Text(StringId::GlobalSearchEmpty) : Text(StringId::GlobalSearchNoResults);
                const D2D1_RECT_F bounds{16, 136, width - 16, bottom - 8};
                if (!busy && error.empty()) {
                    const auto layout = ui::MakePaneEmptyLayout(bounds, 1, false);
                    if (empty_art_target != target.Get() || empty_art_scale != scale) {
                        empty_art.Reset(); empty_art_target = target.Get(); empty_art_scale = scale;
                        ui::CreateSvgResourceBitmap(target.Get(), IDR_EMPTY_FOLDER_SVG,
                            D2D1::SizeU(static_cast<UINT32>(Px(280)), static_cast<UINT32>(Px(280 * 360.0f / 512))), D2D1::SizeF(512, 360), &empty_art);
                    }
                    if (empty_art) target->DrawBitmap(empty_art.Get(), layout.art, dark ? 1.0f : 0.68f);
                    Label(status, layout.title, 16, muted, false, FontRole::Text, false, DWRITE_TEXT_ALIGNMENT_CENTER);
                } else {
                    Label(status, bounds, 16, muted, false, FontRole::Text, false, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
            const int first = static_cast<int>(scroll_y / kRow);
            for (int i = first; i < static_cast<int>(rows.size()); ++i) {
                const float y = 136 + static_cast<float>(i) * kRow - scroll_y;
                if (y >= bottom) break;
                const auto& row = rows[static_cast<size_t>(i)];
                const bool selected_hc = hc && i == selected;
                const UINT32 row_fg = selected_hc ? Rgb(theme.text_selected) : fg;
                const UINT32 row_muted = selected_hc ? row_fg : muted;
                if (i == selected) {
                    brush->SetColor(theme.fill_selected);
                    target->FillRoundedRectangle(D2D1::RoundedRect({5, y, width - 5, y + kRow - 3}, 6, 6), brush.Get());
                    Fill({5, y + 9, 8, y + kRow - 12}, accent, 2);
                }
                const auto dot = row.name.find_last_of(L'.');
                std::wstring extension = dot == std::wstring::npos ? L"" : row.name.substr(dot);
                for (auto& letter : extension) letter = static_cast<wchar_t>(towlower(letter));
                const bool excel = extension == L".xlsx" || extension == L".xls" || extension == L".csv";
                const bool word = extension == L".docx" || extension == L".doc";
                const bool pdf = extension == L".pdf", powerpoint = extension == L".pptx" || extension == L".ppt";
                const UINT32 icon_color = hc ? row_fg : row.directory ? 0xd59b27 : excel ? 0x178753 : pdf ? 0xc4413b : powerpoint ? 0xc55a39 : 0x367cb5;
                if (!hc) Fill({29, y + 23, 58, y + 57}, icon_color, 4);
                const UINT32 icon_fg = hc ? row_fg : 0xffffff;
                if (excel || word || pdf || powerpoint) Label(excel ? L"X" : word ? L"W" : pdf ? L"PDF" : L"P", {33, y + 27, 58, y + 54}, pdf ? 11.0f : 19.0f, icon_fg, true);
                else Label(row.directory ? L"\xE8B7" : L"\xE8A5", {34, y + 27, 58, y + 54}, 19, icon_fg, false, FontRole::Icon);
                const auto alias = model::FindDisplayAlias(display_aliases, row.path);
                Label(alias.empty() ? row.name : std::wstring(alias),
                    {78, y + 8, width - 30, y + 35}, 17, row_fg, true, FontRole::Text, !hc);
                Label(ParentPath(row.path), {78, y + 34, width - 30, y + 57}, 14, row_muted);
                if (content_mode) Label(row.snippet, {78, y + 56, width - 30, y + 80}, 14, row_muted, false, FontRole::Text, !hc);
            }
            target->PopAxisAlignedClip();
            if (MaxScroll() > 0) {
                const float track = bottom - 142, thumb = std::min(track, std::max(20.0f, track * ViewHeight() / (static_cast<float>(rows.size()) * kRow)));
                const float top = 138 + (track - thumb) * scroll_y / MaxScroll();
                Fill({width - 4, top, width - 1, top + thumb}, muted, 1.5f);
            }
            Fill({0, bottom, width, bottom + 1}, line);
            Label(!error.empty() ? error : busy ? Text(StringId::GlobalSearchLoading) : std::to_wstring(total) + Text(StringId::GlobalSearchResults), {25, bottom + 8, 155, height - 8}, 12, muted);
            if (truncated) Label(Text(StringId::GlobalSearchTruncated), {165, bottom + 8, width - 22, height - 8}, 12, muted, false, FontRole::Text, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            else DrawKeys(bottom, muted);
            DrawScopeTooltip(theme);
            const HRESULT rendered = target->EndDraw();
            if (FAILED(rendered)) { compositor.NotifyDeviceLost(rendered); empty_art.Reset(); empty_art_target = nullptr; logo.Reset(); logo_target = nullptr; brush.Reset(); target.Reset(); }
            else if (live && present) compositor.Present();
        }
        EndPaint(hwnd, &paint);
    }
    LRESULT Message(UINT message, WPARAM wp, LPARAM lp) {
        if (message == ui::typography::TextBackendChangedMessage()) {
            compositor.UpdateTextRenderingParams(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
            Invalidate();
            return 0;
        }
        switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            TickScroll(ui::AnimationTimeMs());
            ApplyAppearance(); return 0;
        case WM_SIZE: if (edit) Layout(); return 0;
        case WM_DPICHANGED: {
            scale = static_cast<float>(HIWORD(wp)) / 96.0f;
            const auto* rect = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(); return 0;
        }
        case WM_CLOSE: Hide(); return 0;
        case WM_ACTIVATEAPP: if (!wp) Hide(); return 0;
        case WM_SETFOCUS: if (focus == Focus::Edit) SetFocus(edit); Invalidate(); return 0;
        case WM_KILLFOCUS: Invalidate(); return 0;
        case WM_KEYDOWN: if (Key(wp)) return 0; break;
        case WM_COMMAND: if (reinterpret_cast<HWND>(lp) == edit && HIWORD(wp) == EN_CHANGE) Changed(); return 0;
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp); SetBkColor(dc, Background());
            const auto ink = Theme().text;
            SetTextColor(dc, RGB(static_cast<BYTE>(ink.r * 255), static_cast<BYTE>(ink.g * 255), static_cast<BYTE>(ink.b * 255))); return reinterpret_cast<LRESULT>(background);
        }
        case WM_TIMER:
            if (wp == kScrollTimer) {
                TickScroll(ui::AnimationTimeMs());
            } else if (wp == kDebounce) Search();
            else if (wp == kConnectTimeout) {
                KillTimer(hwnd, kConnectTimeout);
                if (busy && !content_mode) { busy = false; error = Text(StringId::GlobalSearchFailed); Invalidate(); }
            } else if (wp == kScopeTooltipTimer) {
                KillTimer(hwnd, kScopeTooltipTimer);
                if (scope_hover && current_only && !current_folder_tip.empty()) {
                    scope_tooltip_visible = true;
                    Invalidate();
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_mouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                tracking_mouse = TrackMouseEvent(&tracking) != FALSE;
            }
            UpdateScopeHover(static_cast<float>(GET_X_LPARAM(lp)) / scale,
                static_cast<float>(GET_Y_LPARAM(lp)) / scale);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse = false;
            HideScopeTooltip();
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
            HideScopeTooltip();
            if (y < 76 && x > width - 70) Hide();
            else if (y >= 76 && y < 129) {
                if (x < 220) { FocusControl(x >= 114 ? Focus::Content : Focus::Name); ActivateFocusedControl(); }
                else if (x >= width - 230 && x < width - 126) {
                    FocusControl(Focus::Everywhere); ActivateFocusedControl();
                } else if (x >= width - 126 && x < width - 12 && !current_folder.empty()) {
                    FocusControl(Focus::Current); ActivateFocusedControl();
                }
            } else if (const int row = ResultRowAt(x, y); row >= 0) {
                FocusControl(Focus::Results);
                selected = row; ClampSelection(false); Invalidate();
            }
            if (IsWindowVisible(hwnd) && y < 76) FocusControl(Focus::Edit); return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const int row = ResultRowAt(static_cast<float>(GET_X_LPARAM(lp)) / scale,
                static_cast<float>(GET_Y_LPARAM(lp)) / scale);
            if (row >= 0) { selected = row; ClampSelection(false); Open(false); }
            return 0;
        }
        case WM_MOUSEWHEEL:
            Scroll(-GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA) * kRow * 3);
            return 0;
        case WM_DESTROY: StopScroll(); return 0;
        case kIndexStatus: Invalidate(); return 0;
        case kIndexResult: {
            index::SearchResult result;
            if (filenames->TakeResult(static_cast<uint32_t>(wp), result, hwnd) && wp == generation && !content_mode && IsWindowVisible(hwnd)) {
                error.clear(); local_result = std::move(result); local_ready = true; MergeFilenames();
            }
            return 0;
        }
        case kNetworkResult: {
            index::SearchResult result;
            if (network->TakeResult(static_cast<uint32_t>(wp), result, hwnd) && wp == generation && !content_mode && IsWindowVisible(hwnd)) {
                error = result.error ? Text(StringId::GlobalSearchFailed) : std::wstring{};
                if (result.scanning) KillTimer(hwnd, kConnectTimeout);
                network_result = std::move(result); network_ready = true; MergeFilenames();
            }
            return 0;
        }
        case kDisplayAliasResult:
            if (display_alias_mailbox) for (auto& result : display_alias_mailbox->Take(wp)) {
                if (result.generation != generation || content_mode || !IsWindowVisible(hwnd)) continue;
                display_alias_request.reset();
                display_alias_hits = std::move(result.hits);
                MergeFilenames();
            }
            return 0;
        case kContentResult: {
            index::ContentSearchUpdate update;
            while (contents.TakeUpdate(update)) {
                if (update.progress.generation != generation || !content_mode || !IsWindowVisible(hwnd)) continue;
                for (auto& hit : update.hits) {
                    if (rows.size() >= kMaximumResults) break;
                    rows.push_back({std::move(hit.name), std::move(hit.path), std::move(hit.snippet), false});
                }
                total = rows.size(); truncated = update.progress.truncated;
                busy = !update.progress.done;
                if (update.progress.error && update.progress.error != ERROR_CANCELLED) error = Text(StringId::GlobalSearchFailed);
                ClampSelection(false); Invalidate();
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }
    bool Show(HWND owner, bool use_dark, float requested_scale, const std::wstring& folder, bool pinyin) {
        dark = use_dark; scale = requested_scale > 0 ? requested_scale : 1;
        search_pinyin = pinyin;
        const HWND foreground = GetForegroundWindow();
        POINT cursor{}; GetCursorPos(&cursor);
        const HMONITOR active_monitor = foreground && foreground != hwnd ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) :
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        UINT x = 96, y = 96;
        if (SUCCEEDED(GetDpiForMonitor(active_monitor, MDT_EFFECTIVE_DPI, &x, &y)))
            scale = static_cast<float>(x) / 96.0f;
        (void)owner;
        current_folder = folder;
        current_folder_tip = path::StripExtendedPathPrefix(folder);
        if (folder.empty()) current_only = false;
        HideScopeTooltip();
        if (!hwnd) {
            WNDCLASSEXW cls{sizeof(cls)}; cls.style = CS_DBLCLKS;
            cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"Pulse.GlobalSearch";
            RegisterClassExW(&cls);
            // A hidden owner would hide/minimize this window too; the owner only chooses the monitor.
            if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"Pulse Search", WS_POPUP | WS_CLIPCHILDREN,
                0, 0, Px(780), Px(488), nullptr, nullptr, cls.hInstance, this)) return false;
            edit = ui::CreateChildEdit(hwnd);
            if (!edit) { DestroyWindow(hwnd); hwnd = nullptr; return false; }
            SetWindowTheme(edit, L"", L"");
            EnsureTarget();
            // Match the address editor: uploaded layered bitmaps can disappear
            // below the parent's composition surface on some display stacks.
            // LumaText paints into the redirected child DC instead.
            SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
            ShowWindow(edit, SW_SHOW);
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(edit, EM_SETLIMITTEXT, 2048, 0);
            if (!filenames) {
                owned_filenames = std::make_unique<index::IndexClient>();
                filenames = owned_filenames.get();
                filenames->Start(hwnd, kIndexStatus, kIndexResult);
            }
            if (!network) {
                owned_network = std::make_unique<index::NetworkAgentClient>();
                network = owned_network.get();
                network->Start(hwnd, kIndexStatus, kNetworkResult);
            }
            contents.Start(hwnd, kContentResult, index::ContentAgentMode::Instant);
        }
        ApplyAppearance();
        BOOL is_dark = dark; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(StringId::GlobalSearchPlaceholder).c_str()));
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(active_monitor, &monitor);
        const auto& work = monitor.rcWork;
        const int w = std::min(Px(780), static_cast<int>(work.right - work.left) - Px(32));
        const int h = std::min(Px(488), static_cast<int>(work.bottom - work.top) - Px(32));
        SetWindowPos(hwnd, HWND_TOPMOST, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 3, w, h, SWP_SHOWWINDOW);
        Layout(); SetForegroundWindow(hwnd); SetFocus(edit); SendMessageW(edit, EM_SETSEL, 0, -1);
        Changed(); return true;
    }
    ~Impl() {
        Cancel();
        if (display_alias_mailbox) display_alias_mailbox->Close();
        // Borrowed filename clients remain live for the main window.
        if (owned_filenames) owned_filenames->Stop();
        if (owned_network) owned_network->Stop();
        contents.Stop();
        if (hwnd) {
            MSG pending{};
            while (PeekMessageW(&pending, hwnd, kIndexStatus, kNetworkResult, PM_REMOVE)) {}
        }
        if (edit) RemoveWindowSubclass(edit, EditProc, 1);
        if (hwnd) DestroyWindow(hwnd);
        if (edit_font) DeleteObject(edit_font);
        if (background) DeleteObject(background);
    }
};
GlobalSearchWindow::GlobalSearchWindow() = default;
GlobalSearchWindow::~GlobalSearchWindow() = default;
bool GlobalSearchWindow::Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->Show(owner, dark, scale, current_folder, search_pinyin);
}
void GlobalSearchWindow::BindFilenameClients(index::IndexClient& filenames, index::NetworkAgentClient& network, uint32_t& next_request) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (impl_->filenames == &filenames && impl_->network == &network) return;
    impl_->Cancel();
    impl_->owned_filenames.reset();
    impl_->owned_network.reset();
    impl_->filenames = &filenames;
    impl_->network = &network;
    impl_->next_request = &next_request;
}
void GlobalSearchWindow::BindDisplayAliases(app::WorkerPool& worker, model::DisplayAliasSnapshot aliases) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->display_alias_worker = &worker;
    impl_->display_aliases = std::move(aliases);
}
void GlobalSearchWindow::SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->dark = dark; impl_->effect = effect; impl_->background_image = background_image; impl_->accent_color = accent;
    impl_->ApplyAppearance();
}
void GlobalSearchWindow::SetOpenCallback(std::function<void(const std::wstring&, bool)> callback) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->open_callback = std::move(callback);
}
void GlobalSearchWindow::Hide() { if (impl_) impl_->Hide(); }
void GlobalSearchWindow::Shutdown() { impl_.reset(); }
bool GlobalSearchWindow::Visible() const { return impl_ && impl_->hwnd && IsWindowVisible(impl_->hwnd); }
}
