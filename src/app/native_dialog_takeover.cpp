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
#include <map>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
using I = l10n::StringId;
constexpr DWORD kPollMs = 60;
constexpr DWORD kIdlePollMs = 120;
constexpr ULONGLONG kSettleMs = 150;
constexpr ULONGLONG kInspectUntilMs = 900;
constexpr ULONGLONG kConfirmationMs = 8000;

struct AutomationApartment {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~AutomationApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

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
    std::atomic<bool> exited{false};
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
        spec.software_device = true;
        spec.begin_submit = [&](const ui::FilePickerResult& result) {
            return QueueAction(shared, session, {false, result});
        };
        spec.begin_cancel = [&] { return QueueAction(shared, session, {true, {}}); };
        spec.poll_submit = [&] {
            std::lock_guard lock(session->mutex);
            return std::exchange(session->reply, std::nullopt);
        };
        spec.should_close = [&] {
            return !IsCurrent(*shared, *session) || !SourceOpen(source) ||
                (!session->picker && ForegroundForObservation(*shared) != source.window);
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
    bool sent = false;
    if (request->cancel) {
        sent = CancelNativeDialog(session.source, reason, cancelled);
    } else {
        NativeFileDialog prepared;
        bool ready = false;
        {
            const AutomationApartment apartment;
            ready = SUCCEEDED(apartment.result) && PrepareNativeDialogSelection(session.source,
                request->selection.paths, request->selection.filter_index, prepared, reason, cancelled);
        }
        // Release accessibility providers while the native app still owns the dialog.
        if (ready && prepared.kind == NativeDialogKind::Folder && !prepared.legacy_folder) {
            const AutomationApartment apartment;
            sent = SUCCEEDED(apartment.result) && ConfirmNativeDialogSelection(prepared, reason, cancelled, attempted);
        } else if (ready) {
            sent = ConfirmNativeDialogSelection(prepared, reason, cancelled, attempted);
        }
    }
    if (sent || attempted) {
        session.confirmation_deadline = GetTickCount64() + kConfirmationMs;
    } else if (!cancelled()) {
        Reply(session, false, true);
    }
}

void WatchDialogs(const std::shared_ptr<NativeDialogTakeover::Shared>& shared) {
    HWND candidate = nullptr;
    ULONGLONG observed = 0;
    ULONGLONG retry_at = 0;
    std::map<HWND, DWORD> handled;
    const auto remember = [&](HWND window) {
        if (const DWORD thread = GetWindowThreadProcessId(window, nullptr))
            handled.insert_or_assign(window, thread);
    };
    std::shared_ptr<Session> session;
    std::thread picker_thread;
    uint64_t observed_generation = 0;
    while (!shared->stopped && !shared->watcher_failed) {
        const uint64_t generation = shared->generation.load();
        const bool enabled = (generation & 1u) != 0;
        if (generation != observed_generation) {
            candidate = nullptr;
            handled.clear();
            const HWND current = ForegroundForObservation(*shared);
            if (enabled && DialogCandidate(current, shared->source_process)) remember(current);
            if (session) session->finishing = true;
            observed_generation = generation;
            shared->ready = enabled;
        }
        if (session && session->returned) {
            if (picker_thread.joinable()) picker_thread.join();
            session.reset();
        }
        if (enabled && IsEnabled(*shared)) {
            std::erase_if(handled, [](const auto& entry) {
                return !IsWindowVisible(entry.first) || GetWindowThreadProcessId(entry.first, nullptr) != entry.second;
            });
            if (session) ServiceSession(shared, *session);
            const auto now = GetTickCount64();
            const HWND source = ForegroundForObservation(*shared);
            if (session || handled.contains(source) || !DialogCandidate(source, shared->source_process)) {
                candidate = nullptr;
            } else {
                if (source != candidate) {
                    candidate = source;
                    observed = now;
                    retry_at = now + kSettleMs;
                }
                if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || now - observed >= kInspectUntilMs) {
                    remember(source);
                    candidate = nullptr;
                }
            }
            if (candidate && now >= retry_at) {
                NativeFileDialog dialog;
                std::wstring reason;
                bool recognized = false;
                {
                    const AutomationApartment apartment;
                    if (FAILED(apartment.result)) { shared->watcher_failed = true; break; }
                    recognized = InspectNativeFileDialog(source, dialog, reason);
                }
                if (recognized) {
                    candidate = nullptr;
                    remember(source);
                    if (ForegroundForObservation(*shared) == source && shared->generation == generation && IsEnabled(*shared)) {
                        session = std::make_shared<Session>(std::move(dialog), generation);
                        try { picker_thread = std::thread(RunPicker, shared, session); }
                        catch (...) { session->returned = true; }
                    }
                } else retry_at = now + kSettleMs;
            }
        }
        const DWORD timeout = session || candidate ? kPollMs : enabled ? kIdlePollMs : INFINITE;
        WaitForSingleObject(shared->wake, timeout);
    }
    if (session) {
        session->finishing = true;
        if (picker_thread.joinable()) picker_thread.join();
    }
    shared->exited = true;
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

bool NativeDialogTakeover::StoppedForTesting() const {
    std::lock_guard lock(mutex_);
    return !shared_ || shared_->exited;
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
