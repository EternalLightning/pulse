// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.

#include "native_dialog_takeover.h"
#include "app_file_picker.h"
#include "app_prefs.h"
#include "file_picker_places.h"
#include "native_file_dialog_adapter.h"
#include "../common/file_dialog_bridge_bypass.h"
#include "../common/localization.h"
#include "../common/display_path.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../fs/fs_enum.h"

#include <ole2.h>
#include <d2d1helper.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
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
    model::FileVisibility visibility;
    D2D1_COLOR_F accent{};
    bool verify_copies = false;
    bool zebra_rows = true;
};

struct PickerRequest {
    bool cancel = false;
    ui::FilePickerResult selection;
    uint64_t revision = 0;
};

struct Session {
    Session(NativeFileDialog dialog, uint64_t generation)
        : source(std::move(dialog)), generation(generation) {}

    const NativeFileDialog source;
    const uint64_t generation;
    std::atomic<bool> finishing{false};
    std::atomic<bool> returned{false};
    std::mutex mutex;
    bool can_request = true;
    std::atomic<uint64_t> request_revision{0};
    std::optional<PickerRequest> request;
    std::optional<ui::PickerSubmitReply> reply;
    ULONGLONG confirmation_deadline = 0;
};

bool SourceOpen(const Session& session) {
    const auto& source = session.source;
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
    return shared.source_process ? shared.fixture_foreground.load() : GetForegroundWindow();
}

bool QueueAction(const std::shared_ptr<NativeDialogTakeover::Shared>& shared,
                 const std::shared_ptr<Session>& session, PickerRequest request) {
    std::lock_guard lock(session->mutex);
    if (!IsCurrent(*shared, *session) || (!request.cancel && !session->can_request) ||
        !SourceOpen(*session))
        return false;
    session->can_request = false;
    request.revision = ++session->request_revision;
    session->reply.reset();
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
        spec.ok_button_label = source.ok_button_label;
        spec.filename = source.filename;
        spec.filters = source.filters;
        spec.filter_index = source.filter_index;
        spec.allow_multiselect = source.allow_multiselect;
        if (source.allow_multiselect)
            spec.multiselect_directory = path::FriendlyPathText(fs::NormalizePath(source.initial_path));
        spec.visibility = appearance.visibility;
        spec.verify_copies = appearance.verify_copies;
        spec.zebra_rows = appearance.zebra_rows;
        AppPrefs prefs;
        const auto config = storage::ConfigurationRoot();
        std::wstring json;
        if (!config.empty() && ReadUtf8File(config + L"\\app.json", json)) prefs.FromJson(json);
        SetFilePickerFolderViews(spec, prefs);
        spec.software_device = true;
        spec.cover_source = true;
        spec.external_handoff = true;
        spec.load_places = LoadTakeoverPickerPlaces;
        spec.begin_submit = [&](const ui::FilePickerResult& result) {
            return QueueAction(shared, session, {false, result});
        };
        spec.begin_cancel = [&] { return QueueAction(shared, session, {true, {}}); };
        spec.poll_submit = [&] {
            std::lock_guard lock(session->mutex);
            return std::exchange(session->reply, std::nullopt);
        };
        spec.should_close = [&] {
            return !IsCurrent(*shared, *session) || !SourceOpen(*session);
        };
        const HWND owner = GetWindow(source.window, GW_OWNER);
        DWORD owner_pid = 0;
        const DWORD owner_thread = GetWindowThreadProcessId(owner, &owner_pid);
        const auto return_target = [&]() -> HWND {
            if (SourceOpen(*session)) {
                if (const HWND popup = ValidationPopup(source)) return popup;
                return source.window;
            }
            DWORD pid = 0;
            if (!owner_thread || GetWindowThreadProcessId(owner, &pid) != owner_thread || pid != owner_pid) return nullptr;
            for (HWND target = owner; target; target = GetWindow(target, GW_OWNER)) {
                if (!GetWindowThreadProcessId(target, &pid) || pid != owner_pid) return nullptr;
                if (IsWindowVisible(target) && !IsIconic(target)) return target;
            }
            return nullptr;
        };
        ULONGLONG handoff_deadline = 0;
        spec.defer_hide = [&](HWND dialog, bool) {
            return ui::DeferFilePickerHide(dialog, return_target(), handoff_deadline);
        };
        spec.on_foreground_close = [&] {
            if (const HWND target = return_target(); target && IsWindowEnabled(target))
                SetForegroundWindow(target);
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
    if (session.request) return;
    session.can_request = retry;
    session.reply = ui::PickerSubmitReply{accepted, retry, accepted ? std::wstring() : l10n::Get(error)};
}

void ServiceSession(const std::shared_ptr<NativeDialogTakeover::Shared>& shared, Session& session) {
    if (!IsCurrent(*shared, session)) return;
    if (!SourceOpen(session)) {
        if (session.confirmation_deadline) Reply(session, true, false);
        session.finishing = true;
        return;
    }
    std::optional<PickerRequest> request;
    {
        std::lock_guard lock(session.mutex);
        if (session.request && session.request->cancel) session.confirmation_deadline = 0;
        if (!session.confirmation_deadline) request = std::exchange(session.request, std::nullopt);
    }
    if (session.confirmation_deadline) {
        if (ValidationPopup(session.source)) {
            session.finishing = true;
        } else if (GetTickCount64() >= session.confirmation_deadline) {
            session.confirmation_deadline = 0;
            Reply(session, false, false, I::PickerConfirmationPending);
        }
        return;
    }
    if (!request) return;
    const auto cancelled = [&] {
        return session.request_revision != request->revision ||
            !IsCurrent(*shared, session) || session.returned || !SourceOpen(session);
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
        if (shared->source_process && !reason.empty())
            std::fwprintf(stderr, L"[DIAG] Native picker submission: %ls\n", reason.c_str());
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
    // Keep the picker on one UI thread so consecutive dialogs reuse its graphics device.
    std::mutex picker_mutex;
    std::condition_variable_any picker_ready;
    std::shared_ptr<Session> next_picker;
    std::jthread picker_thread;
    const auto queue_picker = [&] {
        if (!picker_thread.joinable()) {
            picker_thread = std::jthread([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    std::unique_lock lock(picker_mutex);
                    if (!picker_ready.wait(lock, stop, [&] { return next_picker != nullptr; })) return;
                    auto task = std::move(next_picker);
                    lock.unlock();
                    RunPicker(shared, task);
                }
            });
        }
        {
            std::lock_guard lock(picker_mutex);
            next_picker = session;
        }
        picker_ready.notify_one();
    };
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
        if (session && session->returned) session.reset();
        if (enabled && IsEnabled(*shared)) {
            std::erase_if(handled, [&](const auto& entry) {
                return GetWindowThreadProcessId(entry.first, nullptr) != entry.second ||
                    (!IsWindowVisible(entry.first) && (!session || session->source.window != entry.first));
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
                        try { queue_picker(); }
                        catch (...) { session->returned = true; }
                    }
                } else retry_at = now + kSettleMs;
            }
        }
        const DWORD timeout = session || candidate ? kPollMs : enabled ? kIdlePollMs : INFINITE;
        WaitForSingleObject(shared->wake, timeout);
    }
    if (session) session->finishing = true;
    if (picker_thread.joinable()) {
        picker_thread.request_stop();
        picker_thread.join();
    }
    shared->exited = true;
}
} // namespace

NativeDialogTakeover::~NativeDialogTakeover() { Stop(); }

bool NativeDialogTakeover::Configure(bool enabled, bool dark, D2D1_COLOR_F accent, model::FileVisibility visibility,
                                     std::wstring pulse_folder, bool verify_copies, bool zebra_rows) {
    return ConfigureImpl(enabled, dark, accent, visibility, std::move(pulse_folder), 0, verify_copies, zebra_rows);
}

bool NativeDialogTakeover::StartForTesting(DWORD source_process) {
    if (!source_process || source_process == GetCurrentProcessId()) return false;
    return ConfigureImpl(true, false, D2D1::ColorF(0x0078d4), {}, {}, source_process, false, true);
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

bool NativeDialogTakeover::ConfigureImpl(bool enabled, bool dark, D2D1_COLOR_F accent, model::FileVisibility visibility,
                                         std::wstring pulse_folder, DWORD source_process, bool verify_copies, bool zebra_rows) {
    std::lock_guard owner(mutex_);
    if (stopped_) return !enabled;
    if (shared_ && shared_->source_process != source_process) return false;
    if (!shared_ && !enabled) return true;
    if (!shared_) {
        auto shared = std::make_shared<Shared>(source_process);
        if (!shared->wake) return false;
        try { watcher_ = std::thread(WatchDialogs, shared); }
        catch (...) { return false; }
        shared_ = std::move(shared);
    }
    {
        std::lock_guard lock(shared_->appearance_mutex);
        shared_->appearance = {dark, visibility, accent, verify_copies, zebra_rows};
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
    // Watcher and picker only reference Shared; neither calls the owning object.
    if (watcher_.joinable()) watcher_.join();
}

} // namespace pulse::app
