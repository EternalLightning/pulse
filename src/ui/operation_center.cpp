#include "operation_center.h"
#include "operation_center_layout.h"
#include "dialog_text_fit.h"
#include "typography.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace pulse::ui {
using operation_center_layout::kMargin;
using operation_center_layout::kHeader;
namespace {
constexpr wchar_t kClass[] = L"PulseOperationCenter";
constexpr UINT_PTR kTimer = 1;
constexpr float kContentInset = 16, kCardPadding = 12;
constexpr float kResizeDurationMs = 200;
bool Terminal(const ops::OpStatus& task) {
    return task.phase == ops::OpPhase::Completed || task.phase == ops::OpPhase::Failed ||
        task.phase == ops::OpPhase::Cancelled;
}
bool Transfer(const ops::OpStatus& task) {
    return task.type == ops::OpType::Copy || task.type == ops::OpType::Move;
}
bool ShowPhaseBadge(const ops::OpStatus& task) {
    return task.phase != ops::OpPhase::Running && task.phase != ops::OpPhase::Completed;
}
bool AbsoluteDestination(const ops::OpStatus& task) {
    if (task.type == ops::OpType::RecycleDelete || task.type == ops::OpType::RealDelete ||
        task.type == ops::OpType::EmptyRecycle || task.destination_label.empty()) return false;
    const auto& path = task.destination_label;
    // Syntactic validation only: filesystem resolution belongs to the callback,
    // where slow/disconnected destinations can be handled off the UI thread.
    return (path.size() >= 3 && iswalpha(path[0]) && path[1] == L':' && path[2] == L'\\') ||
        (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\' &&
         path.find(L'\\', 2) != std::wstring::npos && path.rfind(L"\\\\.\\", 0) != 0);
}
int Group(const ops::OpStatus& task) {
    return Terminal(task) ? 2 : task.phase == ops::OpPhase::Queued ? 1 : 0;
}
std::wstring SingleLine(std::wstring text) {
    for (auto& ch : text) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    return text;
}
std::wstring Statistics(const ops::OpStatus& task) {
    if (task.total_items) return std::to_wstring(task.completed_items) + L" / " + std::to_wstring(task.total_items);
    if (task.total_bytes) return format::ByteSize(task.transferred_bytes) + L" / " + format::ByteSize(task.total_bytes);
    return {};
}
bool RichStatistics(const ops::OpStatus& task) {
    return task.active && Transfer(task) && !Terminal(task) && task.phase != ops::OpPhase::Queued;
}
bool LiveThroughput(const ops::OpStatus& task) {
    return task.active && Transfer(task) && task.phase == ops::OpPhase::Running;
}
std::array<std::wstring, 3> MetricValues(const ops::OpStatus& task, const OperationCenterLabels& labels) {
    const bool live = LiveThroughput(task);
    std::wstring time = labels.estimating;
    if (!live) {
        const auto phase = static_cast<size_t>(task.phase);
        time = phase < labels.phase_names.size() ? labels.phase_names[phase] : L"—";
    } else if (task.eta_seconds) {
        wchar_t eta[64]{};
        swprintf_s(eta, L"%llu:%02llu:%02llu", task.eta_seconds / 3600,
            (task.eta_seconds / 60) % 60, task.eta_seconds % 60);
        time = eta;
    }
    const std::wstring remaining = task.total_items
        ? std::to_wstring(task.total_items - (std::min)(task.total_items, task.completed_items))
        : task.total_bytes ? format::ByteSize(task.total_bytes - (std::min)(task.total_bytes, task.transferred_bytes))
        : labels.estimating;
    const auto speed = live && task.bytes_per_second > 0
        ? format::ByteSize(static_cast<uint64_t>(task.bytes_per_second)) + L"/s" : std::wstring(L"—");
    return {std::move(time), remaining, speed};
}
const wchar_t* TaskGlyph(const ops::OpStatus& task) {
    if (task.phase == ops::OpPhase::Failed) return L"\xE783";
    if (task.phase == ops::OpPhase::Completed) return L"\xE73E";
    if (task.phase == ops::OpPhase::Paused) return L"\xE769";
    if (task.phase == ops::OpPhase::Cancelled) return L"\xE711";
    switch (task.type) {
    case ops::OpType::Copy: return L"\xE8C8";
    case ops::OpType::Move: return L"\xE8DE";
    case ops::OpType::RecycleDelete: case ops::OpType::RealDelete: case ops::OpType::EmptyRecycle: return L"\xE74D";
    case ops::OpType::Rename: case ops::OpType::BatchRename: return L"\xE8AC";
    case ops::OpType::CreateFolder: return L"\xE8F4";
    case ops::OpType::CreateTextFile: return L"\xE8A5";
    case ops::OpType::RestoreRecycle: return L"\xE7A7";
    }
    return L"\xE9D9";
}
}

OperationCenter::~OperationCenter() { Destroy(); }

bool OperationCenter::Create(HWND owner, Compositor* compositor) {
    if (hwnd_) return true;
    if (!owner || !compositor || !compositor->Dc()) return false;
    owner_ = owner;
    compositor_ = compositor;
    painter_.SetCompositor(compositor);
    scale_ = GetDpiForWindow(owner) / 96.0f;
    painter_.SetScale(scale_);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc) && !RegisterClassExW(&wc)) return false;
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, kClass, labels_.title.c_str(),
        WS_POPUP, 0, 0, 10, 10, owner, nullptr, wc.hInstance, this);
    return hwnd_ != nullptr;
}

void OperationCenter::Destroy() {
    Hide(false);
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (dib_) DeleteObject(dib_);
    if (memory_) DeleteDC(memory_);
    dib_ = nullptr; memory_ = nullptr; bits_ = nullptr;
    backdrop_.Reset(); backdrop_source_.Reset();
    compositor_ = nullptr;
}

void OperationCenter::SetCallbacks(OperationCenterCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    if (open_) { Layout(); UpdateHeight(true); Redraw(); }
}
void OperationCenter::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark; accent_ = accent;
    FinishCardTransition();
    if (open_) Layout();
    backdrop_.Reset();
    Redraw();
}
void OperationCenter::SetLabels(OperationCenterLabels labels) {
    labels_ = std::move(labels);
    if (hwnd_) SetWindowTextW(hwnd_, labels_.title.c_str());
    if (open_) { Layout(); UpdateHeight(true); Redraw(); }
}

void OperationCenter::Update(std::vector<ops::OpStatus> tasks) {
    uint64_t focused_task = 0;
    Action focused_action = Action::Clear;
    const bool had_focus = focused_ >= 0 && focused_ < static_cast<int>(buttons_.size());
    if (had_focus) { focused_task = buttons_[focused_].task_id; focused_action = buttons_[focused_].action; }
    uint64_t pressed_task = 0;
    Action pressed_action = Action::Clear;
    const bool had_press = pressed_ >= 0 && pressed_ < static_cast<int>(buttons_.size());
    if (had_press) { pressed_task = buttons_[pressed_].task_id; pressed_action = buttons_[pressed_].action; }
    // Keep a bounded recent history even when the caller has not trimmed it.
    size_t finished = 0;
    for (const auto& task : tasks) if (Terminal(task)) ++finished;
    if (finished > 64) {
        size_t remove = finished - 64;
        std::erase_if(tasks, [&](const auto& task) { if (Terminal(task) && remove) { --remove; return true; } return false; });
    }
    PrepareCardTransition(tasks);
    tasks_ = std::move(tasks);
    std::stable_sort(tasks_.begin(), tasks_.end(), [](const auto& a, const auto& b) {
        return Group(a) != Group(b) ? Group(a) < Group(b) : Group(a) == 2 && a.task_id > b.task_id;
    });
    hover_ = pressed_ = focused_ = -1;
    if (open_) {
        Layout();
        UpdateHeight(true);
        if (had_focus) for (size_t i = 0; i < buttons_.size(); ++i)
            if (buttons_[i].task_id == focused_task && buttons_[i].action == focused_action) { focused_ = static_cast<int>(i); break; }
        if (had_press) for (size_t i = 0; i < buttons_.size(); ++i)
            if (buttons_[i].task_id == pressed_task && buttons_[i].action == pressed_action) { pressed_ = static_cast<int>(i); break; }
        POINT cursor{};
        if (IsWindowVisible(hwnd_) && GetCursorPos(&cursor) && ScreenToClient(hwnd_, &cursor))
            hover_ = Hit(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        Redraw();
        SetTimer(hwnd_, kTimer, 16, nullptr);
    }
}

bool OperationCenter::HasFinished() const {
    return std::any_of(tasks_.begin(), tasks_.end(), Terminal);
}

float OperationCenter::ButtonWidth(const std::wstring& text) const {
    return (std::max)(56 * scale_, painter_.MeasureButtonWidth(text) + 8 * scale_);
}

void OperationCenter::Layout() {
    cards_.clear(); buttons_.clear();
    if (width_ <= 0 || height_ <= 0) return;
    const float s = scale_, margin = kMargin * s;
    body_height_ = (std::max)(1.0f, height_ - 2 * margin - kHeader * s);
    if (HasFinished() && callbacks_.clear_finished) {
        const float right = width_ - margin - kContentInset * s;
        const float width = (std::min)(ButtonWidth(labels_.clear_finished), width_ * 0.48f);
        buttons_.push_back({D2D1::RectF(right - width, margin + 12 * s, right, margin + 44 * s),
            Action::Clear, 0, labels_.clear_finished, {}});
    }
    float y = 0;
    int previous_group = -1;
    const float left = margin + kContentInset * s, right = width_ - margin - kContentInset * s;
    for (size_t i = 0; i < tasks_.size(); ++i) {
        const auto& task = tasks_[i];
        const int group = Group(task);
        std::wstring section;
        if (group != previous_group) { section = group == 0 ? labels_.current : group == 1 ? labels_.queued : labels_.recent; y += 28 * s; }
        previous_group = group;
        const float top = y;
        Card card;
        card.task = i; card.section = std::move(section); card.section_top = top;
        card.paired_paths = right - left >= 360 * s && !task.source_label.empty() && !task.destination_label.empty();
        y += (ShowPhaseBadge(task) ? 64 : 44) * s;
        if (!task.source_label.empty()) { card.source_y = y; y += 24 * s; }
        if (!task.destination_label.empty()) {
            card.destination_y = card.paired_paths ? card.source_y : y;
            if (!card.paired_paths) y += 24 * s;
        }
        if (!Terminal(task) && !task.current_item.empty() && task.current_item != task.source_label) {
            card.item_y = y; y += 24 * s;
        }
        if (!task.last_error.empty()) { card.error_y = y; y += 48 * s; }
        if (!Terminal(task) && task.phase != ops::OpPhase::Queued) {
            card.progress_y = y; y += 24 * s;
        }
        if (RichStatistics(task)) {
            card.statistics_y = y;
            card.statistics_columns = right - left >= 360 * s ? 3 : 2;
            y += (card.statistics_columns == 3 ? 56 : 100) * s;
        } else if (!Statistics(task).empty()) { card.statistics_y = y; y += 24 * s; }
        std::vector<Button> actions;
        float title_right = right - kCardPadding * s;
        if (task.task_id && callbacks_.details && (task.active || Terminal(task)) &&
            task.phase != ops::OpPhase::Queued && (task.active || !task.last_error.empty() ||
            !task.summary.empty() || !task.source_label.empty() || !task.destination_label.empty())) {
            const float width = (std::min)(ButtonWidth(labels_.details), (right - left) * 0.5f);
            const auto bounds = D2D1::RectF(right - kCardPadding * s - width, top + 6 * s, right - kCardPadding * s, top + 38 * s);
            buttons_.push_back({bounds, Action::Details, task.task_id, labels_.details, {}});
            title_right = bounds.left - 8 * s;
        }
        if (task.task_id && task.active && Transfer(task)) {
            if (task.phase == ops::OpPhase::Paused && callbacks_.resume)
                actions.push_back({{}, Action::Resume, task.task_id, labels_.resume, {}});
            else if (task.phase == ops::OpPhase::Running && callbacks_.pause)
                actions.push_back({{}, Action::Pause, task.task_id, labels_.pause, {}});
            if (task.phase != ops::OpPhase::Cancelling && callbacks_.cancel)
                actions.push_back({{}, Action::Cancel, task.task_id, labels_.cancel, {}});
        }
        if (AbsoluteDestination(task) && callbacks_.open_destination)
            actions.push_back({{}, Action::Destination, task.task_id, labels_.open_destination, task.destination_label});
        if (Terminal(task) && callbacks_.can_return_to_tray && callbacks_.return_to_tray &&
            callbacks_.can_return_to_tray(task.task_id))
            actions.push_back({{}, Action::ReturnToTray, task.task_id, labels_.return_to_tray, {}});
        else if (Terminal(task) && callbacks_.can_resume_incomplete && callbacks_.resume_incomplete &&
            callbacks_.can_resume_incomplete(task.task_id))
            actions.push_back({{}, Action::ResumeIncomplete, task.task_id, labels_.resume_incomplete, {}});
        const float action_left = left + kCardPadding * s;
        float x = action_left;
        const float end = right - kCardPadding * s;
        size_t row_first = buttons_.size();
        const auto align_row = [&] {
            const float offset = end - (x - 6 * s);
            for (size_t index = row_first; index < buttons_.size(); ++index) {
                buttons_[index].bounds.left += offset;
                buttons_[index].bounds.right += offset;
            }
        };
        if (!actions.empty()) { y += 8 * s; card.footer_y = y; }
        for (auto& button : actions) {
            const float width = (std::min)(ButtonWidth(button.text), end - action_left);
            if (x + width > end && x > action_left) {
                align_row();
                row_first = buttons_.size();
                x = action_left;
                y += 38 * s;
            }
            button.bounds = D2D1::RectF(x, y, x + width, y + 32 * s);
            buttons_.push_back(std::move(button));
            x += width + 6 * s;
        }
        if (!actions.empty()) { align_row(); y += 38 * s; }
        y += 10 * s;
        card.bounds = D2D1::RectF(left, top, right, y);
        card.title_right = title_right;
        cards_.push_back(std::move(card));
        y += 10 * s;
    }
    content_height_ = y + 8 * s;
    scroll_ = std::clamp(scroll_, 0.0f, (std::max)(0.0f, content_height_ - body_height_));
    AnimateCardLayout();
    const float offset = margin + kHeader * s - scroll_;
    for (auto& button : buttons_) if (button.action != Action::Clear) {
        button.bounds.top += offset; button.bounds.bottom += offset;
    }
}

void OperationCenter::Position(RECT anchor) {
    anchor_ = anchor;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    scale_ = GetDpiForWindow(owner_) / 96.0f;
    painter_.SetScale(scale_);
    work_area_ = monitor.rcWork;
    const RECT work = work_area_;
    const int margin = static_cast<int>(kMargin * scale_);
    width_ = (std::min)(static_cast<int>(472 * scale_) + 2 * margin, static_cast<int>(work.right - work.left));
    height_ = static_cast<int>(work.bottom - work.top);
    // Measure wrapped action rows at the final width before fitting the panel.
    Layout();
    base_x_ = std::clamp(static_cast<int>(anchor.right) - width_ + margin, static_cast<int>(work.left), static_cast<int>(work.right) - width_);
    UpdateHeight(false);
}

void OperationCenter::UpdateHeight(bool animate) {
    if (work_area_.bottom <= work_area_.top) return;
    const int maximum = (std::min)(static_cast<int>(std::ceil((600 + 2 * kMargin) * scale_)),
        static_cast<int>(work_area_.bottom - work_area_.top));
    const float content = tasks_.empty() ? 210 * scale_ :
        (std::max)(210 * scale_, content_height_ + kHeader * scale_);
    const int next = (std::min)(maximum, static_cast<int>(std::ceil(content + 2 * kMargin * scale_)));
    if (!animate || !AnimationsEnabled()) {
        target_height_ = height_ = next;
        resizing_ = false;
        UpdateVerticalPosition();
        Layout();
    } else if (next != target_height_) {
        resize_from_height_ = height_;
        target_height_ = next;
        resize_start_ = GetTickCount64();
        resizing_ = height_ != target_height_;
        if (resizing_) SetTimer(hwnd_, kTimer, 16, nullptr);
    }
}

void OperationCenter::UpdateVerticalPosition() {
    base_y_ = std::clamp(static_cast<int>(anchor_.bottom) + static_cast<int>((6 - kMargin) * scale_),
        static_cast<int>(work_area_.top), static_cast<int>(work_area_.bottom) - height_);
}

bool OperationCenter::AnimationsEnabled() const {
    if (motion_override_ >= 0) return motion_override_ != 0;
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    return enabled && !IsHighContrast();
}

void OperationCenter::Toggle(RECT anchor) {
    if (!hwnd_) return;
    if (origin_click_tick_ && GetTickCount64() - origin_click_tick_ < GetDoubleClickTime()) {
        origin_click_tick_ = 0;
        Hide();
        return;
    }
    origin_click_tick_ = 0;
    if (open_ && !closing_) { Hide(); return; }
    const bool reopening = open_ && closing_;
    if (!reopening) {
        scroll_ = 0; hover_ = pressed_ = focused_ = -1;
        Position(anchor);
        backdrop_.Reset(); backdrop_source_.Reset();
        visibility_ = 0;
    }
    open_ = true;
    BeginVisibility(false);
    if (!AnimationsEnabled()) visibility_ = 1;
    if (!Render()) { Hide(false); return; }
    Present();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    SetTimer(hwnd_, kTimer, 16, nullptr);
}

void OperationCenter::Hide(bool animate) {
    if (!hwnd_) return;
    if (animate && open_ && !closing_ && AnimationsEnabled()) {
        BeginVisibility(true);
        return;
    }
    if (animate && closing_) return;
    KillTimer(hwnd_, kTimer);
    open_ = closing_ = resizing_ = false; visibility_ = 0;
    FinishCardTransition();
    ShowWindow(hwnd_, SW_HIDE);
    backdrop_.Reset(); backdrop_source_.Reset();
    if (owner_ && IsWindow(owner_)) InvalidateRect(owner_, nullptr, FALSE);
}

void OperationCenter::DismissForDeactivation(HWND activated) {
    if (open_ && activated != hwnd_ && !IsChild(hwnd_, activated)) {
        POINT cursor{};
        if (activated == owner_ && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
            GetCursorPos(&cursor) && PtInRect(&anchor_, cursor)) origin_click_tick_ = GetTickCount64();
        Hide();
    }
}

void OperationCenter::Text(const std::wstring& text, D2D1_RECT_F rect, D2D1_COLOR_F color, bool small_text, bool wrap) {
    if (text.empty() || rect.right <= rect.left) return;
    auto* factory = compositor_->DwriteFactory();
    ComPtr<IDWriteTextLayout> layout;
    const auto value = SingleLine(text);
    if (FAILED(typography::CreateTextLayout(factory, value.c_str(), static_cast<UINT32>(value.size()),
        small_text ? compositor_->SmallFormat() : compositor_->TextFormat(), rect.right - rect.left,
        rect.bottom - rect.top, &layout))) return;
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!small_text) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0, static_cast<UINT32>(value.size())});
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(small_text ? compositor_->SmallFormat() : compositor_->TextFormat(), &ellipsis);
    layout->SetTrimming(&trim, ellipsis.get());
    if (small_text && !wrap && compositor_->DrawLumaText(
            value, layout.get(), rect, color, MakeTheme(dark_, accent_).bg)) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(compositor_->Dc()->CreateSolidColorBrush(color, &brush)))
        compositor_->Dc()->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool OperationCenter::Render(bool allow_capture) {
    if (!compositor_ || !compositor_->Dc() || width_ <= 0 || height_ <= 0) return false;
    auto* dc = compositor_->Dc();
    const auto size = D2D1::SizeU(static_cast<UINT32>(width_), static_cast<UINT32>(height_));
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &bitmap))) return false;
    ComPtr<ID2D1Image> previous;
    dc->GetTarget(&previous);
    const auto aa = dc->GetTextAntialiasMode();
    dc->SetTarget(bitmap.get());
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0));
    const auto theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    const float s = scale_, margin = kMargin * s, top = margin + kHeader * s;
    const auto surface = D2D1::RectF(margin, margin, width_ - margin, height_ - margin);
    acrylic_ = !force_solid_ && !snapshot_ && backdrop_.Draw(*compositor_, backdrop_source_, surface,
        POINT{base_x_, base_y_}, dark_, s, allow_capture && !IsWindowVisible(hwnd_));
    if (painter_.BeginFrame(theme, IsHighContrast())) {
        // A soft stacked silhouette stays within the transparent window margin.
        for (int i = 4; i > 0; --i) {
            const float spread = i * 2.0f * s;
            painter_.FillRoundedRect(D2D1::RectF(surface.left - spread, surface.top - spread + 3 * s,
                surface.right + spread, surface.bottom + spread + 3 * s), theme.radius_flyout * s + spread,
                D2D1::ColorF(0, 0.018f));
        }
        // Repaint the sampled plate after the surrounding shadow.
        if (acrylic_) backdrop_.Draw(*compositor_, backdrop_source_, surface, POINT{base_x_, base_y_}, dark_, s, false);
        painter_.DrawMenuSurface(surface, acrylic_);
        float title_right = surface.right - kContentInset * s;
        if (!buttons_.empty() && buttons_[0].action == Action::Clear) title_right = buttons_[0].bounds.left - 8 * s;
        Text(labels_.title, D2D1::RectF(surface.left + kContentInset * s, margin + 17 * s, title_right, top - 8 * s), theme.text, false);
        const auto viewport = D2D1::RectF(surface.left, top, surface.right, surface.bottom);
        dc->PushAxisAlignedClip(viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        if (tasks_.empty()) {
            const float opacity = departing_cards_.empty() ? 1 : std::clamp((card_progress_ - 0.4f) / 0.6f, 0.0f, 1.0f);
            dc->PushLayer(D2D1::LayerParameters(viewport, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::Matrix3x2F::Identity(), opacity), nullptr);
            painter_.DrawGlyph(L"\xE73E", D2D1::RectF(width_ * 0.5f - 16 * s, top + 22 * s, width_ * 0.5f + 16 * s, top + 54 * s), theme.text_secondary);
            painter_.DrawText(labels_.empty, D2D1::RectF(surface.left + 12 * s, top + 68 * s, surface.right - 12 * s, top + 98 * s),
                compositor_->TextFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
            dc->PopLayer();
        }
        DrawDepartingCards();
        for (const auto& card : cards_) {
            const auto& task = tasks_[card.task];
            auto rect = card.bounds; rect.top += top - scroll_; rect.bottom += top - scroll_;
            if (!card.section.empty()) {
                const float section_top = card.section_top + top - scroll_;
                Text(card.section, D2D1::RectF(rect.left, section_top - 25 * s, rect.right, section_top - 3 * s), theme.text_secondary);
            }
            if (rect.bottom < top || rect.top > surface.bottom) continue;
            auto fill = theme.surface_card;
            if (acrylic_) fill.a = dark_ ? 0.45f : 0.65f;
            painter_.FillRoundedRect(rect, 8 * s, fill);
            painter_.StrokeRoundedRect(rect, 8 * s, theme.stroke_card);
            const float x = rect.left + kCardPadding * s, right = rect.right - kCardPadding * s;
            const float offset = top - scroll_;
            const auto type = static_cast<size_t>(task.type), phase = static_cast<size_t>(task.phase);
            const std::wstring type_label = type < labels_.type_names.size() ? labels_.type_names[type] : labels_.title;
            const std::wstring phase_label = phase < labels_.phase_names.size() ? labels_.phase_names[phase] : std::wstring{};
            fluent::BadgeKind kind = fluent::BadgeKind::Accent;
            auto state_color = theme.accent;
            if (task.phase == ops::OpPhase::Failed) { kind = fluent::BadgeKind::Danger; state_color = theme.danger; }
            else if (task.phase == ops::OpPhase::Completed) {
                kind = fluent::BadgeKind::Success;
                state_color = IsHighContrast() ? theme.text : dark_ ? HexColor(0x45D7A4) : HexColor(0x107C5A);
            } else if (task.phase == ops::OpPhase::Paused || task.phase == ops::OpPhase::WaitingForConflict ||
                task.phase == ops::OpPhase::WaitingForDeleteConfirmation) {
                kind = fluent::BadgeKind::Warning;
                state_color = IsHighContrast() ? theme.text : dark_ ? HexColor(0xF7D154) : HexColor(0x8A5500);
            } else if (task.phase == ops::OpPhase::Queued || task.phase == ops::OpPhase::Cancelled ||
                task.phase == ops::OpPhase::Cancelling) { kind = fluent::BadgeKind::Neutral; state_color = theme.text_secondary; }
            painter_.DrawGlyph(TaskGlyph(task), D2D1::RectF(x, rect.top + 12 * s, x + 20 * s, rect.top + 32 * s), state_color);
            const float title_x = x + 32 * s;
            const auto& title = task.summary.empty() || task.summary == task.last_error ? type_label : task.summary;
            Text(title, D2D1::RectF(title_x, rect.top + 9 * s, card.title_right, rect.top + 33 * s), theme.text, false);
            if (ShowPhaseBadge(task)) {
                const float badge_width = (std::min)(painter_.MeasureBadgeWidth(phase_label), right - title_x);
                painter_.DrawBadge({D2D1::RectF(title_x, rect.top + 36 * s, title_x + badge_width, rect.top + 54 * s), phase_label, kind});
            }
            const auto path_line = [&](const std::wstring& path, float y, float left, float end, const wchar_t* glyph) {
                if (!y) return;
                painter_.DrawGlyph(glyph, D2D1::RectF(left, y + offset + 2 * s, left + 16 * s, y + offset + 18 * s), theme.text_secondary);
                const float text_left = left + 22 * s;
                const auto fitted = FitPathMiddle(SingleLine(path), end - text_left, [&](const std::wstring& value) {
                    return typography::MeasureLine(compositor_, compositor_->SmallFormat(), value);
                });
                Text(fitted, D2D1::RectF(text_left, y + offset, end, y + offset + 22 * s), theme.text_secondary);
            };
            if (card.paired_paths) {
                const float middle = (x + right) * 0.5f;
                path_line(task.source_label, card.source_y, x, middle - 16 * s, L"\xE8B7");
                painter_.DrawGlyph(L"\xE72A", D2D1::RectF(middle - 8 * s, card.source_y + offset + 2 * s,
                    middle + 8 * s, card.source_y + offset + 18 * s), theme.text_secondary);
                path_line(task.destination_label, card.destination_y, middle + 16 * s, right, L"\xE8B7");
            } else {
                path_line(task.source_label, card.source_y, x, right, L"\xE8B7");
                path_line(task.destination_label, card.destination_y, x, right, L"\xE72A");
            }
            path_line(task.current_item, card.item_y, x, right, L"\xE8A5");
            if (card.error_y) {
                auto error_fill = theme.danger; error_fill.a = 0.06f;
                const float y = card.error_y + offset;
                painter_.FillRoundedRect(D2D1::RectF(x, y, right, y + 42 * s), 4 * s, error_fill);
                Text(task.last_error, D2D1::RectF(x + 8 * s, y + 3 * s, right - 8 * s, y + 39 * s), theme.danger, true, true);
            }
            if (card.progress_y) {
                const float y = card.progress_y + offset;
                wchar_t percent[32]{};
                if (task.percent >= 0) swprintf_s(percent, L"%.0f%%", std::clamp(task.percent, 0.0f, 100.0f));
                const float bar_right = task.percent >= 0 ? right - 50 * s : right;
                fluent::ProgressSpec progress;
                progress.bounds = D2D1::RectF(x, y + 8 * s, bar_right, y + 12 * s);
                progress.value = std::clamp(task.percent / 100.0f, 0.0f, 1.0f);
                progress.indeterminate = task.percent < 0;
                progress.paused = task.phase == ops::OpPhase::Paused || task.phase == ops::OpPhase::WaitingForConflict ||
                    task.phase == ops::OpPhase::WaitingForDeleteConfirmation;
                progress.animation_progress = AnimationsEnabled() ? static_cast<float>(GetTickCount64() % 1200) / 1200 : 0;
                painter_.DrawProgressBar(progress);
                painter_.DrawText(percent, D2D1::RectF(bar_right + 6 * s, y, right, y + 22 * s),
                    compositor_->SmallFormat(), state_color, fluent::HorizontalAlignment::Right);
            }
            if (card.statistics_y) {
                const float y = card.statistics_y + offset;
                if (RichStatistics(task)) {
                    const auto values = MetricValues(task, labels_);
                    const std::array<std::wstring, 3> names{labels_.remaining_time,
                        task.total_items ? labels_.remaining_items : labels_.remaining_size, labels_.current_speed};
                    auto stats_fill = theme.text; stats_fill.a = dark_ ? 0.04f : 0.035f;
                    painter_.FillRoundedRect(D2D1::RectF(x, y, right, y + (card.statistics_columns == 3 ? 48 : 92) * s), 6 * s, stats_fill);
                    const float cell_width = (right - x - 16 * s) / card.statistics_columns;
                    for (int index = 0; index < 3; ++index) {
                        const float left = x + 8 * s + (index % card.statistics_columns) * cell_width;
                        const float cell_y = y + 4 * s + (index / card.statistics_columns) * 44 * s;
                        Text(names[index], D2D1::RectF(left, cell_y, left + cell_width - 6 * s, cell_y + 18 * s), theme.text_secondary);
                        Text(values[index], D2D1::RectF(left, cell_y + 18 * s, left + cell_width - 6 * s, cell_y + 40 * s),
                            index == 2 && LiveThroughput(task) ? state_color : theme.text);
                    }
                } else Text(Statistics(task), D2D1::RectF(x, y, right, y + 22 * s), theme.text_secondary);
            }
            if (card.footer_y) {
                const float y = card.footer_y + offset - 5 * s;
                painter_.FillRoundedRect(D2D1::RectF(x, y, right, y + s), 0, theme.stroke_divider);
            }
        }
        for (size_t i = 0; i < buttons_.size(); ++i) {
            const auto& button = buttons_[i];
            if (button.action == Action::Clear) continue;
            fluent::ButtonSpec spec;
            spec.bounds = button.bounds; spec.text = button.text;
            spec.kind = fluent::ButtonKind::Standard;
            spec.state.hovered = static_cast<int>(i) == hover_;
            spec.state.pressed = static_cast<int>(i) == pressed_;
            spec.state.keyboard_focus = spec.state.focused = static_cast<int>(i) == focused_;
            painter_.DrawButton(spec);
        }
        if (content_height_ > body_height_) {
            fluent::ScrollbarSpec scrollbar;
            scrollbar.viewport = viewport; scrollbar.offset = scroll_;
            scrollbar.viewport_extent = body_height_; scrollbar.content_extent = content_height_;
            painter_.DrawScrollbar(scrollbar);
        }
        dc->PopAxisAlignedClip();
        if (!buttons_.empty() && buttons_[0].action == Action::Clear) {
            fluent::ButtonSpec spec;
            spec.bounds = buttons_[0].bounds; spec.text = buttons_[0].text;
            spec.kind = fluent::ButtonKind::Standard;
            spec.state.hovered = hover_ == 0;
            spec.state.pressed = pressed_ == 0;
            spec.state.keyboard_focus = spec.state.focused = focused_ == 0;
            painter_.DrawButton(spec);
        }
    }
    const auto hr = dc->EndDraw();
    dc->SetTarget(previous.get()); dc->SetTextAntialiasMode(aa);
    if (FAILED(hr)) return false;
    const auto cpu_props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, cpu_props, &cpu)) || FAILED(cpu->CopyFromBitmap(nullptr, bitmap.get(), nullptr))) return false;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
    if (!memory_) memory_ = CreateCompatibleDC(nullptr);
    if (dib_ && (dib_width_ != width_ || dib_height_ != height_)) { DeleteObject(dib_); dib_ = nullptr; }
    if (!dib_ && memory_) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_; info.bmiHeader.biHeight = -height_;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
        dib_width_ = width_; dib_height_ = height_;
    }
    if (dib_) for (int row = 0; row < height_; ++row)
        std::memcpy(static_cast<BYTE*>(bits_) + static_cast<size_t>(row) * width_ * 4,
            mapped.bits + static_cast<size_t>(row) * mapped.pitch, static_cast<size_t>(width_) * 4);
    cpu->Unmap();
    return dib_ != nullptr;
}

void OperationCenter::Present() {
    if (!hwnd_ || !dib_ || !memory_) return;
    const auto old = SelectObject(memory_, dib_);
    POINT point{base_x_, base_y_ - static_cast<LONG>((1 - visibility_) * 12 * scale_)};
    POINT source{}; SIZE size{width_, height_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::clamp(visibility_, 0.0f, 1.0f) * 255), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &point, &size, memory_, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory_, old);
}

void OperationCenter::Redraw() { if (open_ && Render()) Present(); }

void OperationCenter::Tick() {
    if (!open_) return;
    if (!AnimationsEnabled()) {
        if (closing_) { Hide(false); return; }
        visibility_ = 1;
        FinishCardTransition();
        if (resizing_) UpdateHeight(false);
        Layout();
        Redraw();
        KillTimer(hwnd_, kTimer);
        return;
    }
    const float elapsed = static_cast<float>(GetTickCount64() - animation_start_);
    const float t = std::clamp(elapsed / (closing_ ? kCloseDurationMs : kOpenDurationMs), 0.0f, 1.0f);
    const float eased = closing_ ? t * t : 1 - (1 - t) * (1 - t) * (1 - t);
    visibility_ = animation_from_ + ((closing_ ? 0.0f : 1.0f) - animation_from_) * eased;
    if (closing_ && t >= 1) { Hide(false); return; }
    const bool height_changed = resizing_;
    const bool cards_changed = cards_animating_;
    if (cards_animating_) {
        card_progress_ = std::clamp(static_cast<float>(GetTickCount64() - card_animation_start_) / kCardDurationMs, 0.0f, 1.0f);
        if (card_progress_ >= 1) FinishCardTransition();
    }
    if (resizing_) {
        const float progress = std::clamp(static_cast<float>(GetTickCount64() - resize_start_) / kResizeDurationMs, 0.0f, 1.0f);
        const float resize_eased = progress * progress * (3 - 2 * progress);
        height_ = static_cast<int>(std::lround(resize_from_height_ + (target_height_ - resize_from_height_) * resize_eased));
        if (progress >= 1) { height_ = target_height_; resizing_ = false; }
        UpdateVerticalPosition();
    }
    if (height_changed || cards_changed) Layout();
    if (cards_changed && !cards_.empty() && target_height_ < resize_from_height_) {
        float bottom = 0;
        for (const auto& card : cards_) bottom = (std::max)(bottom, card.bounds.bottom);
        const int maximum = (std::min)(static_cast<int>((600 + 2 * kMargin) * scale_),
            static_cast<int>(work_area_.bottom - work_area_.top));
        const int fitted = (std::min)(maximum, static_cast<int>(std::ceil(bottom - scroll_ + (18 + kHeader + 2 * kMargin) * scale_)));
        const int next = (std::max)(target_height_, fitted);
        if (next != height_) { height_ = next; UpdateVerticalPosition(); Layout(); }
    }
    if (height_changed || cards_changed || t >= 1) Redraw();
    else Present();
    const bool indeterminate = std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) {
        return task.active && task.percent < 0 && task.phase != ops::OpPhase::Paused;
    });
    if (t >= 1 && !indeterminate && !resizing_ && !cards_animating_) KillTimer(hwnd_, kTimer);
}

void OperationCenter::Scroll(float delta) {
    const float previous = scroll_;
    scroll_ = std::clamp(scroll_ + delta, 0.0f, (std::max)(0.0f, content_height_ - body_height_));
    if (previous != scroll_) { hover_ = pressed_ = -1; Layout(); Redraw(); }
}

void OperationCenter::ScrollAt(float y) {
    const float top = (kMargin + kHeader) * scale_;
    const float target = (y - top) / (std::max)(1.0f, body_height_) * content_height_ - body_height_ * 0.5f;
    Scroll(target - scroll_);
}

int OperationCenter::Hit(float x, float y) const {
    if (closing_) return -1;
    const float top = (kMargin + kHeader) * scale_, bottom = height_ - kMargin * scale_;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        const auto& button = buttons_[i]; const auto& rect = button.bounds;
        if (button.action != Action::Clear && (y < top || y >= bottom)) continue;
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) return static_cast<int>(i);
    }
    return -1;
}

void OperationCenter::FocusButton(int direction) {
    if (buttons_.empty()) return;
    focused_ = focused_ < 0 ? (direction > 0 ? 0 : static_cast<int>(buttons_.size()) - 1) :
        (focused_ + direction + static_cast<int>(buttons_.size())) % static_cast<int>(buttons_.size());
    const auto& button = buttons_[focused_];
    if (button.action != Action::Clear) {
        const float top = (kMargin + kHeader) * scale_, bottom = height_ - kMargin * scale_;
        if (button.bounds.top < top) Scroll(button.bounds.top - top - 8 * scale_);
        else if (button.bounds.bottom > bottom) Scroll(button.bounds.bottom - bottom + 8 * scale_);
    }
    Redraw();
}

void OperationCenter::Invoke(int index) {
    if (index < 0 || index >= static_cast<int>(buttons_.size()) || closing_) return;
    const auto button = buttons_[index];
    // Copy callbacks before dispatch: application code may synchronously replace
    // the snapshots/labels or close this popup. No references survive dispatch.
    const auto callbacks = callbacks_;
    switch (button.action) {
    case Action::Clear: if (callbacks.clear_finished) callbacks.clear_finished(); return;
    case Action::Details: Hide(); if (callbacks.details) callbacks.details(button.task_id); return;
    case Action::Cancel: if (callbacks.cancel) callbacks.cancel(button.task_id); return;
    case Action::Pause: if (callbacks.pause) callbacks.pause(button.task_id); return;
    case Action::Resume: if (callbacks.resume) callbacks.resume(button.task_id); return;
    case Action::Destination: Hide(); if (callbacks.open_destination) callbacks.open_destination(button.path); return;
    case Action::ReturnToTray:
        if (callbacks.can_return_to_tray && callbacks.can_return_to_tray(button.task_id) && callbacks.return_to_tray)
            callbacks.return_to_tray(button.task_id);
        return;
    case Action::ResumeIncomplete:
        if (callbacks.can_resume_incomplete && callbacks.can_resume_incomplete(button.task_id) && callbacks.resume_incomplete)
            callbacks.resume_incomplete(button.task_id);
        return;
    }
}

LRESULT CALLBACK OperationCenter::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<OperationCenter*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<OperationCenter*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); EndPaint(hwnd, &paint); return 0; }
    case WM_TIMER: self->Tick(); return 0;
    case WM_CLOSE: self->Hide(); return 0;
    case WM_CANCELMODE: self->Hide(false); return 0;
    case WM_ACTIVATE: if (LOWORD(wparam) == WA_INACTIVE) self->DismissForDeactivation(reinterpret_cast<HWND>(lparam)); return 0;
    case WM_ACTIVATEAPP: if (!wparam) self->Hide(); return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        self->backdrop_.Reset(); self->Tick(); return 0;
    case WM_DPICHANGED:
        self->Hide(false); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) self->Hide();
        else if (wparam == VK_TAB) self->FocusButton(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
        else if (wparam == VK_RETURN || wparam == VK_SPACE) self->Invoke(self->focused_);
        else if (wparam == VK_DOWN) self->Scroll(48 * self->scale_);
        else if (wparam == VK_UP) self->Scroll(-48 * self->scale_);
        else if (wparam == VK_NEXT) self->Scroll(self->body_height_ * 0.8f);
        else if (wparam == VK_PRIOR) self->Scroll(-self->body_height_ * 0.8f);
        else if (wparam == VK_HOME) self->Scroll(-self->content_height_);
        else if (wparam == VK_END) self->Scroll(self->content_height_);
        return 0;
    case WM_MOUSEWHEEL:
        self->Scroll(-GET_WHEEL_DELTA_WPARAM(wparam) / static_cast<float>(WHEEL_DELTA) * 72 * self->scale_); return 0;
    case WM_MOUSEMOVE: {
        if (self->dragging_scrollbar_) { self->ScrollAt(static_cast<float>(GET_Y_LPARAM(lparam))); return 0; }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
        const int hit = self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (hit != self->hover_) { self->hover_ = hit; self->Redraw(); } return 0;
    }
    case WM_MOUSELEAVE: self->hover_ = -1; self->Redraw(); return 0;
    case WM_LBUTTONDOWN:
        if (!self->closing_ && self->content_height_ > self->body_height_ &&
            GET_X_LPARAM(lparam) >= self->width_ - (kMargin + 14) * self->scale_ &&
            GET_Y_LPARAM(lparam) >= (kMargin + kHeader) * self->scale_) {
            self->dragging_scrollbar_ = true; SetCapture(hwnd);
            self->ScrollAt(static_cast<float>(GET_Y_LPARAM(lparam))); return 0;
        }
        self->pressed_ = self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (self->pressed_ >= 0) { self->focused_ = self->pressed_; SetCapture(hwnd); self->Redraw(); } return 0;
    case WM_LBUTTONUP: {
        if (self->dragging_scrollbar_) { self->dragging_scrollbar_ = false; ReleaseCapture(); return 0; }
        const int pressed = self->pressed_;
        self->pressed_ = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        const int hit = self->Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        self->Redraw(); if (pressed >= 0 && pressed == hit) self->Invoke(hit); return 0;
    }
    case WM_CAPTURECHANGED: self->dragging_scrollbar_ = false; self->pressed_ = -1; self->Redraw(); return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool OperationCenter::SaveDebugSnapshot(const wchar_t* path, int width_dip, int height_dip) {
    if (!compositor_ || open_ || width_dip < 120 || height_dip < 120) return false;
    const int previous_width = width_, previous_height = height_;
    width_ = static_cast<int>((width_dip + 2 * kMargin) * scale_);
    height_ = static_cast<int>((height_dip + 2 * kMargin) * scale_);
    snapshot_ = true; Layout(); const bool rendered = Render(); snapshot_ = false;
    width_ = previous_width; height_ = previous_height;
    if (!rendered) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(static_cast<UINT>(dib_width_), static_cast<UINT>(dib_height_)))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WritePixels(static_cast<UINT>(dib_height_), static_cast<UINT>(dib_width_ * 4),
            static_cast<UINT>(dib_width_ * dib_height_ * 4), static_cast<BYTE*>(bits_))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace pulse::ui
