#include "typography.h"
#include "quick_preview_window.h"
#include "playback_timeline.h"
#include "../common/localization.h"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kVideoTimer = 8;
constexpr UINT_PTR kScrubTimer = 9;
constexpr ULONGLONG kSeekIntervalMs = 80;
constexpr float kPlaybackHeight = 76.0f;
bool Contains(const D2D1_RECT_F& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
        point.y >= rect.top && point.y < rect.bottom;
}
std::wstring Clock(int64_t ticks) {
    const int64_t ms = (std::max)(int64_t{0}, ticks / 10000);
    wchar_t value[64];
    swprintf_s(value, L"%02lld:%02lld.%03lld", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return value;
}
}

bool QuickPreviewWindow::HasPlayback() const noexcept {
    return frame_count_ > 1 || video_.active();
}
float QuickPreviewWindow::PlaybackHeight() const noexcept {
    return HasPlayback() ? kPlaybackHeight * scale_ : 0.0f;
}
D2D1_RECT_F QuickPreviewWindow::PlaybackRect() const {
    const float height = static_cast<float>(compositor_.Height());
    return D2D1::RectF(0, height - PlaybackHeight(),
        static_cast<float>(compositor_.Width()), height);
}
D2D1_RECT_F QuickPreviewWindow::PlaybackButtonRect(int button) const {
    const auto bar = PlaybackRect();
    const float x = (12.0f + 70.0f * static_cast<float>(button)) * scale_;
    return D2D1::RectF(x, bar.top + 6.0f * scale_, x + 66.0f * scale_,
        bar.top + 38.0f * scale_);
}
D2D1_RECT_F QuickPreviewWindow::PlaybackTrackRect() const {
    const auto bar = PlaybackRect();
    const float left = (video_.active() ? 164.0f : 234.0f) * scale_;
    return D2D1::RectF(left, bar.top + 6.0f * scale_,
        (std::max)(left + scale_, bar.right - 18.0f * scale_), bar.top + 38.0f * scale_);
}
void QuickPreviewWindow::BeginVideo() {
    if (safe_mode_ || OfflinePlaceholder() || !VideoPreview::Supports(item_.path)) return;
    handler_.Reset();
    video_.Open(hwnd_, item_.path);
    SetTimer(hwnd_, kVideoTimer, 50, nullptr);
}
void QuickPreviewWindow::CancelPlaybackScrub() {
    const bool was_dragging = playback_drag_;
    playback_drag_ = false;
    playback_resume_ = false;
    playback_scrub_pending_ = false;
    playback_seek_dirty_ = false;
    playback_scrub_fraction_ = 0;
    playback_last_seek_ms_ = 0;
    if (hwnd_) {
        KillTimer(hwnd_, kScrubTimer);
        if (was_dragging && GetCapture() == hwnd_) ReleaseCapture();
    }
}
void QuickPreviewWindow::ResetPlayback() {
    CancelPlaybackScrub();
    if (hwnd_) {
        KillTimer(hwnd_, kVideoTimer);
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    video_.Reset();
}
void QuickPreviewWindow::TogglePlayback() {
    if (playback_drag_) {
        EndPlaybackDrag(false);
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    if (video_.active()) {
        const auto state = video_.Snapshot();
        if (state.ready && SUCCEEDED(state.error)) video_.Play(!state.playing);
    } else if (frame_count_ > 1) {
        animation_active_ = !animation_active_;
        KillTimer(hwnd_, kAnimationTimer);
        if (animation_active_) {
            completed_loops_ = 0;
            if (!playback_scrub_pending_ && !waiting_for_frame_ && frame_index_ + 1 >= frame_count_) {
                requested_frame_ = 0;
                waiting_for_frame_ = true;
            }
            if (!playback_scrub_pending_ && !waiting_for_frame_)
                SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        } else if (waiting_for_frame_ && !playback_scrub_pending_) {
            // Pause means retain the frame actually on screen, not an outstanding auto tick.
            waiting_for_frame_ = false;
            requested_frame_ = frame_index_;
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::StepPlayback(int direction) {
    const uint32_t base_frame = playback_scrub_pending_
        ? playback::FrameAt(playback_scrub_fraction_, frame_count_)
        : waiting_for_frame_ ? requested_frame_ : frame_index_;
    CancelPlaybackScrub();
    if (video_.active()) {
        if (direction > 0) video_.Step(); // MFPlay's exact frame step is forward-only.
    } else if (frame_count_ > 1) {
        animation_active_ = false;
        KillTimer(hwnd_, kAnimationTimer);
        requested_frame_ = playback::StepFrame(
            base_frame, direction, frame_count_);
        waiting_for_frame_ = requested_frame_ != frame_index_;
        completed_loops_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SeekPlayback(float x) {
    const auto track = PlaybackTrackRect();
    // Pointer position is continuous, independent of both frame quantization and
    // asynchronous decoding. Moving the thumb never waits for a thumbnail.
    playback_scrub_fraction_ = playback::Fraction(x, track.left, track.right);
    playback_scrub_pending_ = true;
    playback_seek_dirty_ = true;
    SubmitPlaybackSeek(!playback_drag_);
    SetTimer(hwnd_, kScrubTimer, 16, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::SubmitPlaybackSeek(bool immediate) {
    if (!playback_seek_dirty_) return;
    const auto now = GetTickCount64();
    if (!immediate && playback_last_seek_ms_ && now - playback_last_seek_ms_ < kSeekIntervalMs)
        return;
    if (video_.active()) {
        const auto state = video_.Snapshot();
        if (!state.ready || !state.can_seek || state.duration <= 0 || FAILED(state.error)) {
            playback_seek_dirty_ = false;
            return;
        }
        if (!immediate && state.busy) return;
        video_.Seek(playback_scrub_fraction_);
    } else if (frame_count_ > 1) {
        // Do not replace an in-flight frame with every mouse move. Keep only the
        // newest desired fraction, then submit it when this decode has finished.
        if (waiting_for_frame_) return;
        requested_frame_ = playback::FrameAt(playback_scrub_fraction_, frame_count_);
        waiting_for_frame_ = requested_frame_ != frame_index_;
        completed_loops_ = 0;
    }
    playback_last_seek_ms_ = now;
    playback_seek_dirty_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void QuickPreviewWindow::TickPlaybackSeek() {
    SubmitPlaybackSeek(!playback_drag_);
    const bool settled = video_.active() ? !playback_seek_dirty_
        : !playback_seek_dirty_ && !waiting_for_frame_;
    if (!playback_drag_ && settled) {
        playback_scrub_pending_ = false;
        KillTimer(hwnd_, kScrubTimer);
        if (!video_.active() && animation_active_ && frame_count_ > 1)
            SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
double QuickPreviewWindow::PlaybackFraction(const VideoPreview::State& state) const {
    if (playback_drag_ || playback_scrub_pending_) return playback_scrub_fraction_;
    if (video_.active() && state.duration > 0)
        return std::clamp(static_cast<double>(state.position) / state.duration, 0.0, 1.0);
    return frame_count_ > 1 ? static_cast<double>(frame_index_) / (frame_count_ - 1) : 0;
}
bool QuickPreviewWindow::PlaybackMouseDown(POINT point) {
    if (!HasPlayback() || !Contains(PlaybackRect(), point)) return false;
    SetFocus(hwnd_);
    const bool video = video_.active();
    const auto state = video_.Snapshot();
    if (video && (!state.ready || FAILED(state.error))) return true;
    const int play = video ? 0 : 1;
    const int next = video ? 1 : 2;
    if (Contains(PlaybackButtonRect(play), point)) TogglePlayback();
    else if (Contains(PlaybackButtonRect(next), point)) StepPlayback(1);
    else if (!video && Contains(PlaybackButtonRect(0), point)) StepPlayback(-1);
    else if (Contains(PlaybackTrackRect(), point) && (!video || state.can_seek)) {
        const bool was_playing = video ? state.playing : animation_active_;
        CancelPlaybackScrub();
        playback_resume_ = was_playing;
        playback_drag_ = true;
        if (video) video_.Play(false);
        else {
            animation_active_ = false;
            KillTimer(hwnd_, kAnimationTimer);
        }
        SetCapture(hwnd_);
        SeekPlayback(static_cast<float>(point.x));
    }
    return true;
}
void QuickPreviewWindow::EndPlaybackDrag(bool resume) {
    if (!playback_drag_) return;
    playback_drag_ = false;
    const bool play = resume && playback_resume_;
    playback_resume_ = false;
    SubmitPlaybackSeek(true); // release bypasses throttling, but not a GIF decode in flight
    if (video_.active()) video_.Play(play);
    else animation_active_ = play;
    TickPlaybackSeek();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::DrawPlayback(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush) {
    if (!HasPlayback() || !dc || !brush) return;
    const auto bar = PlaybackRect();
    const bool video = video_.active();
    const auto state = video_.Snapshot();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    const auto original = brush->GetColor();
    const auto text_background = dark_ ? D2D1::ColorF(0x151515) : D2D1::ColorF(0xF7F7F7);
    auto color = original;
    color.a = 0.12f;
    brush->SetColor(color);
    dc->DrawLine(D2D1::Point2F(bar.left, bar.top), D2D1::Point2F(bar.right, bar.top), brush);
    auto* format = compositor_.SmallFormat();
    if (!format) { brush->SetColor(original); return; }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < (video ? 2 : 3); ++i) {
        const bool is_play = i == (video ? 0 : 1);
        const auto& text = l10n::Get(is_play
            ? (playing ? l10n::StringId::OpPause : l10n::StringId::PlaybackPlay)
            : (i == 0 ? l10n::StringId::PlaybackPrevFrame : l10n::StringId::PlaybackNextFrame));
        const bool available = enabled && (is_play || !video || !state.ended);
        const auto rect = PlaybackButtonRect(i);
        color = original; color.a = available ? 0.09f : 0.03f;
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4 * scale_, 4 * scale_), brush);
        color = original; color.a = available ? 1.0f : 0.4f;
        brush->SetColor(color);
        if (!compositor_.DrawLumaText(text, format, rect, color, text_background, DWRITE_TEXT_ALIGNMENT_CENTER))
            dc->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect, brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
    }
    const auto track = PlaybackTrackRect();
    const float y = (track.top + track.bottom) * 0.5f;
    const double fraction = PlaybackFraction(state);
    color = original; color.a = enabled && (!video || state.can_seek) ? 0.3f : 0.1f;
    brush->SetColor(color);
    dc->DrawLine(D2D1::Point2F(track.left, y), D2D1::Point2F(track.right, y), brush, 3 * scale_);
    const float thumb = track.left + static_cast<float>(fraction) * (track.right - track.left);
    brush->SetColor(original);
    dc->DrawLine(D2D1::Point2F(track.left, y), D2D1::Point2F(thumb, y), brush, 3 * scale_);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb, y), 5 * scale_, 5 * scale_), brush);
    const std::wstring info = PlaybackInfo(state);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    const auto info_rect = D2D1::RectF(12 * scale_, bar.top + 40 * scale_, bar.right - 12 * scale_, bar.bottom - 8 * scale_);
    if (!compositor_.DrawLumaText(info, format, info_rect, original, text_background))
        dc->DrawTextW(info.data(), static_cast<UINT32>(info.size()), format, info_rect,
            brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
    brush->SetColor(original);
}
std::wstring QuickPreviewWindow::PlaybackInfo(const VideoPreview::State& state) const {
    const bool video = video_.active();
    const bool enabled = !video || (state.ready && SUCCEEDED(state.error));
    const bool playing = video ? state.playing : animation_active_;
    std::wstring info;
    if (video) {
        info = Clock(state.position) + L" / " + Clock(state.duration);
        if (state.width && state.height)
            info += L" · " + std::to_wstring(state.width) + L"×" + std::to_wstring(state.height);
        if (state.stepped_frames)
            info += l10n::Get(l10n::StringId::PlaybackSteppedPrefix) + std::to_wstring(state.stepped_frames);
        if (FAILED(state.error)) info += l10n::Get(l10n::StringId::PlaybackError);
        else if (FAILED(state.control_error)) info += l10n::Get(l10n::StringId::PlaybackControlUnavailable);
        else if (!state.ready) info += l10n::Get(l10n::StringId::PlaybackLoading);
        else if (playback_drag_) info += l10n::Get(l10n::StringId::PlaybackScrubbing);
        else if (state.busy) info += l10n::Get(l10n::StringId::PlaybackSeeking);
        else if (state.ended) info += l10n::Get(l10n::StringId::PlaybackEnded);
    } else {
        info = l10n::Get(l10n::StringId::PlaybackFramePrefix) + std::to_wstring(frame_index_ + 1) + L" / " +
            std::to_wstring(frame_count_) + L" · " + std::to_wstring(frame_delay_ms_) + L" ms";
        // Waiting is normal between every pair of GIF frames. Never insert a
        // transient "Seeking" label here: it shifts/flashes the entire suffix.
        if (playback_drag_) info += l10n::Get(l10n::StringId::PlaybackScrubbing);
    }
    if (enabled) info += playing ? l10n::Get(l10n::StringId::PlaybackPlaying) : l10n::Get(l10n::StringId::PlaybackPaused);
    return info;
}
} // namespace pulse::ui
