// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../common/windows_compat.h"
#include "file_operation_dialog.h"
#include "dialog_keyboard.h"
#include "../ops/operation_presentation.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "typography.h"
#include "window_helpers.h"
#include "dialog_text_fit.h"
#include "conflict_details.h"
#include "animation_clock.h"
#ifdef PULSE_UI_TEST_HOOKS
#include "../bench/snapshot_output.h"
#endif

#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <string>
#include <tuple>

namespace pulse::ui {
namespace {

constexpr wchar_t kTransferClass[] = L"PulseFileOperationWindow";
constexpr wchar_t kConflictClass[] = L"PulseFileConflictWindow";
constexpr wchar_t kConfirmClass[] = L"PulseConfirmWindow";
constexpr float kConfirmMinW = 380.0f;
constexpr UINT_PTR kRenderTimer = 1;
constexpr UINT_PTR kScrollTimer = 2;
constexpr float kDlgW = 460.0f;
constexpr float kTitleH = 36.0f;
constexpr float kPadX = 20.0f;
constexpr float kFooterH = 52.0f;
constexpr float kCollapsedH = 268.0f;
constexpr float kGraphBlockH = 156.0f;

bool SameTransferPresentation(const ops::OpStatus& a, const ops::OpStatus& b) {
    return std::tie(a.active, a.type, a.phase, a.task_id, a.percent, a.summary,
        a.last_error, a.source_label, a.destination_label, a.current_item,
        a.total_bytes, a.transferred_bytes, a.total_items, a.completed_items,
        a.bytes_per_second, a.peak_bytes_per_second, a.eta_seconds, a.failed_path) ==
        std::tie(b.active, b.type, b.phase, b.task_id, b.percent, b.summary,
        b.last_error, b.source_label, b.destination_label, b.current_item,
        b.total_bytes, b.transferred_bytes, b.total_items, b.completed_items,
        b.bytes_per_second, b.peak_bytes_per_second, b.eta_seconds, b.failed_path);
}

bool TransferStatisticsActive(const ops::OpStatus& status) {
    return status.active && status.phase != ops::OpPhase::Paused &&
        status.phase != ops::OpPhase::WaitingForConflict &&
        status.phase != ops::OpPhase::WaitingForDeleteConfirmation &&
        status.phase != ops::OpPhase::Completed && status.phase != ops::OpPhase::Failed &&
        status.phase != ops::OpPhase::Cancelled;
}

using pulse::ui::ScaleDip;

struct TransferChrome {
    D2D1_RECT_F close{};
    D2D1_RECT_F details{};
    D2D1_RECT_F pause{};
    D2D1_RECT_F cancel{};
};

struct TransferLabels {
    std::wstring details;
    std::wstring pause;
    std::wstring cancel;
    bool show_pause = true;
};

TransferLabels MakeTransferLabels(const ops::OpStatus& status, bool detailed,
                                  const FileOperationCallbacks& callbacks) {
    TransferLabels labels;
    labels.details = detailed ? l10n::Get(l10n::StringId::OpSummary)
                              : l10n::Get(l10n::StringId::OpDetails);
    labels.pause = status.phase == ops::OpPhase::Paused
        ? l10n::Get(l10n::StringId::OpResume) : l10n::Get(l10n::StringId::OpPause);
    const bool emptying = status.type == ops::OpType::EmptyRecycle;
    labels.show_pause = !emptying && status.active && (status.phase == ops::OpPhase::Paused
        ? static_cast<bool>(callbacks.resume) : static_cast<bool>(callbacks.pause));
    labels.cancel = status.active && !emptying
        ? l10n::Get(l10n::StringId::Cancel) : l10n::Get(l10n::StringId::Close);
    return labels;
}

TransferChrome MakeTransferChrome(float scale, float width, float height,
                                  const fluent::Painter& painter, const TransferLabels& labels) {
    TransferChrome chrome;
    const float title = ScaleDip(scale, kTitleH);
    const float caption = ScaleDip(scale, 40.0f);
    chrome.close = D2D1::RectF(width - caption, 0, width, title);
    const float footer_h = ScaleDip(scale, kFooterH);
    const float footer_top = height - footer_h;
    const float pad = ScaleDip(scale, kPadX);
    const float gap = ScaleDip(scale, 8.0f);
    const float btn_h = painter.MeasureButtonHeight();
    const float y0 = footer_top + (footer_h - btn_h) * 0.5f;
    const float y1 = y0 + btn_h;

    const float cancel_w = painter.MeasureButtonWidth(labels.cancel);
    chrome.cancel = D2D1::RectF(width - pad - cancel_w, y0, width - pad, y1);
    if (labels.show_pause) {
        const float pause_w = painter.MeasureButtonWidth(labels.pause);
        chrome.pause = D2D1::RectF(chrome.cancel.left - gap - pause_w, y0,
                                   chrome.cancel.left - gap, y1);
    }
    chrome.details = painter.FitButtonBounds(D2D1::RectF(pad, y0, pad, y1),
                                             labels.details, L"\xE70D");
    return chrome;
}

void DrawEllipsizedText(Compositor& compositor, IDWriteTextFormat* format,
                        const D2D1_RECT_F& bounds, const std::wstring& text,
                        const D2D1_COLOR_F& color, const D2D1_COLOR_F& background) {
    auto* factory = compositor.DwriteFactory();
    auto* dc = compositor.Dc();
    if (!factory || !dc || !format || text.empty()) return;
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;
    if (compositor.DrawLumaText(text, format, bounds, color, background)) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(typography::CreateTextLayout(factory, text.c_str(), static_cast<UINT32>(text.size()),
                                         format, width, height, &layout))) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush))) return;
    dc->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawWrappedText(Compositor& compositor, IDWriteTextFormat* format,
                     const D2D1_RECT_F& bounds, const std::wstring& text,
                     const D2D1_COLOR_F& color) {
    auto* factory = compositor.DwriteFactory();
    auto* dc = compositor.Dc();
    if (!factory || !dc || !format || text.empty()) return;
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    if (width <= 0.0f || height <= 0.0f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(typography::CreateTextLayout(factory, text.c_str(), static_cast<UINT32>(text.size()),
                                         format, width, height, &layout)) || !layout.get())
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush)) || !brush.get()) return;
    dc->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get());
}

void DrawSpeedGraph(ID2D1DeviceContext* dc, const D2D1_RECT_F& plot,
                    const std::deque<double>& history, D2D1_COLOR_F accent, float scale) {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> grid;
    D2D1_COLOR_F grid_color = accent;
    grid_color.a = 0.08f;
    dc->CreateSolidColorBrush(grid_color, &grid);
    const float step = ScaleDip(scale, 20.0f);
    for (float y = plot.top + ScaleDip(scale, 12.0f); y < plot.bottom; y += step) {
        dc->DrawLine(D2D1::Point2F(plot.left, y), D2D1::Point2F(plot.right, y),
                     grid.get(), 1.0f);
    }
    if (history.size() < 2) return;

    const double max_speed = (std::max)(1.0, *std::max_element(history.begin(), history.end()));
    const float left = plot.left;
    const float right = plot.right;
    const float top = plot.top + ScaleDip(scale, 6.0f);
    const float bottom = plot.bottom;
    const size_t count = history.size();
    auto y_at = [&](size_t index) {
        return bottom - (bottom - top) *
            static_cast<float>(history[index] / max_speed);
    };
    auto x_at = [&](size_t index) {
        return left + (right - left) * static_cast<float>(index)
            / static_cast<float>(count - 1);
    };

    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (factory.get()) {
        ComPtr<ID2D1PathGeometry> geometry;
        if (SUCCEEDED(factory->CreatePathGeometry(&geometry))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(geometry->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(x_at(0), y_at(0)), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t i = 1; i < count; ++i)
                    sink->AddLine(D2D1::Point2F(x_at(i), y_at(i)));
                sink->AddLine(D2D1::Point2F(x_at(count - 1), bottom));
                sink->AddLine(D2D1::Point2F(x_at(0), bottom));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                D2D1_COLOR_F fill_color = accent;
                fill_color.a = 0.28f;
                ComPtr<ID2D1SolidColorBrush> fill;
                dc->CreateSolidColorBrush(fill_color, &fill);
                dc->FillGeometry(geometry.get(), fill.get());
            }
        }
        ComPtr<ID2D1PathGeometry> curve;
        ComPtr<ID2D1GeometrySink> curve_sink;
        if (SUCCEEDED(factory->CreatePathGeometry(&curve)) && SUCCEEDED(curve->Open(&curve_sink))) {
            curve_sink->BeginFigure(D2D1::Point2F(x_at(0), y_at(0)), D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < count; ++i)
                curve_sink->AddLine(D2D1::Point2F(x_at(i), y_at(i)));
            curve_sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(curve_sink->Close())) {
                ComPtr<ID2D1StrokeStyle> stroke;
                auto style = D2D1::StrokeStyleProperties();
                style.startCap = style.endCap = D2D1_CAP_STYLE_ROUND;
                style.lineJoin = D2D1_LINE_JOIN_ROUND;
                factory->CreateStrokeStyle(style, nullptr, 0, &stroke);
                ComPtr<ID2D1SolidColorBrush> line;
                dc->CreateSolidColorBrush(accent, &line);
                // Draw joined segments together so sharp peaks cannot expose butt-cap seams.
                dc->DrawGeometry(curve.get(), line.get(), 2.0f * scale, stroke.get());
            }
        }
    }

    ComPtr<ID2D1SolidColorBrush> line;
    dc->CreateSolidColorBrush(accent, &line);
    const float last_x = x_at(count - 1);
    const float last_y = y_at(count - 1);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(last_x, last_y),
                                  3.5f * scale, 3.5f * scale), line.get());
}

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return pulse::ui::DipRect(scale, x, y, width, height);
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                             : std::wstring(view.substr(slash + 1));
}

std::wstring ParentName(const std::wstring& path) {
    std::wstring copy = path;
    while (copy.size() > 1 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
    const size_t slash = copy.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return copy;
    return LeafName(copy.substr(0, slash));
}

std::wstring FormatDuration(uint64_t seconds) {
    if (seconds == 0) return l10n::Get(l10n::StringId::OpEstimating).c_str();
    if (seconds < 60) return std::to_wstring(seconds) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    const uint64_t minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + l10n::Get(l10n::StringId::OpMinutes).c_str()
        + std::to_wstring(seconds % 60) + l10n::Get(l10n::StringId::OpSeconds).c_str();
    return std::to_wstring(minutes / 60) + l10n::Get(l10n::StringId::OpHours).c_str()
        + std::to_wstring(minutes % 60) + l10n::Get(l10n::StringId::OpMinutesEnd).c_str();
}

class ConflictWindow {
public:
    ConflictDialogResult Show(HWND owner, const ops::ConflictItemInfo& conflict,
                              bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        conflict_ = conflict;
        conflict_.source = path::FriendlyPathText(conflict_.source);
        conflict_.destination = path::FriendlyPathText(conflict_.destination);
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner)) / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConflictClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConflictClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(480 * scale_);
        const int height = static_cast<int>(428 * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConflictClass, l10n::Get(l10n::StringId::OpConflictTitle).c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return result_;
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        OwnerDimScope owner_dim(owner_);
        EnableWindow(owner_, FALSE);
        ShowDialogWithFade(hwnd_);
        SetForegroundWindow(hwnd_);
        if (conflict_.still_valid) SetTimer(hwnd_, 1, 100, nullptr);

        MSG message{};
        while (!done_ && GetModalMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        EnableWindow(owner_, TRUE);
        return result_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConflictWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConflictWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, 434, 0, 46, 44); }
    D2D1_RECT_F CardRect(int index) const { return Rect(scale_, 20, 121 + index * 66.0f, 440, 58); }
    D2D1_RECT_F CheckRect() const { return Rect(scale_, 20, details_ ? 438.0f : 326.0f, 350, 30); }
    D2D1_RECT_F DetailRect() const { return Rect(scale_, 20, details_ ? 486.0f : 374.0f, 124, 32); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, 362, details_ ? 482.0f : 370.0f, 98, 34); }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 7;
        for (int i = 0; i < 3; ++i) if (pulse::ui::ContainsRect(CardRect(i), x, y)) return i + 1;
        if (conflict_.remaining > 1 && pulse::ui::ContainsRect(CheckRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(DetailRect(), x, y)) return 5;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 6;
        return 0;
    }

    void Complete(ops::ConflictChoice choice) {
        result_.choice = conflict_.still_valid && !conflict_.still_valid()
            ? ops::ConflictChoice::Cancel : choice;
        result_.apply_to_all = apply_all_;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void ToggleDetails() {
        details_ = !details_;
        RECT window{};
        GetWindowRect(hwnd_, &window);
        const int target = static_cast<int>((details_ ? 538 : 428) * scale_);
        SetWindowPos(hwnd_, nullptr, 0, 0, window.right - window.left, target,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void DrawCard(const Theme& theme, int index, const wchar_t* title,
                  const wchar_t* subtitle, const wchar_t* glyph,
                  D2D1_COLOR_F icon_color, const wchar_t* shortcut) {
        const auto bounds = CardRect(index);
        fluent::ControlState state{};
        state.hovered = hover_ == index + 1;
        state.pressed = pressed_ == index + 1;
        state.focused = focus_ == index;
        const bool high_contrast = IsHighContrast();
        D2D1_COLOR_F fill = high_contrast ? theme.fill_input : dark_ ? HexColor(state.hovered ? 0x474747 : 0x3A3A3A, 0.92f)
                                  : HexColor(state.hovered ? 0xE4E4E4 : 0xEEEEEE, 0.92f);
        painter_.FillRoundedRect(bounds, 10 * scale_, fill);
        painter_.StrokeRoundedRect(bounds, 10 * scale_,
            high_contrast ? (state.focused || state.hovered ? theme.accent : theme.text) :
                state.focused ? WithAlpha(theme.accent, 0.35f) : theme.stroke_card,
            state.focused ? 1.5f : 1.0f);
        const auto icon = Rect(scale_, 32, 133 + index * 66.0f, 34, 34);
        painter_.FillRoundedRect(icon, 17 * scale_, high_contrast ? theme.fill_input : icon_color);
        if (high_contrast) painter_.StrokeRoundedRect(icon, 17 * scale_, theme.text, scale_);
        painter_.DrawGlyph(glyph, icon, high_contrast ? theme.text : HexColor(0xFFFFFF));
        painter_.DrawText(title, Rect(scale_, 78, 126 + index * 66.0f, 272, 25),
                          compositor_.TextFormat(), theme.text);
        painter_.DrawText(subtitle, Rect(scale_, 78, 150 + index * 66.0f, 300, 20),
                          compositor_.SmallFormat(), theme.text_secondary);
        fluent::BadgeSpec badge{ Rect(scale_, 382, 139 + index * 66.0f, 58, 22), shortcut,
                                 fluent::BadgeKind::Keycap };
        painter_.DrawBadge(badge);
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);

        painter_.DrawGlyph(L"\xE8C8", Rect(scale_, 14, 11, 22, 22), theme.accent);
        painter_.DrawText(l10n::Get(l10n::StringId::OpConflictTitle).c_str(), Rect(scale_, 42, 0, 260, 44),
                          compositor_.SmallFormat(), theme.text);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 7;
        close_state.pressed = pressed_ == 7;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close,
                                    {}, close_state);
        painter_.FillRoundedRect(Rect(scale_, 0, 43, 480, 1), 0, theme.stroke_divider);

        const std::wstring route = ParentName(conflict_.source) + L"  →  "
                                 + ParentName(conflict_.destination);
        painter_.DrawBadge({ Rect(scale_, 20, 58, 250, 24), route,
                             fluent::BadgeKind::Neutral });
        const std::wstring headline = l10n::Get(l10n::StringId::OpConflictLead).c_str() + LeafName(conflict_.destination) + L"”";
        painter_.DrawText(headline, Rect(scale_, 20, 84, 440, 30),
                          compositor_.HeaderFormat(), theme.text);

        DrawCard(theme, 0, l10n::Get(l10n::StringId::OpReplaceFile).c_str(), l10n::Get(l10n::StringId::OpReplaceDesc).c_str(),
                 L"\xE73E", HexColor(0x107C10), L"Alt+R");
        DrawCard(theme, 1, l10n::Get(l10n::StringId::OpSkipFile).c_str(), l10n::Get(l10n::StringId::OpSkipDesc).c_str(),
                 L"\xE72A", HexColor(0x0078D4), L"Alt+S");
        DrawCard(theme, 2, l10n::Get(l10n::StringId::OpKeepBoth).c_str(), l10n::Get(l10n::StringId::OpKeepBothDesc).c_str(),
                 L"\xE8C8", HexColor(0x6B69D6), L"Alt+K");

        if (conflict_.remaining > 1) {
            fluent::ControlState check{};
            check.checked = apply_all_;
            check.hovered = hover_ == 4;
            check.focused = focus_ == 3;
            const std::wstring label = l10n::Get(l10n::StringId::OpApplyLead).c_str() + std::to_wstring(conflict_.remaining)
                                     + l10n::Get(l10n::StringId::OpApplyEnd).c_str();
            painter_.DrawCheckBox(CheckRect(), label, check);
        }
        painter_.FillRoundedRect(Rect(scale_, 0, details_ ? 474.0f : 360.0f, 480, 1), 0,
                                 theme.stroke_divider);
        fluent::ControlState detail_state{};
        detail_state.hovered = hover_ == 5;
        detail_state.focused = focus_ == (conflict_.remaining > 1 ? 4 : 3);
        painter_.DrawButton({ DetailRect(), details_ ? l10n::Get(l10n::StringId::OpBrief).c_str() : l10n::Get(l10n::StringId::OpMore).c_str(),
                              details_ ? L"\xE70E" : L"\xE70D",
                              fluent::ButtonKind::Transparent, detail_state });
        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 6;
        cancel_state.pressed = pressed_ == 6;
        cancel_state.focused = focus_ == (conflict_.remaining > 1 ? 5 : 4);
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel).c_str(), {}, fluent::ButtonKind::Standard,
                              cancel_state });

        if (details_) {
            const float top = 326.0f;
            const std::wstring source_meta = pulse::format::ByteSize(conflict_.source_size) + L"  ·  "
                                           + pulse::format::LocalFileTime(conflict_.source_modified, l10n::Get(l10n::StringId::OpUnknown).c_str());
            const std::wstring target_meta = pulse::format::ByteSize(conflict_.destination_size) + L"  ·  "
                                           + pulse::format::LocalFileTime(conflict_.destination_modified, l10n::Get(l10n::StringId::OpUnknown).c_str());
            const auto source_label = l10n::Get(l10n::StringId::OpSource);
            const auto existing_label = l10n::Get(l10n::StringId::OpExisting);
            DrawConflictDetails(painter_, compositor_, Rect(scale_, 20, top, 440, 104), scale_, theme, dark_, true,
                {source_label, source_meta, conflict_.source}, {existing_label, target_meta, conflict_.destination});
        }
        pulse::ui::EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize = {static_cast<LONG>(std::ceil(480.0f * scale_)),
                static_cast<LONG>(std::ceil((details_ ? 538.0f : 428.0f) * scale_))};
            return 0;
        }
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 44 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed) {
                if (hit >= 1 && hit <= 3) {
                    focus_ = hit - 1;
                    Complete(hit == 1 ? ops::ConflictChoice::Replace
                             : hit == 2 ? ops::ConflictChoice::Skip
                                        : ops::ConflictChoice::KeepBoth);
                } else if (hit == 4) apply_all_ = !apply_all_;
                else if (hit == 5) ToggleDetails();
                else if (hit == 6 || hit == 7) Complete(ops::ConflictChoice::Cancel);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (GetKeyState(VK_MENU) < 0) {
                if (wparam == 'R') { Complete(ops::ConflictChoice::Replace); return 0; }
                if (wparam == 'S') { Complete(ops::ConflictChoice::Skip); return 0; }
                if (wparam == 'K') { Complete(ops::ConflictChoice::KeepBoth); return 0; }
            }
            if (wparam == VK_ESCAPE) { Complete(ops::ConflictChoice::Cancel); return 0; }
            if (wparam == VK_TAB) {
                const int count = conflict_.remaining > 1 ? 6 : 5;
                focus_ = (focus_ + (GetKeyState(VK_SHIFT) < 0 ? count - 1 : 1)) % count;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (focus_ <= 2) Complete(focus_ == 0 ? ops::ConflictChoice::Replace
                                         : focus_ == 1 ? ops::ConflictChoice::Skip
                                                       : ops::ConflictChoice::KeepBoth);
                else if (focus_ == 3 && conflict_.remaining > 1) apply_all_ = !apply_all_;
                else if (focus_ == (conflict_.remaining > 1 ? 4 : 3)) ToggleDetails();
                else Complete(ops::ConflictChoice::Cancel);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_TIMER:
            if (conflict_.still_valid && !conflict_.still_valid()) Complete(ops::ConflictChoice::Cancel);
            return 0;
        case WM_SYSKEYDOWN:
            if (wparam == 'R') { Complete(ops::ConflictChoice::Replace); return 0; }
            if (wparam == 'S') { Complete(ops::ConflictChoice::Skip); return 0; }
            if (wparam == 'K') { Complete(ops::ConflictChoice::KeepBoth); return 0; }
            break;
        case WM_CLOSE:
            Complete(ops::ConflictChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    ops::ConflictItemInfo conflict_;
    ConflictDialogResult result_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool details_ = false;
    bool apply_all_ = false;
    bool done_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
};

class ConfirmWindow {
public:
    bool Show(HWND owner, const ConfirmDialogSpec& spec, bool dark, D2D1_COLOR_F accent) {
        if (spec.still_valid && !spec.still_valid()) return false;
        owner_ = owner;
        spec_ = spec;
        spec_.message = path::FriendlyPathText(spec_.message);
        spec_.note = path::FriendlyPathText(spec_.note);
        for (auto& row : spec_.rows) if (row.is_path) row.text = path::FriendlyPathText(row.text);
        if (spec_.confirm_text.empty()) spec_.confirm_text = pulse::l10n::Get(pulse::l10n::StringId::ConfirmDefault);
        if (spec_.cancel_text.empty()) spec_.cancel_text = pulse::l10n::Get(pulse::l10n::StringId::Cancel);
        dark_ = dark;
        accent_ = accent;
        accepted_ = false;
        checked_ = false;
        done_ = false;
        focus_ = spec_.cancel_is_default ? 1 : 0;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConfirmClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConfirmClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kConfirmMinW * scale_);
        const int height = static_cast<int>(188.0f * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConfirmClass,
            spec_.title.c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return false;
        RECT placed{};
        GetWindowRect(hwnd_, &placed);
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, placed.right - placed.left,
                          placed.bottom - placed.top);
        OwnerDimScope owner_dim(owner_);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowDialogWithFade(hwnd_);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        if (spec_.still_valid) SetTimer(hwnd_, 1, 100, nullptr);

        MSG message{};
        while (!done_ && GetModalMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_) {
            EnableWindow(owner_, TRUE);
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConfirmWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConfirmWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F CloseRect() const { return close_rc_; }
    D2D1_RECT_F CancelRect() const { return cancel_rc_; }
    D2D1_RECT_F ConfirmRect() const { return confirm_rc_; }
    D2D1_RECT_F AlternateRect() const { return alternate_rc_; }

    float MeasureContent(float width) {
        if (!compositor_.DwriteFactory() || !compositor_.TextFormat()) return 0.0f;
        const float available = std::max(1.0f, width);
        body_height_ = spec_.message.empty() ? 0.0f : typography::MeasureWrapped(
            compositor_.DwriteFactory(), compositor_.TextFormat(), spec_.message, available);
        float height = body_height_;
        row_heights_.clear();
        if (!spec_.rows.empty()) {
            if (height > 0.0f) height += ScaleDip(scale_, 12.0f);
            for (const auto& row : spec_.rows) {
                const float text_height = row.is_path ? ScaleDip(scale_, 22.0f) :
                    typography::MeasureWrapped(compositor_.DwriteFactory(), compositor_.TextFormat(),
                        row.text, std::max(1.0f, available - (row.boxed ? ScaleDip(scale_, 24.0f) : 0.0f)));
                const float row_height = row.boxed
                    ? std::max(ScaleDip(scale_, 36.0f), text_height + ScaleDip(scale_, 12.0f))
                    : std::max(ScaleDip(scale_, 22.0f), text_height);
                row_heights_.push_back(row_height);
                height += row_height + ScaleDip(scale_, 4.0f);
            }
        }
        note_height_ = spec_.note.empty() ? 0.0f : typography::MeasureWrapped(
            compositor_.DwriteFactory(), compositor_.SmallFormat(), spec_.note, available);
        if (note_height_ > 0.0f) height += ScaleDip(scale_, 12.0f) + note_height_;
        return std::max(ScaleDip(scale_, 22.0f), height);
    }

    void LayoutFromSize(float width, float height) {
        const float pad = ScaleDip(scale_, 20.0f);
        const float title_h = ScaleDip(scale_, 36.0f);
        const float close_w = ScaleDip(scale_, 46.0f);
        const float gap = ScaleDip(scale_, 8.0f);
        const float btn_h = painter_.MeasureButtonHeight();
        close_rc_ = D2D1::RectF(width - close_w, 0, width, title_h);
        const float cancel_w = painter_.MeasureButtonWidth(spec_.cancel_text);
        const float confirm_w = painter_.MeasureButtonWidth(spec_.confirm_text);
        const float alternate_w = spec_.alternate_text.empty() ? 0.0f : painter_.MeasureButtonWidth(spec_.alternate_text);
        const float buttons_width = cancel_w + gap + confirm_w + (alternate_w > 0 ? gap + alternate_w : 0.0f);
        const bool stacked = buttons_width > width - 2.0f * pad;
        const float y1 = height - ScaleDip(scale_, 10.0f);
        const float y0 = y1 - btn_h;
        confirm_rc_ = D2D1::RectF(width - pad - confirm_w, y0, width - pad, y1);
        cancel_rc_ = D2D1::RectF(confirm_rc_.left - gap - cancel_w, y0,
                                 confirm_rc_.left - gap, y1);
        if (!spec_.alternate_text.empty()) {
            alternate_rc_ = D2D1::RectF(cancel_rc_.left - gap - alternate_w, y0,
                cancel_rc_.left - gap, y1);
        }
        float footer_top = y0;
        if (stacked) {
            confirm_rc_ = D2D1::RectF(pad, y0, width - pad, y1);
            cancel_rc_ = D2D1::RectF(pad, y0 - gap - btn_h, width - pad, y0 - gap);
            footer_top = cancel_rc_.top;
            if (alternate_w > 0) {
                alternate_rc_ = D2D1::RectF(pad, footer_top - gap - btn_h, width - pad, footer_top - gap);
                footer_top = alternate_rc_.top;
            }
        }
        const float msg_top = title_h + ScaleDip(scale_, 16.0f);
        const float check_h = spec_.checkbox_text.empty() ? 0.0f : ScaleDip(scale_, 38.0f);
        message_rc_ = D2D1::RectF(pad, msg_top, width - pad, footer_top - ScaleDip(scale_, 12.0f) - check_h);
        message_height_ = MeasureContent(width - 2.0f * pad);
        dc_scroll_max_ = std::max(0.0f, message_height_ - (message_rc_.bottom - message_rc_.top));
        message_scroll_animation_.Tick(message_scroll_, dc_scroll_max_, AnimationTimeMs(), false);
        KillTimer(hwnd_, kScrollTimer);
        check_rc_ = D2D1::RectF(pad, message_rc_.bottom + ScaleDip(scale_, 4.0f),
                               width - pad, message_rc_.bottom + check_h);
        divider_top_ = title_h;
        divider_footer_ = footer_top - ScaleDip(scale_, 12.0f);
        dip_w_ = width / std::max(scale_, 0.001f);
    }

    void SizeToContent() {
        if (!hwnd_ || !compositor_.DwriteFactory()) return;
        const float pad = ScaleDip(scale_, 20.0f);
        const float title_h = ScaleDip(scale_, 36.0f);
        const float gap = ScaleDip(scale_, 8.0f);
        const float btn_h = painter_.MeasureButtonHeight();
        const float cancel_w = painter_.MeasureButtonWidth(spec_.cancel_text);
        const float confirm_w = painter_.MeasureButtonWidth(spec_.confirm_text);
        float width = std::max(ScaleDip(scale_, kConfirmMinW),
                               pad + cancel_w + gap + confirm_w + pad +
                               (spec_.alternate_text.empty() ? 0.0f : gap + painter_.MeasureButtonWidth(spec_.alternate_text)));
        RECT work{};
        MONITORINFO monitor{sizeof(monitor)};
        if (spec_.fit_to_work_area && GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            work = monitor.rcWork;
            width = std::min(width, static_cast<float>(work.right - work.left - 24));
        }
        const float buttons_width = cancel_w + gap + confirm_w +
            (spec_.alternate_text.empty() ? 0.0f : gap + painter_.MeasureButtonWidth(spec_.alternate_text));
        const int rows = buttons_width > width - 2.0f * pad ? (spec_.alternate_text.empty() ? 2 : 3) : 1;
        const float footer_h = rows * btn_h + (rows - 1) * gap + ScaleDip(scale_, 20.0f);
        const float wrap = width - pad * 2.0f;
        const float msg_h = MeasureContent(wrap);
        message_height_ = msg_h;
        float height = title_h + ScaleDip(scale_, 16.0f) + msg_h
                     + ScaleDip(scale_, 16.0f) + footer_h
                     + (spec_.checkbox_text.empty() ? 0.0f : ScaleDip(scale_, 38.0f));
        if (work.bottom > work.top) height = std::min(height, static_cast<float>(work.bottom - work.top - 24));
        SetWindowPos(hwnd_, nullptr, 0, 0,
                     static_cast<int>(std::ceil(width)),
                     static_cast<int>(std::ceil(height)),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        compositor_.Resize(static_cast<int>(std::ceil(width)),
                           static_cast<int>(std::ceil(height)));
        LayoutFromSize(width, height);
    }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 3;
        if (pulse::ui::ContainsRect(ConfirmRect(), x, y)) return 1;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 2;
        if (!spec_.alternate_text.empty() && pulse::ui::ContainsRect(AlternateRect(), x, y)) return 5;
        if (!spec_.checkbox_text.empty() && pulse::ui::ContainsRect(check_rc_, x, y)) return 4;
        return 0;
    }

    void Complete(bool accepted, bool alternate = false) {
        accepted_ = accepted && (!spec_.still_valid || spec_.still_valid());
        if (accepted_ && spec_.checkbox_checked) *spec_.checkbox_checked = checked_;
        if (accepted_ && spec_.alternate_selected) *spec_.alternate_selected = alternate;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);

        const D2D1_COLOR_F warning = dark_ ? HexColor(0xF7D154) : HexColor(0x8A5500);
        painter_.DrawGlyph(L"\xE7BA", Rect(scale_, 14, 8, 22, 22), warning);
        painter_.DrawText(spec_.title, D2D1::RectF(ScaleDip(scale_, 42.0f), 0,
                                                  close_rc_.left - ScaleDip(scale_, 8.0f),
                                                  ScaleDip(scale_, 36.0f)),
                          compositor_.SmallFormat(), theme.text);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 3;
        close_state.pressed = pressed_ == 3;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close,
                                    {}, close_state);
        painter_.FillRoundedRect(D2D1::RectF(0, divider_top_,
                                             ScaleDip(scale_, dip_w_), divider_top_ + 1.0f),
                                 0, theme.stroke_divider);

        dc_scroll_max_ = std::max(0.0f, message_height_ - (message_rc_.bottom - message_rc_.top));
        message_scroll_animation_.Tick(message_scroll_, dc_scroll_max_, AnimationTimeMs(), ScrollAnimationsEnabled());
        UpdateScrollTimer();
        compositor_.Dc()->PushAxisAlignedClip(message_rc_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float content_y = message_rc_.top - message_scroll_;
        if (body_height_ > 0.0f) {
            DrawWrappedText(compositor_, compositor_.TextFormat(),
                D2D1::RectF(message_rc_.left, content_y, message_rc_.right, content_y + body_height_),
                spec_.message, theme.text);
            content_y += body_height_;
        }
        if (!spec_.rows.empty() && body_height_ > 0.0f) content_y += ScaleDip(scale_, 12.0f);
        for (size_t index = 0; index < spec_.rows.size(); ++index) {
            const auto& row = spec_.rows[index];
            const float height = row_heights_[index];
            const auto bounds = D2D1::RectF(message_rc_.left, content_y, message_rc_.right, content_y + height);
            if (row.boxed) painter_.FillRoundedRect(bounds, ScaleDip(scale_, 4.0f), theme.fill_input);
            const float horizontal_padding = row.boxed ? ScaleDip(scale_, 12.0f) : 0.0f;
            const float vertical_padding = row.boxed ? ScaleDip(scale_, 6.0f) : 0.0f;
            const auto text_bounds = D2D1::RectF(bounds.left + horizontal_padding,
                bounds.top + vertical_padding, bounds.right - horizontal_padding,
                bounds.bottom - vertical_padding);
            if (row.is_path) {
                const auto fitted = FitPathMiddle(row.text, text_bounds.right - text_bounds.left,
                    [&](const std::wstring& value) { return typography::MeasureLine(&compositor_, compositor_.TextFormat(), value); });
                DrawEllipsizedText(compositor_, compositor_.TextFormat(), text_bounds, fitted, theme.text, theme.bg);
            } else DrawWrappedText(compositor_, compositor_.TextFormat(), text_bounds, row.text, theme.text);
            content_y += height + ScaleDip(scale_, 4.0f);
        }
        if (note_height_ > 0.0f) {
            content_y += ScaleDip(scale_, 12.0f);
            DrawWrappedText(compositor_, compositor_.SmallFormat(),
                D2D1::RectF(message_rc_.left, content_y, message_rc_.right, content_y + note_height_),
                spec_.note, theme.text_secondary);
        }
        compositor_.Dc()->PopAxisAlignedClip();
        if (dc_scroll_max_ > 0.0f) {
            fluent::ScrollbarSpec scrollbar;
            scrollbar.viewport = D2D1::RectF(message_rc_.right + ScaleDip(scale_, 3.0f), message_rc_.top,
                message_rc_.right + ScaleDip(scale_, 13.0f), message_rc_.bottom);
            scrollbar.viewport_extent = message_rc_.bottom - message_rc_.top;
            scrollbar.content_extent = message_height_;
            scrollbar.offset = message_scroll_;
            scrollbar.expand_progress = 0.0f;
            painter_.DrawScrollbar(scrollbar);
        }

        if (!spec_.checkbox_text.empty()) {
            fluent::ControlState check{};
            check.checked = checked_;
            check.hovered = hover_ == 4;
            check.focused = focus_ == 2;
            painter_.DrawCheckBox(check_rc_, spec_.checkbox_text, check);
        }

        painter_.FillRoundedRect(D2D1::RectF(0, divider_footer_,
                                             ScaleDip(scale_, dip_w_), divider_footer_ + 1.0f),
                                 0, theme.stroke_divider);

        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 2;
        cancel_state.pressed = pressed_ == 2;
        cancel_state.keyboard_focus = focus_ == 1;
        painter_.DrawButton({ CancelRect(), spec_.cancel_text, {},
                              fluent::ButtonKind::Transparent, cancel_state });

        if (!spec_.alternate_text.empty()) {
            fluent::ControlState alternate_state{};
            alternate_state.hovered = hover_ == 5;
            alternate_state.pressed = pressed_ == 5;
            alternate_state.keyboard_focus = focus_ == (spec_.checkbox_text.empty() ? 2 : 3);
            painter_.DrawButton({ AlternateRect(), spec_.alternate_text, {},
                fluent::ButtonKind::Standard, alternate_state });
        }

        fluent::ControlState confirm_state{};
        confirm_state.hovered = hover_ == 1;
        confirm_state.pressed = pressed_ == 1;
        confirm_state.keyboard_focus = focus_ == 0;
        painter_.DrawButton({ ConfirmRect(), spec_.confirm_text, {},
                              spec_.danger ? fluent::ButtonKind::Danger
                                           : fluent::ButtonKind::Primary,
                              confirm_state });

        pulse::ui::EndSurface(compositor_);
    }

    bool ScrollAnimationsEnabled() const {
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        return enabled && !IsHighContrast() && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
    }

    void UpdateScrollTimer() {
        if (message_scroll_animation_.Pending()) {
            if (!SetTimer(hwnd_, kScrollTimer, 15, nullptr))
                message_scroll_animation_.Tick(message_scroll_, dc_scroll_max_, AnimationTimeMs(), false);
        } else KillTimer(hwnd_, kScrollTimer);
    }

    void ScrollMessage(float delta, bool animate = true) {
        message_scroll_animation_.ScrollBy(message_scroll_, delta, dc_scroll_max_,
            message_rc_.bottom - message_rc_.top, AnimationTimeMs(), animate && ScrollAnimationsEnabled());
        UpdateScrollTimer();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            SizeToContent();
            return 0;
#ifdef PULSE_UI_TEST_HOOKS
        case kConfirmSnapshotMessage:
            Render();
            return lparam && bench::SaveSnapshot(compositor_, reinterpret_cast<const wchar_t*>(lparam));
        case kConfirmInspectMessage:
            if (lparam) {
                Render();
                auto& info = *reinterpret_cast<ConfirmDialogInspection*>(lparam);
                info = {spec_.rows.size(), message_scroll_, dc_scroll_max_, message_rc_,
                    confirm_rc_, cancel_rc_, alternate_rc_};
                return TRUE;
            }
            return FALSE;
#endif
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return pulse::ui::BorderlessHitTest(hwnd_, lparam, 36 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            LayoutFromSize(static_cast<float>(LOWORD(lparam)),
                           static_cast<float>(HIWORD(lparam)));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            if (suggested) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            SizeToContent();
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed) {
                if (hit == 1) Complete(true);
                else if (hit == 5) Complete(true, true);
                else if (hit == 2 || hit == 3) Complete(false);
                else if (hit == 4) { checked_ = !checked_; focus_ = 2; }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            ScrollMessage(-GET_WHEEL_DELTA_WPARAM(wparam) / 120.0f * 48.0f * scale_);
            return 0;
        case WM_TIMER:
            if (wparam == kScrollTimer) {
                message_scroll_animation_.Tick(message_scroll_, dc_scroll_max_, AnimationTimeMs(), ScrollAnimationsEnabled());
                UpdateScrollTimer();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (spec_.still_valid && !spec_.still_valid()) Complete(false);
            return 0;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            message_scroll_animation_.Tick(message_scroll_, dc_scroll_max_, AnimationTimeMs(), ScrollAnimationsEnabled());
            UpdateScrollTimer();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_SHOWWINDOW:
            if (!wparam) {
                message_scroll_animation_.Tick(message_scroll_, dc_scroll_max_, AnimationTimeMs(), false);
                KillTimer(hwnd_, kScrollTimer);
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { Complete(false); return 0; }
            if (wparam == VK_HOME || wparam == VK_END) {
                message_scroll_animation_.Reset();
                KillTimer(hwnd_, kScrollTimer);
                message_scroll_ = wparam == VK_HOME ? 0.0f : dc_scroll_max_;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_NEXT || wparam == VK_PRIOR || wparam == VK_DOWN || wparam == VK_UP) {
                const float direction = wparam == VK_NEXT || wparam == VK_DOWN ? 1.0f : -1.0f;
                ScrollMessage(direction * 80 * scale_);
                return 0;
            }
            if (wparam == VK_TAB) {
                const int count = (spec_.checkbox_text.empty() ? 2 : 3) + (spec_.alternate_text.empty() ? 0 : 1);
                focus_ = (focus_ + (GetKeyState(VK_SHIFT) < 0 ? count - 1 : 1)) % count;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (!spec_.alternate_text.empty() && focus_ == (spec_.checkbox_text.empty() ? 2 : 3)) Complete(true, true);
                else if (!spec_.checkbox_text.empty() && focus_ == 2) { checked_ = !checked_; InvalidateRect(hwnd_, nullptr, FALSE); }
                else Complete(focus_ == 0);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(hwnd_, kScrollTimer);
            message_scroll_animation_.Reset();
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    ConfirmDialogSpec spec_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool accepted_ = false;
    bool checked_ = false;
    bool done_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
    D2D1_RECT_F close_rc_{};
    D2D1_RECT_F cancel_rc_{};
    D2D1_RECT_F confirm_rc_{};
    D2D1_RECT_F alternate_rc_{};
    D2D1_RECT_F message_rc_{};
    D2D1_RECT_F check_rc_{};
    float dip_w_ = kConfirmMinW;
    float message_height_ = 0;
    float body_height_ = 0;
    float note_height_ = 0;
    std::vector<float> row_heights_;
    SmoothScroll message_scroll_animation_;
    float message_scroll_ = 0;
    float dc_scroll_max_ = 0;
    float divider_top_ = 36.0f;
    float divider_footer_ = 120.0f;
};

} // namespace

FileOperationWindow::FileOperationWindow() : painter_(&compositor_) {}
FileOperationWindow::~FileOperationWindow() { Destroy(); }

bool FileOperationWindow::Create(HWND owner, FileOperationCallbacks callbacks,
                                 FileOperationWindowOptions options) {
    if (hwnd_) return true;
    owner_ = owner;
    callbacks_ = std::move(callbacks);
    options_ = options;
    dark_ = ShouldUseDarkMode(ThemeMode::Auto);
    accent_ = GetAccentColor();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kTransferClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!GetClassInfoExW(wc.hInstance, kTransferClass, &wc)) RegisterClassExW(&wc);
    const UINT owner_dpi = pulse::compat::WindowDpi(owner_);
    const int width = MulDiv(static_cast<int>(kDlgW), owner_dpi, 96);
    const int height = MulDiv(static_cast<int>(kCollapsedH), owner_dpi, 96);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kTransferClass, l10n::Get(l10n::StringId::OpWindow).c_str(),
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner_, nullptr,
        wc.hInstance, this);
    return hwnd_ != nullptr;
}

void FileOperationWindow::Destroy() {
    if (hwnd_) { HideComposedDialog(hwnd_, options_.modal_owner ? owner_ : nullptr); DestroyWindow(hwnd_); }
    hwnd_ = nullptr;
}

void FileOperationWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FileOperationWindow::Update(const ops::OpStatus& status) {
    const bool new_task = status.task_id != status_.task_id;
    auto shown = ops::PresentOperationStatus(status);
    const bool presentation_changed = !SameTransferPresentation(shown, status_);
    const bool title_changed = shown.summary != status_.summary;
    const bool entering_failure = shown.phase == ops::OpPhase::Failed &&
        (new_task || status_.phase != ops::OpPhase::Failed);
    const bool detail_changed = shown.phase != status_.phase ||
        (shown.phase == ops::OpPhase::Failed && (shown.last_error != status_.last_error ||
            shown.failed_path != status_.failed_path));
    if (new_task || shown.phase != ops::OpPhase::Completed) completion_reveal_.Reset();
    status_ = std::move(shown);
    if (new_task || detail_changed) {
        failure_scroll_ = failure_scroll_max_ = 0.0f;
        failure_scroll_animation_.Reset();
        if (hwnd_) KillTimer(hwnd_, kScrollTimer);
    }
    if (entering_failure) detailed_ = true;
    if (new_task) {
        cancel_requested_ = false;
        speed_history_.clear();
        speed_sample_tick_ = 0;
    }
    if (hwnd_) {
        if (options_.use_summary_title && title_changed) SetWindowTextW(hwnd_, status_.summary.c_str());
        if (entering_failure || detail_changed) ResizeForDetails(false);
        if (presentation_changed) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FileOperationWindow::Show(bool activate) {
    if (!hwnd_) return;
    if (!positioned_) {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        pulse::ui::CenterOwnedWindow(hwnd_, owner_, rect.right - rect.left, rect.bottom - rect.top);
        positioned_ = true;
    }
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                 (activate ? 0 : SWP_NOACTIVATE));
    ShowDialogWithFade(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(hwnd_);
}

void FileOperationWindow::Hide() {
    completion_reveal_.Sample(AnimationTimeMs(), false);
    failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(), false);
    if (hwnd_) KillTimer(hwnd_, kScrollTimer);
    if (hwnd_) HideComposedDialog(hwnd_, options_.modal_owner ? owner_ : nullptr);
    options_.modal_owner = false;
}

bool FileOperationWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void FileOperationWindow::ApplyWindowTheme() {
    backdrop_enabled_ = pulse::ui::ApplyBackdrop(hwnd_, dark_);
}

float FileOperationWindow::DetailBlockHeight() const {
    const float minimum = kGraphBlockH * scale_;
    if (status_.phase != ops::OpPhase::Failed) return minimum;
    const float width = std::max(1.0f, static_cast<float>(compositor_.Width()) - 64.0f * scale_);
    return std::max(minimum, painter_.MeasureWrappedCaptionHeight(
        ops::OperationFailureDetails(status_), width) + 52.0f * scale_);
}

void FileOperationWindow::ResizeForDetails(bool preserve_center) {
    if (!hwnd_) return;
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    const int width = rect.right - rect.left;
    const int old_height = rect.bottom - rect.top;
    int height = static_cast<int>(std::ceil(kCollapsedH * scale_ +
        (detailed_ ? DetailBlockHeight() : 0.0f)));
    MONITORINFO monitor{sizeof(monitor)};
    const bool fit_failure = detailed_ && status_.phase == ops::OpPhase::Failed &&
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
    if (fit_failure)
        height = std::min(height, static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 24));
    int y = preserve_center ? rect.top - (height - old_height) / 2 : rect.top;
    if (fit_failure) y = std::clamp(y, static_cast<int>(monitor.rcWork.top + 12),
        static_cast<int>(monitor.rcWork.bottom - height - 12));
    if (height == old_height && y == rect.top) return;
    SetWindowPos(hwnd_, nullptr, rect.left, y, width, height,
                 SWP_NOZORDER | SWP_NOOWNERZORDER);
}

int FileOperationWindow::HitTestControl(float x, float y) const {
    const auto labels = MakeTransferLabels(status_, detailed_, callbacks_);
    const auto chrome = MakeTransferChrome(scale_,
        static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()),
        painter_, labels);
    if (pulse::ui::ContainsRect(chrome.close, x, y)) return 1;
    if (pulse::ui::ContainsRect(chrome.pause, x, y)) return 3;
    if (pulse::ui::ContainsRect(chrome.cancel, x, y)) return 4;
    if (pulse::ui::ContainsRect(chrome.details, x, y)) return 5;
    return 0;
}

void FileOperationWindow::RequestClose() {
    if (!IsVisible()) return;
    if (callbacks_.dismiss) callbacks_.dismiss();
    Hide();
}

void FileOperationWindow::RequestCancel() {
    if (status_.phase == ops::OpPhase::Cancelling || (cancel_requested_ && status_.active)) return;
    const bool emptying = status_.type == ops::OpType::EmptyRecycle;
    if (status_.active && status_.phase != ops::OpPhase::Completed &&
        status_.phase != ops::OpPhase::Failed && !emptying) {
        if (callbacks_.cancel) {
            cancel_requested_ = true;
            callbacks_.cancel();
        }
        return;
    }
    RequestClose();
}

void FileOperationWindow::RefreshAnimationSettings() {
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    animations_enabled_ = animations && !IsHighContrast();
    completion_reveal_.Sample(AnimationTimeMs(), animations_enabled_);
    failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(), animations_enabled_);
    UpdateFailureScrollTimer();
}

bool FileOperationWindow::NeedsAnimationFrame() const {
    if (!animations_enabled_ || !IsVisible() || IsIconic(hwnd_)) return false;
    return completion_reveal_.pending || (TransferStatisticsActive(status_) && status_.percent < 0.0f &&
        (status_.phase == ops::OpPhase::Scanning || status_.type == ops::OpType::EmptyRecycle));
}

void FileOperationWindow::Render(bool present) {
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
    }
    if (!compositor_.Dc()) return;
    const bool high_contrast = IsHighContrast();
    const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    pulse::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                 scale_);
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float dip_w = width / std::max(scale_, 0.001f);
    const auto labels = MakeTransferLabels(status_, detailed_, callbacks_);
    const auto chrome = MakeTransferChrome(scale_, width, height, painter_, labels);
    const bool failed = status_.phase == ops::OpPhase::Failed;
    const bool cancelled = status_.phase == ops::OpPhase::Cancelled;
    const bool cancelling = status_.phase == ops::OpPhase::Cancelling;
    const bool paused = status_.phase == ops::OpPhase::Paused;
    const bool waiting = status_.phase == ops::OpPhase::WaitingForConflict;
    const bool scanning = status_.phase == ops::OpPhase::Scanning;
    const bool completed = status_.phase == ops::OpPhase::Completed;
    completion_reveal_.Tick(completed, AnimationTimeMs(),
        animations_enabled_ && IsVisible() && !IsIconic(hwnd_));
    const bool moving = status_.type == ops::OpType::Move;
    const bool deleting = status_.type == ops::OpType::RecycleDelete
                       || status_.type == ops::OpType::RealDelete;
    const bool restoring = status_.type == ops::OpType::RestoreRecycle;
    const bool emptying = status_.type == ops::OpType::EmptyRecycle;
    const D2D1_COLOR_F sky = dark_ ? HexColor(0x38BDF8) : HexColor(0x0284C7);

    const wchar_t* operation_glyph = (deleting || emptying) ? L"\xE74D"
        : restoring ? L"\xE777" : (moving ? L"\xE7C2" : L"\xE8C8");
    painter_.DrawGlyph(operation_glyph, Rect(scale_, 14, 10, 16, 16), sky);
    std::wstring title;
    if (options_.use_summary_title || cancelled) title = status_.summary;
    else if (emptying) {
        if (failed) title = l10n::Get(l10n::StringId::OpCannotEmpty);
        else if (completed) title = l10n::Get(l10n::StringId::OpEmptied);
        else title = l10n::Get(l10n::StringId::OpEmptying);
    } else if (failed) {
        title = deleting ? l10n::Get(l10n::StringId::OpDeleteFailed).c_str() : restoring ? l10n::Get(l10n::StringId::OpRestoreFailed).c_str()
            : moving ? l10n::Get(l10n::StringId::OpMoveFailed).c_str() : l10n::Get(l10n::StringId::OpCopyFailed).c_str();
    } else if (completed) {
        title = (deleting ? l10n::Get(l10n::StringId::OpDeletedPrefix).c_str() : restoring ? l10n::Get(l10n::StringId::OpRestoredPrefix).c_str() : moving ? l10n::Get(l10n::StringId::OpMovedPrefix).c_str() : l10n::Get(l10n::StringId::OpCopiedPrefix).c_str())
              + std::to_wstring(status_.completed_items) + l10n::Get(l10n::StringId::OpItemsSuffix).c_str();
    } else if (paused) {
        title = deleting ? l10n::Get(l10n::StringId::OpDeletePaused).c_str() : restoring ? l10n::Get(l10n::StringId::OpRestorePaused).c_str()
            : moving ? l10n::Get(l10n::StringId::OpMovePaused).c_str() : l10n::Get(l10n::StringId::OpCopyPaused).c_str();
    } else if (scanning) title = l10n::Get(l10n::StringId::OpPreparingList).c_str();
    else {
        const uint64_t count = status_.total_items > 0 ? status_.total_items
            : (std::max)(status_.completed_items, static_cast<uint64_t>(1));
        title = (deleting ? l10n::Get(l10n::StringId::OpDeletingPrefix).c_str() : restoring ? l10n::Get(l10n::StringId::OpRestoringPrefix).c_str() : moving ? l10n::Get(l10n::StringId::OpMovingPrefix).c_str() : l10n::Get(l10n::StringId::OpCopyingPrefix).c_str())
              + std::to_wstring(count) + l10n::Get(l10n::StringId::OpItemsSuffix).c_str();
    }
    painter_.DrawText(title, D2D1::RectF(ScaleDip(scale_, 36.0f), 0,
                                         chrome.close.left - ScaleDip(scale_, 8.0f),
                                         ScaleDip(scale_, kTitleH)),
                      compositor_.SmallFormat(), theme.text);

    fluent::ControlState close{};
    close.hovered = hover_ == 1;
    close.pressed = pressed_ == 1;
    painter_.DrawTitleBarButton(chrome.close, fluent::TitleBarButtonRole::Close, {}, close);
    if (GetFocus() == hwnd_ && focus_ == 1) painter_.DrawFocusRing(chrome.close, 4.0f * scale_);
    painter_.FillRoundedRect(D2D1::RectF(0, ScaleDip(scale_, kTitleH), width,
                                         ScaleDip(scale_, kTitleH) + 1.0f), 0, theme.stroke_divider);

    const std::wstring src = failed && !status_.failed_path.empty() ? path::FriendlyPathText(status_.failed_path) :
        status_.source_label.empty() ? l10n::Get(l10n::StringId::OpFilePath) : status_.source_label;
    const std::wstring dst = status_.destination_label.empty() ? l10n::Get(l10n::StringId::OpDestination).c_str() : status_.destination_label;
    std::wstring subtitle;
    if (options_.use_summary_title) {
        subtitle = l10n::Get(l10n::StringId::OpFromPrefix) + src;
        if (!status_.destination_label.empty()) subtitle += L" → " + dst;
    } else if (completed || failed || cancelled) {
        subtitle = emptying ? title : src;
        if (!emptying && !status_.destination_label.empty()) subtitle += L" → " + dst;
    }
    else if (emptying) subtitle = l10n::Get(l10n::StringId::OpEmptyingSub);
    else if (deleting) subtitle = l10n::Get(l10n::StringId::OpDeletingPrefix).c_str() + src;
    else if (restoring) subtitle = l10n::Get(l10n::StringId::OpRestoringPrefix).c_str() + src;
    else if (moving) subtitle = l10n::Get(l10n::StringId::OpFromPrefix).c_str() + src + l10n::Get(l10n::StringId::OpMoveTo).c_str() + dst;
    else subtitle = l10n::Get(l10n::StringId::OpFromPrefix).c_str() + src + l10n::Get(l10n::StringId::OpCopyTo).c_str() + dst;
    if (deleting) subtitle = FitPathMiddle(subtitle, (dip_w - kPadX * 2.0f - 72.0f) * scale_,
        [&](const std::wstring& value) {
            return typography::MeasureLine(&compositor_, compositor_.SmallFormat(), value);
        });
    DrawEllipsizedText(compositor_, compositor_.SmallFormat(),
        Rect(scale_, kPadX, 48, dip_w - kPadX * 2.0f - 72.0f, 18), subtitle, theme.text, theme.bg);
    wchar_t percent[32];
    if (status_.percent < 0.0f) percent[0] = 0;
    else swprintf_s(percent, L"%.0f%%", std::clamp(status_.percent, 0.0f, 100.0f));
    painter_.DrawText(percent, Rect(scale_, dip_w - kPadX - 56.0f, 46, 56, 20),
                      compositor_.TextFormat(), failed ? theme.danger : sky,
                      fluent::HorizontalAlignment::Right);

    std::wstring file_line;
    std::wstring badge_text;
    fluent::BadgeKind badge_kind = fluent::BadgeKind::Success;
    if (cancelled) {
        file_line = status_.summary;
        badge_text = l10n::Get(l10n::StringId::Cancel);
        badge_kind = fluent::BadgeKind::Neutral;
    } else if (failed) {
        file_line = status_.last_error.empty() ? l10n::Get(l10n::StringId::OpFailed).c_str() : status_.last_error;
        badge_text = l10n::Get(l10n::StringId::OpFailedBadge).c_str();
        badge_kind = fluent::BadgeKind::Danger;
    } else if (paused) {
        file_line = status_.current_item.empty() ? src : l10n::Get(l10n::StringId::OpItemPrefix) + status_.current_item;
        badge_text = l10n::Get(l10n::StringId::OpPaused).c_str();
        badge_kind = fluent::BadgeKind::Warning;
    } else if (waiting) {
        file_line = status_.current_item.empty() ? src : l10n::Get(l10n::StringId::OpItemPrefix) + status_.current_item;
        badge_text = l10n::Get(l10n::StringId::OpWaitingConflict).c_str();
        badge_kind = fluent::BadgeKind::Warning;
    } else if (completed) {
        file_line = title;
        badge_text = l10n::Get(l10n::StringId::OpCompleted).c_str();
        badge_kind = fluent::BadgeKind::Success;
    } else if (scanning) {
        file_line = l10n::Get(l10n::StringId::OpScanning);
        badge_text = l10n::Get(l10n::StringId::OpPreparing).c_str();
        badge_kind = fluent::BadgeKind::Neutral;
    } else if (emptying) {
        file_line = l10n::Get(l10n::StringId::OpEmptyingSub);
        badge_text = l10n::Get(l10n::StringId::OpEmptying);
        badge_kind = fluent::BadgeKind::Accent;
    } else {
        file_line = status_.current_item.empty() ? src : l10n::Get(l10n::StringId::OpItemPrefix) + status_.current_item;
        badge_text = options_.use_summary_title ? status_.summary : deleting ? l10n::Get(l10n::StringId::OpDeleting).c_str() : moving ? l10n::Get(l10n::StringId::OpMoving).c_str() : l10n::Get(l10n::StringId::OpCopying).c_str();
        badge_kind = fluent::BadgeKind::Accent;
    }
    const float badge_w = std::max(48.0f, painter_.MeasureBadgeWidth(badge_text) / std::max(scale_, 0.001f));
    const bool fading_completion = completed && completion_reveal_.opacity < 1.0f;
    if (fading_completion) compositor_.Dc()->PushLayer(D2D1::LayerParameters(
        Rect(scale_, kPadX, 70, dip_w - kPadX * 2.0f, 20), nullptr,
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(),
        completion_reveal_.opacity), nullptr);
    DrawEllipsizedText(compositor_, compositor_.SmallFormat(),
        Rect(scale_, kPadX, 70, dip_w - kPadX * 2.0f - badge_w - 12.0f, 20),
        file_line, theme.text, theme.bg);
    fluent::BadgeSpec badge{ Rect(scale_, dip_w - kPadX - badge_w, 70, badge_w, 18),
                            badge_text, badge_kind };
    if (badge_kind == fluent::BadgeKind::Accent && !high_contrast) {
        badge.use_custom_colors = true;
        badge.custom_background = dark_ ? HexColor(0x12344A) : HexColor(0xE1F0FC);
        badge.custom_foreground = sky;
    }
    painter_.DrawBadge(badge);
    if (fading_completion) compositor_.Dc()->PopLayer();

    const auto track = Rect(scale_, kPadX, 98, dip_w - kPadX * 2.0f, 10);
    painter_.FillRoundedRect(track, ScaleDip(scale_, 5.0f),
        dark_ ? HexColor(0xFFFFFF, 0.10f) : HexColor(0x000000, 0.10f));
    const bool indeterminate = (emptying || scanning) && status_.active && !failed && !completed
        && status_.percent < 0.0f;
    if (indeterminate) {
        fluent::ProgressSpec bar;
        bar.bounds = track;
        bar.indeterminate = true;
        const double now_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        bar.animation_progress = NeedsAnimationFrame()
            ? static_cast<float>(std::fmod(now_ms, 1952.0) / 1952.0) : 0.25f;
        painter_.DrawProgressBar(bar);
    } else {
        const float inner = ScaleDip(scale_, 2.0f);
        D2D1_RECT_F fill = D2D1::RectF(track.left + inner, track.top + inner,
                                       track.right - inner, track.bottom - inner);
        const float value = status_.percent < 0.0f ? 0.0f
            : std::clamp(status_.percent / 100.0f, 0.0f, 1.0f);
        fill.right = fill.left + (fill.right - fill.left) * value;
        D2D1_COLOR_F bar = failed ? theme.danger
            : (paused || waiting) ? HexColor(0xF59E0B)
            : HexColor(0x0EA5E9);
        if (fill.right > fill.left) {
            painter_.FillRoundedRect(fill, ScaleDip(scale_, 3.0f), bar);
        }
    }

    const auto stats = Rect(scale_, kPadX, 118, dip_w - kPadX * 2.0f, 52);
    painter_.FillRoundedRect(stats, ScaleDip(scale_, 12.0f),
        dark_ ? HexColor(0xFFFFFF, 0.05f) : HexColor(0x000000, 0.05f));
    const bool byte_transfer = status_.type == ops::OpType::Copy
        || status_.type == ops::OpType::Move;
    const std::wstring time_text = failed ? l10n::Get(l10n::StringId::OpIncomplete).c_str()
        : cancelled ? l10n::Get(l10n::StringId::Cancel).c_str()
        : cancelling ? l10n::Get(l10n::StringId::OpNoEstimate).c_str()
        : completed ? l10n::Get(l10n::StringId::OpCompleted).c_str()
        : paused ? l10n::Get(l10n::StringId::OpPaused).c_str()
        : waiting ? l10n::Get(l10n::StringId::OpWaiting).c_str()
        : emptying ? l10n::Get(l10n::StringId::OpEmptying)
        : !byte_transfer ? l10n::Get(l10n::StringId::OpNoEstimate).c_str()
        : status_.eta_seconds == 0 ? l10n::Get(l10n::StringId::OpEstimating).c_str()
        : l10n::Get(l10n::StringId::OpApprox).c_str() + FormatDuration(status_.eta_seconds);
    const uint64_t remain_items = status_.total_items > status_.completed_items
        ? status_.total_items - status_.completed_items : 0;
    const uint64_t remain_bytes = status_.total_bytes > status_.transferred_bytes
        ? status_.total_bytes - status_.transferred_bytes : 0;
    std::wstring items_text;
    if (!options_.item_progress_known) {
        items_text = failed && status_.total_bytes == 0 ? L"—"
            : status_.total_bytes == 0 && !completed ? l10n::Get(l10n::StringId::OpCalculating)
            : (options_.estimated_bytes ? l10n::Get(l10n::StringId::OpApprox) : L"")
                + pulse::format::ByteSize(remain_bytes);
    } else if (completed) items_text = l10n::Get(l10n::StringId::OpZeroItems).c_str();
    else if (failed && status_.total_items == 0) items_text = L"—";
    else if (status_.total_items == 0) items_text = l10n::Get(l10n::StringId::OpCalculating).c_str();
    else if (byte_transfer && status_.total_bytes > 0)
        items_text = std::to_wstring(remain_items) + l10n::Get(l10n::StringId::OpCountBytes).c_str()
            + pulse::format::ByteSize(remain_bytes) + L")";
    else items_text = std::to_wstring(remain_items) + l10n::Get(l10n::StringId::OpCountSuffix).c_str();
    const std::wstring speed_text = emptying || !byte_transfer ? L"—"
        : completed || failed || cancelled || cancelling || paused || waiting ? L"0 B/s"
        : status_.bytes_per_second <= 0.0 ? l10n::Get(l10n::StringId::OpEstimating).c_str()
        : (options_.estimated_bytes ? l10n::Get(l10n::StringId::OpApprox) : L"")
            + pulse::format::ByteSize(static_cast<uint64_t>(status_.bytes_per_second)) + L"/s";
    const auto remaining_label = l10n::Get(options_.item_progress_known
        ? l10n::StringId::OpRemainingItems : l10n::StringId::OpRemainingSize);
    const D2D1_COLOR_F time_color = failed ? theme.danger : theme.text;
    painter_.DrawText(l10n::Get(l10n::StringId::OpRemainingTime).c_str(), Rect(scale_, 32, 124, 120, 14),
                      compositor_.SmallFormat(), theme.text_secondary);
    painter_.DrawText(remaining_label, Rect(scale_, 176, 124, 140, 14),
                      compositor_.SmallFormat(), theme.text_secondary);
    painter_.DrawText(l10n::Get(l10n::StringId::OpCurrentSpeed).c_str(), Rect(scale_, 328, 124, 110, 14),
                      compositor_.SmallFormat(), theme.text_secondary);
    painter_.DrawText(time_text, Rect(scale_, 32, 140, 136, 22),
                      compositor_.SmallFormat(), time_color);
    painter_.DrawText(items_text, Rect(scale_, 176, 140, 148, 22),
                      compositor_.SmallFormat(), theme.text);
    painter_.DrawText(speed_text, Rect(scale_, 328, 140, 112, 22),
                      compositor_.SmallFormat(), sky);

    if (detailed_ && failed) {
        const auto box = Rect(scale_, kPadX, 180, dip_w - kPadX * 2.0f,
            std::max(kGraphBlockH, (height - kFooterH * scale_) / scale_ - 192.0f));
        painter_.FillRoundedRect(box, ScaleDip(scale_, 12.0f),
            dark_ ? HexColor(0xFFFFFF, 0.04f) : HexColor(0x000000, 0.03f));
        painter_.DrawText(l10n::Get(l10n::StringId::OpErrorDetails), Rect(scale_, 32, 188, dip_w - 64, 20),
                          compositor_.SmallFormat(), theme.text);
        const auto text_bounds = D2D1::RectF(32 * scale_, 216 * scale_,
            width - 32 * scale_, height - 64 * scale_);
        const auto text = ops::OperationFailureDetails(status_);
        const float text_height = painter_.MeasureWrappedCaptionHeight(text, text_bounds.right - text_bounds.left);
        failure_viewport_ = std::max(0.0f, text_bounds.bottom - text_bounds.top);
        failure_scroll_max_ = std::max(0.0f, text_height - failure_viewport_);
        failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(),
            animations_enabled_ && IsVisible() && !IsIconic(hwnd_));
        UpdateFailureScrollTimer();
        compositor_.Dc()->PushAxisAlignedClip(text_bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        painter_.DrawWrappedCaption(text, D2D1::Point2F(text_bounds.left, text_bounds.top - failure_scroll_),
            text_bounds.right - text_bounds.left, theme.text_secondary);
        compositor_.Dc()->PopAxisAlignedClip();
        if (failure_scroll_max_ > 0.0f) {
            fluent::ScrollbarSpec scrollbar;
            scrollbar.viewport = D2D1::RectF(text_bounds.right + 3 * scale_, text_bounds.top,
                text_bounds.right + 13 * scale_, text_bounds.bottom);
            scrollbar.viewport_extent = text_bounds.bottom - text_bounds.top;
            scrollbar.content_extent = text_height;
            scrollbar.offset = failure_scroll_;
            painter_.DrawScrollbar(scrollbar);
        }
    } else if (detailed_) {
        const auto card = Rect(scale_, kPadX, 180, dip_w - kPadX * 2.0f, 140);
        painter_.FillRoundedRect(card, ScaleDip(scale_, 12.0f),
            dark_ ? HexColor(0x000000, 0.30f) : HexColor(0x000000, 0.05f));
        painter_.StrokeRoundedRect(card, ScaleDip(scale_, 12.0f), theme.stroke_card);
        painter_.DrawGlyph(L"\xE8F1", Rect(scale_, 30, 188, 14, 14), sky);
        painter_.DrawText(l10n::Get(l10n::StringId::OpSpeedHistory).c_str(), Rect(scale_, 48, 186, 220, 18),
                          compositor_.SmallFormat(), theme.text_secondary);
        const std::wstring peak = l10n::Get(l10n::StringId::OpPeak)
            + (options_.estimated_bytes ? l10n::Get(l10n::StringId::OpApprox) : L"") + pulse::format::ByteSize(
            static_cast<uint64_t>(status_.peak_bytes_per_second)) + L"/s";
        painter_.DrawText(peak, Rect(scale_, 280, 186, 156, 18),
                          compositor_.SmallFormat(), theme.text_secondary,
                          fluent::HorizontalAlignment::Right);
        const auto plot = Rect(scale_, 30, 208, dip_w - kPadX * 2.0f - 20.0f, 100);
        painter_.FillRoundedRect(plot, ScaleDip(scale_, 8.0f),
            dark_ ? HexColor(0x0F172A, 0.60f) : HexColor(0xFFFFFF, 0.50f));
        DrawSpeedGraph(compositor_.Dc(), plot, speed_history_, sky, scale_);
#ifdef PULSE_UI_TEST_HOOKS
        progress_inspection_.peak = peak;
#endif
    }

    painter_.FillRoundedRect(D2D1::RectF(0, height - ScaleDip(scale_, kFooterH), width,
                                         height - ScaleDip(scale_, kFooterH) + 1.0f),
                             0, theme.stroke_divider);
    const auto footer_bg = D2D1::RectF(0, height - ScaleDip(scale_, kFooterH) + 1.0f,
                                       width, height);
    painter_.FillRoundedRect(footer_bg, 0,
        dark_ ? HexColor(0x202022, 0.92f) : HexColor(0x000000, 0.04f));

    fluent::ControlState detail_state{};
    detail_state.hovered = hover_ == 5;
    detail_state.pressed = pressed_ == 5;
    detail_state.keyboard_focus = GetFocus() == hwnd_ && focus_ == 5;
    painter_.DrawButton({ chrome.details, labels.details,
        detailed_ ? L"\xE70E" : L"\xE70D", fluent::ButtonKind::Transparent, detail_state });
    if (labels.show_pause) {
        fluent::ControlState pause_state{};
        pause_state.enabled = status_.active && !cancel_requested_ && !failed && status_.phase != ops::OpPhase::Cancelling
            && status_.phase != ops::OpPhase::WaitingForConflict;
        pause_state.hovered = hover_ == 3;
        pause_state.pressed = pressed_ == 3;
        pause_state.keyboard_focus = GetFocus() == hwnd_ && focus_ == 3;
        painter_.DrawButton({ chrome.pause, labels.pause, {},
                              fluent::ButtonKind::Standard, pause_state });
    }
    fluent::ControlState cancel_state{};
    cancel_state.enabled = status_.phase != ops::OpPhase::Cancelling && !(cancel_requested_ && status_.active);
    cancel_state.hovered = hover_ == 4;
    cancel_state.pressed = pressed_ == 4;
    cancel_state.keyboard_focus = GetFocus() == hwnd_ && focus_ == 4;
    painter_.DrawButton({ chrome.cancel, labels.cancel, {},
                          fluent::ButtonKind::Standard, cancel_state });
#ifdef PULSE_UI_TEST_HOOKS
    progress_inspection_.title = title;
    progress_inspection_.subtitle = subtitle;
    progress_inspection_.file_line = file_line;
    progress_inspection_.percent = percent;
    progress_inspection_.remaining_label = remaining_label;
    progress_inspection_.remaining = items_text;
    progress_inspection_.time = time_text;
    progress_inspection_.speed = speed_text;
    progress_inspection_.show_pause = labels.show_pause;
    progress_inspection_.indeterminate = indeterminate;
    progress_inspection_.details = chrome.details;
    progress_inspection_.pause = chrome.pause;
    progress_inspection_.cancel = chrome.cancel;
#endif
    if (present) pulse::ui::EndSurface(compositor_);
    else {
        const HRESULT hr = compositor_.Dc()->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
            compositor_.NotifyDeviceLost(hr);
    }
}

LRESULT CALLBACK FileOperationWindow::WndProc(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<FileOperationWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<FileOperationWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

void FileOperationWindow::UpdateFailureScrollTimer() {
    if (!hwnd_) return;
    if (failure_scroll_animation_.Pending()) {
        if (!SetTimer(hwnd_, kScrollTimer, 15, nullptr))
            failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(), false);
    } else KillTimer(hwnd_, kScrollTimer);
}

void FileOperationWindow::ScrollFailureDetails(float delta, bool animate) {
    failure_scroll_animation_.ScrollBy(failure_scroll_, delta, failure_scroll_max_, failure_viewport_,
        AnimationTimeMs(), animate && animations_enabled_ && IsVisible() && !IsIconic(hwnd_));
    UpdateFailureScrollTimer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool FileOperationWindow::ControlEnabled(int id) const {
    if (id == 1 || id == 5) return true;
    if (id == 4) return status_.phase != ops::OpPhase::Cancelling && !(cancel_requested_ && status_.active);
    const auto labels = MakeTransferLabels(status_, detailed_, callbacks_);
    return id == 3 && labels.show_pause && status_.active && !cancel_requested_ &&
        status_.phase != ops::OpPhase::Failed && status_.phase != ops::OpPhase::Cancelling &&
        status_.phase != ops::OpPhase::WaitingForConflict;
}

void FileOperationWindow::ActivateControl(int id) {
    if (!ControlEnabled(id)) return;
    if (id == 1) RequestClose();
    else if (id == 4) RequestCancel();
    else if (id == 3) {
        if (status_.phase == ops::OpPhase::Paused) {
            if (callbacks_.resume) callbacks_.resume();
        } else if (callbacks_.pause) callbacks_.pause();
    } else if (id == 5) {
        failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(), false);
        KillTimer(hwnd_, kScrollTimer);
        detailed_ = !detailed_;
        ResizeForDetails(true);
    }
}

LRESULT FileOperationWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
        RefreshAnimationSettings();
        ApplyWindowTheme();
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        SetTimer(hwnd_, kRenderTimer, 100, nullptr);
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const auto labels = MakeTransferLabels(status_, detailed_, callbacks_);
        const auto chrome = MakeTransferChrome(scale_,
            static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()),
            painter_, labels);
        return pulse::ui::BorderlessHitTest(hwnd_, lparam, ScaleDip(scale_, kTitleH),
            chrome.close);
    }
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) completion_reveal_.Sample(AnimationTimeMs(), false);
        failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(), false);
        KillTimer(hwnd_, kScrollTimer);
        if (compositor_.Dc() && wparam != SIZE_MINIMIZED)
            compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
        if (wparam != SIZE_MINIMIZED && detailed_ && status_.phase == ops::OpPhase::Failed &&
            static_cast<float>(HIWORD(lparam)) < kCollapsedH * scale_ + DetailBlockHeight())
            ResizeForDetails(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        RefreshAnimationSettings();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SHOWWINDOW:
        if (!wparam) {
            failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(), false);
            KillTimer(hwnd_, kScrollTimer);
        }
        break;
    case WM_MOVE:
        compositor_.UpdateTextRenderingParams(
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wparam) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kScrollTimer) {
            failure_scroll_animation_.Tick(failure_scroll_, failure_scroll_max_, AnimationTimeMs(),
                animations_enabled_ && IsVisible() && !IsIconic(hwnd_));
            UpdateFailureScrollTimer();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wparam == kRenderTimer && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
            bool dirty = NeedsAnimationFrame() || compositor_.NeedsRecovery();
            if (TransferStatisticsActive(status_)) {
                const ULONGLONG now = GetTickCount64();
                if (speed_sample_tick_ == 0 || now - speed_sample_tick_ >= 250) {
                    speed_sample_tick_ = now;
                    const bool running = status_.phase == ops::OpPhase::Running;
                    const bool byte_transfer = status_.type == ops::OpType::Copy
                        || status_.type == ops::OpType::Move;
                    speed_history_.push_back(running && byte_transfer
                        ? (std::max)(0.0, status_.bytes_per_second) : 0.0);
                    while (speed_history_.size() > 120) speed_history_.pop_front();
                    dirty |= detailed_;
                }
            }
            if (dirty) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        const int next = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                        static_cast<float>(GET_Y_LPARAM(lparam)));
        if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                  static_cast<float>(GET_Y_LPARAM(lparam)));
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                       static_cast<float>(GET_Y_LPARAM(lparam)));
        const int pressed = pressed_;
        pressed_ = 0;
        ReleaseCapture();
        if (hit == pressed) {
            ActivateControl(hit);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (detailed_ && status_.phase == ops::OpPhase::Failed) {
            ScrollFailureDetails(-GET_WHEEL_DELTA_WPARAM(wparam) / 120.0f * 48.0f * scale_);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wparam == VK_TAB) {
            std::vector<DialogFocusTarget> targets;
            for (int id : {5, 3, 4, 1}) if (ControlEnabled(id)) targets.push_back({id});
            MoveDialogFocus(hwnd_, focus_, targets, GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        if (wparam == VK_SPACE || wparam == VK_RETURN) {
            ActivateControl(focus_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wparam == VK_ESCAPE) {
            RequestClose();
            return 0;
        }
        if (detailed_ && status_.phase == ops::OpPhase::Failed) {
            if (wparam == VK_HOME || wparam == VK_END) {
                failure_scroll_animation_.Reset();
                ScrollFailureDetails(wparam == VK_HOME ? -failure_scroll_max_ : failure_scroll_max_, false);
                return 0;
            }
            if (wparam == VK_NEXT || wparam == VK_PRIOR || wparam == VK_DOWN || wparam == VK_UP) {
                ScrollFailureDetails((wparam == VK_NEXT || wparam == VK_DOWN ? 1.0f : -1.0f) * 80 * scale_);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        RequestClose();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        if (NeedsAnimationFrame() && compositor_.CanScheduleAnimationFrame() &&
            IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
            const HWND painted = hwnd_;
            // Materialize due timers before queuing another paint, so continuous
            // preparation animation cannot starve this UI thread's other timers.
            MSG timer{};
            PeekMessageW(&timer, nullptr, WM_TIMER, WM_TIMER, PM_NOREMOVE);
            if (IsWindow(painted) && reinterpret_cast<FileOperationWindow*>(
                    GetWindowLongPtrW(painted, GWLP_USERDATA)) == this)
                InvalidateRect(painted, nullptr, FALSE);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd_, kRenderTimer);
        KillTimer(hwnd_, kScrollTimer);
        failure_scroll_animation_.Reset();
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

ConfirmDialogSpec BuildLinkConfirmationSpec(const ops::ConflictItemInfo& conflict) {
    ConfirmDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::LinkDestinationTitle);
    spec.confirm_text = l10n::Get(l10n::StringId::LinkContinue);
    spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    spec.cancel_is_default = true;
    spec.fit_to_work_area = true;
    spec.still_valid = conflict.still_valid;
    const auto& impact = conflict.link_impact;
    switch (impact.kind) {
    case ops::DestinationLinkKind::DirectorySymbolicLink:
    case ops::DestinationLinkKind::Junction:
        spec.message = l10n::Get(l10n::StringId::LinkDirectoryMessage);
        break;
    case ops::DestinationLinkKind::FileSymbolicLink:
        spec.message = l10n::Get(l10n::StringId::LinkFileMessage);
        break;
    case ops::DestinationLinkKind::HardLink:
        spec.message = l10n::Get(l10n::StringId::LinkHardMessage);
        spec.message += L"\n" + std::to_wstring(impact.link_count) +
            l10n::Get(l10n::StringId::LinkNameCountSuffix);
        break;
    }
    spec.rows.push_back({l10n::Get(impact.kind == ops::DestinationLinkKind::HardLink
        ? l10n::StringId::PermissionOperationDestinationPath : l10n::StringId::LinkPath), false, false});
    spec.rows.push_back({path::FriendlyPathText(impact.path), true});
    if (!impact.resolved_path.empty()) {
        spec.rows.push_back({l10n::Get(l10n::StringId::LinkResolvedPath), false, false});
        spec.rows.push_back({path::FriendlyPathText(impact.resolved_path), true});
    }
    spec.note = l10n::Get(l10n::StringId::LinkTaskScopeNote);
    return spec;
}

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent) {
    if (conflict.link_confirmation) {
        const auto spec = BuildLinkConfirmationSpec(conflict);
        ConflictDialogResult result;
        ConfirmWindow window;
        if (window.Show(owner, spec, dark, accent)) {
            result.choice = ops::ConflictChoice::Continue;
            result.apply_to_all = true;
        }
        return result;
    }
    ConflictWindow window;
    return window.Show(owner, conflict, dark, accent);
}

bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent) {
    ConfirmWindow window;
    return window.Show(owner, spec, dark, accent);
}

bool ShowRenameLockedDialog(HWND owner, const ops::OpStatus& status, bool dark,
    D2D1_COLOR_F accent, std::function<bool()> still_valid) {
    if (still_valid && !still_valid()) return false;
    ConfirmDialogSpec spec;
    spec.title = l10n::Get(l10n::StringId::LockedItemTitle);
    spec.message = l10n::Get(l10n::StringId::LockedItemGenericMessage);
    const auto& file = status.failed_path.empty() ? status.locked_path : status.failed_path;
    if (!file.empty()) spec.rows.push_back({path::FriendlyPathText(file), true});
    spec.confirm_text = l10n::Get(l10n::StringId::LockedItemRetry);
    spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    spec.cancel_is_default = true;
    spec.fit_to_work_area = true;
    spec.still_valid = std::move(still_valid);
    return ShowConfirmDialog(owner, spec, dark, accent);
}

LockedItemChoice ShowLockedItemDialog(HWND owner, const ops::OpStatus& status,
    const LockedItemDialogText& text, bool dark, D2D1_COLOR_F accent,
    std::function<bool()> still_valid) {
    const bool plain_transfer_retry = status.lock_retry_available && !status.active &&
        status.phase == ops::OpPhase::Failed && status.task_id && still_valid &&
        (status.type == ops::OpType::Copy || status.type == ops::OpType::Move);
    if ((status.lock_owners.empty() && !plain_transfer_retry) || (still_valid && !still_valid()))
        return LockedItemChoice::Cancel;
    const bool can_end = !status.lock_owners.empty() &&
        std::all_of(status.lock_owners.begin(), status.lock_owners.end(),
        [](const auto& process) { return process.can_terminate; });
    const auto separator = status.locked_path.find_last_of(L"\\/");
    const auto name = status.locked_path.substr(separator == std::wstring::npos ? 0 : separator + 1);
    ConfirmDialogSpec spec;
    spec.title = text.title;
    spec.message = status.lock_owners.empty()
        ? l10n::Get(l10n::StringId::LockedItemGenericMessage) : text.message;
    const auto marker = spec.message.find(L"{name}");
    if (marker != std::wstring::npos) spec.message.replace(marker, 6, name);
    for (const auto& process : status.lock_owners) {
        std::wstring description = ops::LockOwnerDescription(process);
        if (!process.image_path.empty()) description += L"\n" + path::FriendlyPathText(process.image_path);
        spec.rows.push_back({std::move(description), false});
    }
    if (status.lock_owners.empty()) {
        const auto& file = status.failed_path.empty() ? status.locked_path : status.failed_path;
        if (!file.empty()) spec.rows.push_back({path::FriendlyPathText(file), true});
    } else spec.note = can_end ? text.end_hint : text.close_hint;
    spec.confirm_text = can_end ? text.end_retry : text.retry;
    spec.cancel_text = text.cancel;
    spec.cancel_is_default = true;
    spec.danger = can_end;
    spec.fit_to_work_area = true;
    spec.still_valid = std::move(still_valid);
    bool plain_retry = false;
    if (can_end) {
        spec.alternate_text = text.retry;
        spec.alternate_selected = &plain_retry;
    }
    if (!ShowConfirmDialog(owner, spec, dark, accent)) return LockedItemChoice::Cancel;
    return can_end && !plain_retry ? LockedItemChoice::EndAndRetry : LockedItemChoice::Retry;
}

} // namespace pulse::ui
