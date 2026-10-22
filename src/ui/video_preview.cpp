#include "video_preview.h"
#include "../common/path_utils.h"
#include <mfplay.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#pragma comment(lib, "mfuuid.lib")

namespace pulse::ui {
using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t kVideoClass[] = L"Pulse.VideoPreview";
constexpr UINT kRetireVideo = WM_APP + 87;

struct Event {
    MFP_EVENT_TYPE type{};
    HRESULT error = S_OK;
    ComPtr<IMFPMediaItem> item;
};

// Callback never touches the player, a window, or QuickPreviewWindow.
class Events final : public IMFPMediaPlayerCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFPMediaPlayerCallback))
            return E_NOINTERFACE;
        *out = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header) override {
        if (!header) return;
        Event event{header->eEventType, header->hrEvent, {}};
        if (header->eEventType == MFP_EVENT_TYPE_MEDIAITEM_CREATED && SUCCEEDED(header->hrEvent))
            event.item = reinterpret_cast<MFP_MEDIAITEM_CREATED_EVENT*>(header)->pMediaItem;
        std::lock_guard lock(mutex_);
        if (events_.size() < 64) events_.push_back(std::move(event));
    }
    std::deque<Event> Take() {
        std::lock_guard lock(mutex_);
        return std::exchange(events_, {});
    }
private:
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    std::deque<Event> events_;
};

int64_t Position(IMFPMediaPlayer* player, bool duration) {
    PROPVARIANT value{};
    const HRESULT hr = duration ? player->GetDuration(MFP_POSITIONTYPE_100NS, &value)
                                : player->GetPosition(MFP_POSITIONTYPE_100NS, &value);
    int64_t result = 0;
    if (SUCCEEDED(hr)) {
        if (value.vt == VT_I8) result = value.hVal.QuadPart;
        else if (value.vt == VT_UI8) result = static_cast<int64_t>(value.uhVal.QuadPart);
    }
    PropVariantClear(&value);
    return (std::max)(int64_t{0}, result);
}
} // namespace

struct VideoPreview::Shared {
    std::mutex mutex;
    State snapshot;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> repaint{true};
    HWND child = nullptr;
    bool playing = true;
    bool seek_pending = false;
    double seek_fraction = 0;
    unsigned steps = 0;
#ifdef PULSE_VIDEO_PREVIEW_TESTING
    std::function<void()> worker_hook;
#endif
};

struct VideoPreview::Workers {
    static constexpr uint32_t limit = 4;
    struct Slot {
        std::shared_ptr<Shared> owner;
        HANDLE thread = nullptr;
    };
    std::mutex mutex;
    std::array<Slot, limit> slots;
#ifdef PULSE_VIDEO_PREVIEW_TESTING
    std::function<void()> hook;
#endif
    void Reap() {
        for (auto& slot : slots) {
            if (slot.thread && WaitForSingleObject(slot.thread, 0) == WAIT_OBJECT_0) {
                CloseHandle(slot.thread);
                slot = {};
            }
        }
    }
};

VideoPreview::Workers& VideoPreview::WorkerPool() {
    // A blocked provider must retain its mailbox/render target until actual exit.
    static auto* pool = new Workers();
    return *pool;
}

DWORD WINAPI VideoPreview::ThreadProc(void* argument) {
    using Work = std::pair<std::shared_ptr<Shared>, std::wstring>;
    std::unique_ptr<Work> work(static_cast<Work*>(argument));
    Run(work->first, std::move(work->second));
    return 0;
}

#ifdef PULSE_VIDEO_PREVIEW_TESTING
VideoPreview::ResourceUsage VideoPreview::ResourcesForTest() {
    auto& pool = WorkerPool();
    std::lock_guard lock(pool.mutex);
    pool.Reap();
    ResourceUsage usage{0, 0, Workers::limit};
    for (const auto& slot : pool.slots) {
        if (slot.owner) {
            ++usage.live;
            if (slot.owner->stop.load()) ++usage.retired;
        }
    }
    return usage;
}
void VideoPreview::SetWorkerHookForTest(std::function<void()> hook) {
    auto& pool = WorkerPool();
    std::lock_guard lock(pool.mutex);
    pool.hook = std::move(hook);
}
#endif

VideoPreview::~VideoPreview() { Reset(); }

bool VideoPreview::Supports(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    for (const auto* video : {L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv",
                              L".webm", L".mpg", L".mpeg", L".m2ts", L".mts", L".3gp"})
        if (ext == video) return true;
    return false;
}

LRESULT CALLBACK VideoPreview::VideoProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const HWND parent = GetParent(hwnd);
    auto* holder = reinterpret_cast<std::shared_ptr<Shared>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kRetireVideo) {
        if (holder && holder->get() == reinterpret_cast<Shared*>(lparam) && (*holder)->stop.load())
            DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_NCDESTROY && holder) {
        (*holder)->stop.store(true);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete holder;
    }
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        if (holder) (*holder)->repaint.store(true);
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: SetFocus(parent); return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CONTEXTMENU:
        PostMessageW(parent, message, wparam, lparam);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void VideoPreview::Open(HWND owner, const std::wstring& path) {
    Reset();
    state_ = std::make_shared<Shared>();
    auto& pool = WorkerPool();
    std::lock_guard admission(pool.mutex);
    pool.Reap();
    auto slot = std::find_if(pool.slots.begin(), pool.slots.end(),
        [](const auto& value) { return !value.owner; });
    if (slot == pool.slots.end()) {
        state_->snapshot.error = HRESULT_FROM_WIN32(ERROR_BUSY);
        state_->finished.store(true);
        return;
    }
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = VideoProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kVideoClass;
        RegisterClassW(&wc);
    });
    child_ = CreateWindowExW(0, kVideoClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    state_->child = child_;
    if (!child_) { state_->snapshot.error = HRESULT_FROM_WIN32(GetLastError()); return; }
    try {
        SetWindowLongPtrW(child_, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(new std::shared_ptr<Shared>(state_)));
#ifdef PULSE_VIDEO_PREVIEW_TESTING
        state_->worker_hook = pool.hook;
#endif
        using Work = std::pair<std::shared_ptr<Shared>, std::wstring>;
        auto work = std::make_unique<Work>(state_, path);
        slot->owner = state_;
        slot->thread = CreateThread(nullptr, 0, ThreadProc, work.get(), 0, nullptr);
        if (slot->thread) work.release();
        else {
            slot->owner.reset();
            state_->snapshot.error = HRESULT_FROM_WIN32(GetLastError());
            state_->finished.store(true);
        }
    } catch (...) {
        state_->snapshot.error = E_OUTOFMEMORY;
        state_->finished.store(true);
    }
}

void VideoPreview::Reset() {
    if (state_) state_->stop.store(true);
    if (child_) {
        ShowWindow(child_, SW_HIDE);
        // Retire the render target only after MFPlay has released it. The child
        // holds the mailbox alive; late retirement messages verify its identity.
        if (!state_ || state_->finished.load()) DestroyWindow(child_);
        child_ = nullptr;
    }
    state_.reset();
}

void VideoPreview::Layout(const RECT& bounds, bool visible) {
    if (!child_) return;
    SetWindowPos(child_, nullptr, bounds.left, bounds.top,
        (std::max)(1L, bounds.right - bounds.left), (std::max)(1L, bounds.bottom - bounds.top),
        SWP_NOACTIVATE | SWP_NOZORDER | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

VideoPreview::State VideoPreview::Snapshot() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->snapshot;
}
void VideoPreview::Play(bool playing) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    state_->playing = playing;
    state_->snapshot.playing = playing;
    state_->steps = 0;
    state_->snapshot.control_error = S_OK;
    if (playing && state_->snapshot.ended) {
        state_->seek_fraction = 0;
        state_->seek_pending = true;
    }
}
void VideoPreview::Seek(double fraction) {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (!state_->snapshot.can_seek || state_->snapshot.duration <= 0) return;
    state_->seek_fraction = std::clamp(fraction, 0.0, 1.0);
    state_->seek_pending = true;
    state_->steps = 0;
    state_->snapshot.control_error = S_OK;
}
void VideoPreview::Step() {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (!state_->snapshot.ready || state_->snapshot.ended) return;
    state_->playing = false;
    state_->snapshot.playing = false;
    state_->steps = (std::min)(state_->steps + 1, 16u);
    state_->snapshot.control_error = S_OK;
}

void VideoPreview::Run(std::shared_ptr<Shared> state, std::wstring path) {
    struct Finish {
        std::shared_ptr<Shared> state;
        ~Finish() {
            state->finished.store(true);
            if (state->stop.load())
                PostMessageW(state->child, kRetireVideo, 0, reinterpret_cast<LPARAM>(state.get()));
        }
    } finish{state};
#ifdef PULSE_VIDEO_PREVIEW_TESTING
    if (state->worker_hook) { state->worker_hook(); return; }
#endif
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE library = nullptr;
    ComPtr<IMFPMediaPlayer> player;
    ComPtr<Events> events;
    auto fail = [&](HRESULT hr) {
        std::lock_guard lock(state->mutex);
        state->snapshot.error = hr;
        state->snapshot.ready = false;
        state->snapshot.playing = false;
    };
    if (FAILED(com)) { fail(com); return; }
    library = LoadLibraryExW(L"mfplay.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using CreatePlayer = HRESULT (WINAPI*)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS,
        IMFPMediaPlayerCallback*, HWND, IMFPMediaPlayer**);
    const auto create = library ? reinterpret_cast<CreatePlayer>(
        GetProcAddress(library, "MFPCreateMediaPlayer")) : nullptr;
    HRESULT hr = create ? S_OK : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    if (SUCCEEDED(hr) && !state->stop.load()) {
        events.Attach(new Events());
        hr = create(nullptr, FALSE, MFP_OPTION_FREE_THREADED_CALLBACK,
            events.Get(), state->child, &player);
        // MFPlay resolves a URL and rejects filesystem extended-length prefixes.
        const std::wstring media_path = pulse::path::StripExtendedPathPrefix(path);
        if (SUCCEEDED(hr)) hr = player->CreateMediaItemFromURL(media_path.c_str(), FALSE, 0, nullptr);
    }
    if (FAILED(hr)) fail(hr);
    bool ready = false;
    bool busy = false;
    bool positioning = false;
    bool ended = false;
    bool operation_error = false;
    uint32_t stepped = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (SUCCEEDED(hr) && player && !state->stop.load()) {
        for (const auto& event : events->Take()) {
            if (FAILED(event.error)) {
                if (!ready || event.type == MFP_EVENT_TYPE_ERROR) { hr = event.error; fail(hr); break; }
                busy = false;
                positioning = false;
                operation_error = true;
                std::lock_guard lock(state->mutex);
                state->snapshot.control_error = event.error;
                state->playing = false;
                state->steps = 0;
                state->seek_pending = false;
                continue;
            }
            switch (event.type) {
            case MFP_EVENT_TYPE_MEDIAITEM_CREATED:
                hr = event.item ? player->SetMediaItem(event.item.Get()) : E_FAIL;
                break;
            case MFP_EVENT_TYPE_MEDIAITEM_SET: {
                ready = true;
                MFP_MEDIAITEM_CHARACTERISTICS flags{};
                ComPtr<IMFPMediaItem> item;
                if (SUCCEEDED(player->GetMediaItem(&item))) item->GetCharacteristics(&flags);
                SIZE native{}, aspect{};
                player->GetNativeVideoSize(&native, &aspect);
                std::lock_guard lock(state->mutex);
                state->snapshot.can_seek = (flags & MFP_MEDIAITEM_CAN_SEEK) != 0;
                state->snapshot.width = static_cast<uint32_t>((std::max)(0L, native.cx));
                state->snapshot.height = static_cast<uint32_t>((std::max)(0L, native.cy));
                break;
            }
            case MFP_EVENT_TYPE_PLAYBACK_ENDED: {
                ended = true;
                busy = false;
                positioning = false;
                std::lock_guard lock(state->mutex);
                state->playing = false;
                state->steps = 0;
                break;
            }
            case MFP_EVENT_TYPE_FRAME_STEP: ++stepped; busy = false; break;
            case MFP_EVENT_TYPE_POSITION_SET:
                ended = false; stepped = 0; busy = false; positioning = false; break;
            case MFP_EVENT_TYPE_PLAY: ended = false; stepped = 0; busy = false; break;
            case MFP_EVENT_TYPE_PAUSE: busy = false; break;
            default: break;
            }
        }
        if (FAILED(hr)) { fail(hr); break; }
        MFP_MEDIAPLAYER_STATE actual = MFP_MEDIAPLAYER_STATE_EMPTY;
        player->GetState(&actual);
        const int64_t duration = ready ? Position(player.Get(), true) : 0;
        const int64_t position = ready ? Position(player.Get(), false) : 0;
        bool want_play = false, seek = false, step = false;
        double fraction = 0;
        {
            std::lock_guard lock(state->mutex);
            state->snapshot.ready = ready;
            state->snapshot.playing = state->playing && !ended;
            state->snapshot.ended = ended;
            state->snapshot.busy = busy || state->seek_pending || state->steps != 0;
            state->snapshot.duration = duration;
            state->snapshot.position = ended ? duration : position;
            state->snapshot.stepped_frames = stepped;
            want_play = state->playing;
            // Explicit user input clears a recoverable control error.
            if (SUCCEEDED(state->snapshot.control_error)) operation_error = false;
            if (ready && (!busy || positioning) && !operation_error &&
                (state->playing || actual != MFP_MEDIAPLAYER_STATE_PLAYING)) {
                // Finish the pause before consuming a scrub request. Otherwise
                // continuous mouse seeks can starve Pause and leave audio playing.
                seek = state->seek_pending;
                fraction = state->seek_fraction;
                if (seek) state->seek_pending = false;
                else if (!busy && state->steps && actual != MFP_MEDIAPLAYER_STATE_PLAYING) {
                    step = true;
                    --state->steps;
                }
            }
        }
        HRESULT control = S_OK;
        bool issued = false;
        if (ready && (!busy || (positioning && seek)) && !operation_error) {
            if (seek && duration > 0) {
                PROPVARIANT target{};
                target.vt = VT_I8;
                // Leave a small decodable tail: seeking past the final video sample
                // can stall MFPlay permanently, including subsequent seeks.
                const int64_t last_seek = duration - (std::min)(duration / 2, int64_t{1000000});
                target.hVal.QuadPart = (std::min)(last_seek,
                    static_cast<int64_t>(fraction * static_cast<double>(duration)));
                control = player->SetPosition(MFP_POSITIONTYPE_100NS, &target);
                issued = true;
            } else if (!want_play && actual == MFP_MEDIAPLAYER_STATE_PLAYING) {
                control = player->Pause(); issued = true;
            } else if (step) {
                control = player->FrameStep(); issued = true;
            } else if (want_play && actual != MFP_MEDIAPLAYER_STATE_PLAYING) {
                control = player->Play(); issued = true;
            }
            if (issued && SUCCEEDED(control)) {
                busy = true;
                positioning = seek;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
            if (FAILED(control)) {
                operation_error = true;
                std::lock_guard lock(state->mutex);
                state->snapshot.control_error = control;
                state->playing = false;
                state->steps = 0;
            }
        }
        if ((!ready || busy) && std::chrono::steady_clock::now() > deadline) {
            if (!ready) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); fail(hr); break;
            }
            // A control request timing out must not tear down an already open
            // media item. A later seek/play request can recover the player.
            busy = false;
            positioning = false;
            operation_error = true;
            std::lock_guard lock(state->mutex);
            state->snapshot.control_error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            state->snapshot.busy = false;
            state->playing = false;
            state->steps = 0;
        }
        // Repaint paused frames as well after exposure/resize; no provider calls on UI.
        if (ready && state->repaint.exchange(false)) player->UpdateVideo();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (player) player->Shutdown();
    player.Reset();
    events.Reset();
    if (library) FreeLibrary(library);
    CoUninitialize();
}
} // namespace pulse::ui
