// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#ifdef PULSE_VIDEO_PREVIEW_TESTING
#include <functional>
#endif

namespace pulse::ui {

// MFPlay is loaded on demand (including on Windows N without Media Foundation).
// All media/provider calls run off the UI thread. Each open has its own mailbox,
// so a retired decoder cannot publish state into the next file's preview.
class VideoPreview {
public:
    struct State {
        bool ready = false;
        bool playing = false;
        bool ended = false;
        bool can_seek = false;
        bool busy = false;
        int64_t position = 0; // 100 ns units, not an estimated frame number
        int64_t duration = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stepped_frames = 0; // successful steps since the last seek/play
        HRESULT error = S_OK;
        HRESULT control_error = S_OK;
    };
    VideoPreview() = default;
    ~VideoPreview();
    VideoPreview(const VideoPreview&) = delete;
    VideoPreview& operator=(const VideoPreview&) = delete;
    static bool Supports(const std::wstring& path);
    void Open(HWND owner, const std::wstring& path);
    void Reset();
    void Layout(const RECT& bounds, bool visible);
    void Play(bool playing);
    void Seek(double fraction);
    void Step();
    State Snapshot() const;
    bool active() const noexcept { return state_ != nullptr; }
#ifdef PULSE_VIDEO_PREVIEW_TESTING
    struct ResourceUsage { uint32_t live = 0, retired = 0, limit = 0; };
    static ResourceUsage ResourcesForTest();
    static void SetWorkerHookForTest(std::function<void()> hook);
#endif
private:
    struct Shared;
    struct Workers;
    static Workers& WorkerPool();
    static DWORD WINAPI ThreadProc(void* argument);
    static void Run(std::shared_ptr<Shared> state, std::wstring path);
    static LRESULT CALLBACK VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    std::shared_ptr<Shared> state_;
    HWND child_ = nullptr;
};

} // namespace pulse::ui
