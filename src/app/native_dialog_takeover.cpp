// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#include "native_dialog_takeover.h"
#include "native_file_dialog_adapter.h"
#include "../common/file_dialog_bridge_bypass.h"
#include "../common/localization.h"

#include <ole2.h>
#include <d2d1helper.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
using I = l10n::StringId;
constexpr DWORD kPollMs = 60;
constexpr ULONGLONG kSettleMs = 150;
constexpr ULONGLONG kInspectUntilMs = 900;
constexpr ULONGLONG kConfirmationMs = 8000;

struct Appearance {
    bool dark = false;
    bool show_hidden = false;
    D2D1_COLOR_F accent{};
};

struct PickerRequest {
    bool cancel = false;
    ui::FilePickerResult selection;
};

struct Session {
    Session(NativeFileDialog dialog, uint64_t generation)
        : source(std::move(dialog)), generation(generation) {}

    const NativeFileDialog source;
    const uint64_t generation;
    std::atomic<HWND> picker{nullptr};
    std::atomic<bool> finishing{false};
    std::atomic<bool> returned{false};
    std::mutex mutex;
    bool can_request = true;
    std::optional<PickerRequest> request;
    std::optional<ui::PickerSubmitReply> reply;
    ULONGLONG confirmation_deadline = 0;
    bool was_foreground = false;
};

bool SourceOpen(const NativeFileDialog& source) {
    DWORD pid = 0;
    return GetWindowThreadProcessId(source.window, &pid) == source.window_thread_id &&
        pid == source.process_id && IsWindowVisible(source.window);
}

HWND ValidationPopup(const NativeFileDialog& source) {
    if (IsWindowEnabled(source.window)) return nullptr;
    const HWND popup = GetWindow(source.window, GW_ENABLEDPOPUP);
    DWORD pid = 0;
    if (!popup || popup == source.window || !IsWindowVisible(popup) ||
        GetWindow(popup, GW_OWNER) != source.window ||
        !GetWindowThreadProcessId(popup, &pid) || pid != source.process_id) return nullptr;
    return popup;
}

bool DialogCandidate(HWND window, DWORD source_process = 0) {
    if (!window || GetAncestor(window, GA_ROOT) != window || !IsWindowVisible(window)) return false;
    DWORD pid = 0;
    wchar_t cls[32]{};
    return GetWindowThreadProcessId(window, &pid) && pid != GetCurrentProcessId() &&
        (!source_process || source_process == pid) &&
        GetClassNameW(window, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"#32770") == 0 &&
        !dialog_bridge::HasNativeFallback(pid);
}

BOOL CALLBACK RememberVisibleDialog(HWND window, LPARAM data) {
    if (DialogCandidate(window)) reinterpret_cast<std::set<HWND>*>(data)->insert(window);
    return TRUE;
}
} // namespace

struct NativeDialogTakeover::Shared {
    explicit Shared(DWORD source_process) : source_process(source_process) {}
    ~Shared() { if (wake) CloseHandle(wake); }
    const DWORD source_process;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> stopped{false};
    std::atomic<bool> watcher_failed{false};
    std::atomic<bool> ready{false};
    std::atomic<HWND> fixture_foreground{nullptr};
    std::mutex appearance_mutex;
    Appearance appearance;
    std::wstring pulse_folder;
};

namespace {
bool IsEnabled(const NativeDialogTakeover::Shared& shared) {
    return (shared.generation.load() & 1u) && !shared.stopped;
}

bool IsCurrent(const NativeDialogTakeover::Shared& shared, const Session& session) {
    return !shared.stopped && shared.generation == session.generation && !session.finishing;
}

HWND ForegroundForObservation(const NativeDialogTakeover::Shared& shared) {
    const HWND fixture = shared.fixture_foreground.load();
    return fixture ? fixture : GetForegroundWindow();
}

bool QueueAction(const std::shared_ptr<NativeDialogTakeover::Shared>& shared,
                 const std::shared_ptr<Session>& session, PickerRequest request) {
    std::lock_guard lock(session->mutex);
    if (!IsCurrent(*shared, *session) || !session->can_request || !SourceOpen(session->source))
        return false;
    session->can_request = false;
    session->request = std::move(request);
    SetEvent(shared->wake);
    return true;
}

void RunPicker(const std::shared_ptr<NativeDialogTakeover::Shared>& shared,
               const std::shared_ptr<Session>& session) {
    const HRESULT ole = OleInitialize(nullptr);
    if (SUCCEEDED(ole)) {
        const auto& source = session->source;
        Appearance appearance;
        ui::FilePickerSpec spec;
        {
            std::lock_guard lock(shared->appearance_mutex);
            appearance = shared->appearance;
            spec.initial_path = source.initial_path.empty() ? shared->pulse_folder : source.initial_path;
        }
        switch (source.kind) {
        case NativeDialogKind::Open: spec.mode = ui::PickerMode::Open; break;
        case NativeDialogKind::Save: spec.mode = ui::PickerMode::Save; break;
        case NativeDialogKind::Folder: spec.mode = ui::PickerMode::Folder; break;
        }
        spec.title = source.title;
        spec.filename = source.filename;
        spec.filters = source.filters;
        spec.filter_index = source.filter_index;
        spec.allow_multiselect = source.allow_multiselect;
        spec.show_hidden = appearance.show_hidden;
        spec.begin_submit = [&](const ui::FilePickerResult& result) {
            return QueueAction(shared, session, {false, result});
        };
        spec.begin_cancel = [&] { return QueueAction(shared, session, {true, {}}); };
        spec.poll_submit = [&] {
            std::lock_guard lock(session->mutex);
            return std::exchange(session->reply, std::nullopt);
        };
        spec.should_close = [&] {
            return !IsCurrent(*shared, *session) || !SourceOpen(source);
        };
        spec.on_ready = [&](HWND picker) {
            session->picker = picker;
            if (GetForegroundWindow() == source.window) SetForegroundWindow(picker);
            SetEvent(shared->wake);
        };
        spec.current_pulse_directory = [&] {
            std::lock_guard lock(shared->appearance_mutex);
            return shared->pulse_folder;
        };
        ui::FilePickerResult result;
        if (ForegroundForObservation(*shared) == source.window && !spec.should_close())
            ui::ShowFilePicker(source.window, spec, appearance.dark, appearance.accent, result);
        OleUninitialize();
    }
    session->returned = true;
    SetEvent(shared->wake);
}

void Reply(Session& session, bool accepted, bool retry, I error = I::PickerSubmitFailed) {
    std::lock_guard lock(session.mutex);
    session.can_request = retry;
    session.reply = ui::PickerSubmitReply{accepted, retry, accepted ? std::wstring() : l10n::Get(error)};
}

void ServiceSession(const std::shared_ptr<NativeDialogTakeover::Shared>& shared, Session& session) {
    if (!IsCurrent(*shared, session)) return;
    if (!SourceOpen(session.source)) {
        if (session.confirmation_deadline) Reply(session, true, false);
        session.finishing = true;
        return;
    }
    const HWND foreground = GetForegroundWindow();
    if (foreground && foreground == session.picker.load()) session.was_foreground = true;

    if (session.confirmation_deadline) {
        if (HWND popup = ValidationPopup(session.source)) {
            session.finishing = true;
            SetForegroundWindow(popup);
        } else if (GetTickCount64() >= session.confirmation_deadline) {
            session.confirmation_deadline = 0;
            Reply(session, false, false, I::PickerConfirmationPending);
        }
        return;
    }
    std::optional<PickerRequest> request;
    {
        std::lock_guard lock(session.mutex);
        request = std::exchange(session.request, std::nullopt);
    }
    if (!request) {
        if (session.was_foreground && foreground == session.source.window) session.finishing = true;
        return;
    }
    const auto cancelled = [&] {
        return !IsCurrent(*shared, session) || session.returned || !SourceOpen(session.source);
    };
    bool attempted = false;
    std::wstring reason;
    const bool sent = request->cancel
        ? CancelNativeDialog(session.source, reason, cancelled)
        : CommitNativeDialogSelection(session.source, request->selection.paths,
            request->selection.filter_index, reason, cancelled, &attempted);
    if (sent || attempted) {
        session.confirmation_deadline = GetTickCount64() + kConfirmationMs;
    } else if (!cancelled()) {
        Reply(session, false, true);
    }
}

struct Observer {
    explicit Observer(NativeDialogTakeover::Shared& state) : shared(state) {}

    NativeDialogTakeover::Shared& shared;
    std::set<HWND> handled;
    HWND candidate = nullptr;
    ULONGLONG observed = 0;
    ULONGLONG retry_at = 0;
    bool presenting = false;
};
thread_local Observer* observer = nullptr;

void ObserveWindow(DWORD event, HWND window, LONG object, LONG child) {
    if (!observer || !IsEnabled(observer->shared) || !window ||
        (event != EVENT_SYSTEM_FOREGROUND && (object != OBJID_WINDOW || child != CHILDID_SELF))) return;
    if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) {
        observer->handled.erase(window);
        if (observer->candidate == window) observer->candidate = nullptr;
        return;
    }
    if (ForegroundForObservation(observer->shared) != window || !DialogCandidate(window, observer->shared.source_process) ||
        observer->handled.contains(window) || observer->candidate == window) return;
    if (observer->presenting || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        observer->handled.insert(window);
        return;
    }
    observer->candidate = window;
    observer->observed = GetTickCount64();
    observer->retry_at = observer->observed + kSettleMs;
}

void CALLBACK OnWindow(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD) {
    ObserveWindow(event, window, object, child);
}

void WatchDialogs(const std::shared_ptr<NativeDialogTakeover::Shared>& shared) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { shared->watcher_failed = true; return; }
    Observer state{*shared};
    observer = &state;
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    const auto foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        OnWindow, shared->source_process, 0, flags);
    const auto objects = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr,
        OnWindow, shared->source_process, 0, flags);
    if (!foreground || !objects) shared->watcher_failed = true;
    std::shared_ptr<Session> session;
    uint64_t observed_generation = 0;
    HWND fixture_observed = nullptr;
    while (!shared->stopped && !shared->watcher_failed) {
        const uint64_t generation = shared->generation.load();
        const bool enabled = (generation & 1u) != 0;
        if (generation != observed_generation) {
            state.candidate = nullptr;
            state.handled.clear();
            if (enabled) EnumWindows(RememberVisibleDialog, reinterpret_cast<LPARAM>(&state.handled));
            if (session) session->finishing = true;
            observed_generation = generation;
            shared->ready = enabled;
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (const HWND fixture = shared->fixture_foreground.load(); fixture && fixture != fixture_observed) {
            fixture_observed = fixture;
            ObserveWindow(EVENT_SYSTEM_FOREGROUND, fixture, OBJID_WINDOW, CHILDID_SELF);
        }
        if (session && session->returned) {
            state.presenting = false;
            session.reset();
        }
        if (enabled && IsEnabled(*shared) && session) {
            ServiceSession(shared, *session);
        } else if (enabled && IsEnabled(*shared) && state.candidate) {
            const auto now = GetTickCount64();
            const HWND source = state.candidate;
            if (!DialogCandidate(source, shared->source_process)) {
                state.candidate = nullptr;
            } else if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                now - state.observed >= kInspectUntilMs) {
                state.handled.insert(source);
                state.candidate = nullptr;
            } else if (ForegroundForObservation(*shared) == source && now >= state.retry_at) {
                NativeFileDialog dialog;
                std::wstring reason;
                if (InspectNativeFileDialog(source, dialog, reason)) {
                    state.candidate = nullptr;
                    state.handled.insert(source);
                    if (ForegroundForObservation(*shared) == source && shared->generation == generation && IsEnabled(*shared)) {
                        session = std::make_shared<Session>(std::move(dialog), generation);
                        state.presenting = true;
                        try { std::thread(RunPicker, shared, session).detach(); }
                        catch (...) { session->returned = true; }
                    }
                } else state.retry_at = GetTickCount64() + kSettleMs;
            }
        }
        const DWORD timeout = (session || state.candidate) ? kPollMs : INFINITE;
        MsgWaitForMultipleObjects(1, &shared->wake, FALSE, timeout, QS_ALLINPUT);
    }
    if (session) session->finishing = true;
    if (objects) UnhookWinEvent(objects);
    if (foreground) UnhookWinEvent(foreground);
    observer = nullptr;
    CoUninitialize();
}
} // namespace

NativeDialogTakeover::~NativeDialogTakeover() { Stop(); }

bool NativeDialogTakeover::Configure(bool enabled, bool dark, D2D1_COLOR_F accent, bool show_hidden,
                                     std::wstring pulse_folder) {
    return ConfigureImpl(enabled, dark, accent, show_hidden, std::move(pulse_folder), 0);
}

bool NativeDialogTakeover::StartForTesting(DWORD source_process) {
    if (!source_process || source_process == GetCurrentProcessId()) return false;
    return ConfigureImpl(true, false, D2D1::ColorF(0x0078d4), false, {}, source_process);
}

bool NativeDialogTakeover::ReadyForTesting() const {
    std::lock_guard lock(mutex_);
    return shared_ && shared_->source_process && shared_->ready;
}

bool NativeDialogTakeover::ObserveForTesting(HWND source) {
    std::lock_guard lock(mutex_);
    DWORD process = 0;
    if (!shared_ || !shared_->source_process || !GetWindowThreadProcessId(source, &process) ||
        process != shared_->source_process) return false;
    shared_->fixture_foreground = source;
    return SetEvent(shared_->wake) != FALSE;
}

bool NativeDialogTakeover::ConfigureImpl(bool enabled, bool dark, D2D1_COLOR_F accent, bool show_hidden,
                                         std::wstring pulse_folder, DWORD source_process) {
    std::lock_guard owner(mutex_);
    if (stopped_) return !enabled;
    if (shared_ && shared_->source_process != source_process) return false;
    if (!shared_ && !enabled) return true;
    if (!shared_) {
        auto shared = std::make_shared<Shared>(source_process);
        if (!shared->wake) return false;
        try { std::thread(WatchDialogs, shared).detach(); }
        catch (...) { return false; }
        shared_ = std::move(shared);
    }
    {
        std::lock_guard lock(shared_->appearance_mutex);
        shared_->appearance = {dark, show_hidden, accent};
        if (!pulse_folder.empty()) shared_->pulse_folder = std::move(pulse_folder);
    }
    if (enabled != IsEnabled(*shared_)) {
        ++shared_->generation;
        SetEvent(shared_->wake);
    }
    return !enabled || !shared_->watcher_failed;
}

void NativeDialogTakeover::Stop() {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    if (!shared_) return;
    shared_->stopped = true;
    SetEvent(shared_->wake);
}

} // namespace pulse::app
