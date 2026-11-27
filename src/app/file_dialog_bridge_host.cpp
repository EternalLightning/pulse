// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Host transport adapted from jimmgreen/pulse b32436831a066e18448b2810bef4bf989024fc68.

#include "file_dialog_bridge_host.h"
#include "app_file_picker.h"
#include "file_picker_places.h"
#include "app_commands.h"
#include "app_prefs.h"
#include "link_resolve.h"
#include "../common/display_path.h"
#include "../common/file_operation_error.h"
#include "../common/known_folder_labels.h"
#include "../common/localization.h"
#include "../common/picker_user_storage.h"
#include "../common/picker_shell_namespace.h"
#include "../common/shell_id.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../ipc/picker_client.h"
#include "../ipc/picker_session.h"
#include "../fs/device_source.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/file_picker_dialog.h"
#include "../ui/file_save_access_policy.h"
#include "../ui/typography.h"

#include <ole2.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
namespace wire = ipc::picker;
using I = l10n::StringId;

enum class StartupStage : uint32_t { Caller = 1, Connect, Negotiate, Apartment, Events };

int StartupFailure(StartupStage stage, DWORD error) {
    return static_cast<int>((static_cast<uint32_t>(stage) << 16) | (error & 0xffffu));
}

std::wstring Wide(std::u16string_view text) {
    return {text.begin(), text.end()};
}

bool ParseNumber(const wchar_t* text, uint64_t maximum, uint64_t& number) {
    number = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9') return false;
        const auto digit = static_cast<uint64_t>(*text - L'0');
        if (number > (maximum - digit) / 10u) return false;
        number = number * 10u + digit;
    }
    return true;
}

wire::Response Response(const wire::Request& request, wire::Status status, DWORD error = ERROR_SUCCESS) {
    wire::Response result;
    result.operation = request.operation;
    result.status = status;
    if (error) {
        result.error_source = wire::ErrorSource::Win32;
        result.error_code = error;
    }
    return result;
}

bool Supports(const wire::Request& request) {
    if (request.operation == wire::Operation::PickFolder)
        return true;
    if (request.operation == wire::Operation::OpenFiles)
        return request.default_extension.empty() &&
            (request.options & (wire::ValidateLocalOpenAccessV1 | wire::SkipAccessValidation));
    return (request.options & wire::ValidateLocalSaveAccessV1) && (request.options & wire::OverwritePrompt) &&
        !(request.options & wire::SaveNoTestFileCreate);
}

bool CheckOpenFile(const std::wstring& source, uint32_t options, const std::atomic_bool& cancelled) {
    picker_user_storage::Path storage;
    if (!picker_user_storage::Parse(source, storage)) return false;
    const auto normalized = fs::NormalizePath(storage.value);
    const auto attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const bool probe = (options & wire::ValidateLocalOpenAccessV1) != 0;
    const auto error = picker_user_storage::CheckAncestors(storage, probe,
        [](const std::wstring& root) { return GetDriveTypeW(root.c_str()); },
        [](const std::wstring& part, DWORD& attrs) {
            attrs = GetFileAttributesW(fs::NormalizePath(part).c_str());
            return attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : DWORD{ERROR_SUCCESS};
        }, [&] { return cancelled.load(); });
    if (error || cancelled) return false;
    if (!probe) return true;
    wire::PipeHandle file(CreateFileW(normalized.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file || GetFileType(file.Get()) != FILE_TYPE_DISK) return false;
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(file.Get(), FileBasicInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (picker_user_storage::kUnsupportedAttributes | FILE_ATTRIBUTE_DIRECTORY))) return false;
    std::wstring final_path(wire::kMaxStringUnits, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file.Get(), final_path.data(), static_cast<DWORD>(final_path.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length) return length < final_path.size() && !cancelled &&
        picker_user_storage::SamePath(storage.value, std::wstring_view(final_path.data(), length));
    if (GetLastError() != ERROR_UNRECOGNIZED_VOLUME || cancelled) return false;

    // A provider can expose a normalized NT path without a DOS volume name.
    // Resolve the selected root in that namespace too, so the comparison
    // still verifies the volume and the complete relative path.
    wire::PipeHandle root(CreateFileW(fs::NormalizePath(storage.root).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!root || cancelled) return false;
    std::wstring expected(wire::kMaxStringUnits, L'\0');
    constexpr DWORD nt_path = FILE_NAME_NORMALIZED | VOLUME_NAME_NT;
    const DWORD root_length = GetFinalPathNameByHandleW(root.Get(), expected.data(), static_cast<DWORD>(expected.size()), nt_path);
    if (!root_length || root_length >= expected.size() || cancelled) return false;
    expected.resize(root_length);
    if (!expected.ends_with(L'\\')) expected += L'\\';
    auto relative = std::wstring_view(storage.value).substr(storage.root_length);
    if (relative.starts_with(L'\\')) relative.remove_prefix(1);
    expected += relative;
    const DWORD actual_length = GetFinalPathNameByHandleW(file.Get(), final_path.data(), static_cast<DWORD>(final_path.size()), nt_path);
    return actual_length && actual_length < final_path.size() && !cancelled &&
        CompareStringOrdinal(final_path.data(), static_cast<int>(actual_length),
            expected.c_str(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

struct Validation {
    bool accepted = false;
    DWORD error = ERROR_SUCCESS;
    ui::FilePickerResult selection;
    std::optional<ui::SaveConfirmationSnapshot> confirm;
};

Validation ValidateSelection(const wire::Request& request, ui::FilePickerResult selection,
                             const std::optional<ui::SaveConfirmationSnapshot>& consent,
                             const std::atomic_bool& cancelled) {
    Validation result;
    if (selection.paths.empty() || selection.paths.size() > wire::kMaxPaths || cancelled) return result;
    if (request.operation == wire::Operation::SaveFile) {
        ui::SaveRevalidationResult current;
        const auto cancelled_check = [&] { return cancelled.load(); };
        if (consent) {
            current = ui::RevalidateSaveConfirmation(*consent, cancelled_check);
        } else {
            const std::filesystem::path chosen(selection.paths.front());
            ui::SaveTargetRequest save;
            save.directory = chosen.parent_path().wstring();
            save.filename = chosen.filename().wstring();
            for (const auto& filter : request.filters) save.filters.push_back({Wide(filter.label), Wide(filter.pattern)});
            save.filter_index = selection.filter_index;
            save.filter_changed = selection.filter_index != request.filter_index;
            save.default_extension = Wide(request.default_extension);
            save.native_save_defaults = (request.options & wire::SaveNativeDefaults) != 0;
            save.no_test_file_create = (request.options & wire::SaveNoTestFileCreate) != 0;
            save.require_existing_save_parent = (request.options & wire::SaveRequireExistingParent) != 0;
            save.caller_validates_access = (request.options & wire::SaveSkipAccessValidation) != 0;
            save.allow_legacy_identity = true;
            current = ui::CaptureSaveConfirmation(save, cancelled_check);
        }
        if (cancelled) return result;
        if (current.state == ui::SaveRevalidationState::Failed || !current.snapshot) {
            result.error = current.error;
            return result;
        }
        if (current.state == ui::SaveRevalidationState::ConfirmOverwrite) {
            result.selection = std::move(selection);
            result.confirm = std::move(current.snapshot);
            return result;
        }
        if (!(request.options & wire::SaveSkipAccessValidation)) {
            const bool require_unencrypted = (request.options & wire::SaveRequireUnencrypted) != 0;
            auto checked = ui::ValidateSaveAccess(*current.snapshot, cancelled_check, {}, require_unencrypted);
            if (checked.state != ui::SaveAccessState::Ready) {
                result.error = checked.error;
                result.selection = std::move(selection);
                result.confirm = std::move(checked.confirmation);
                return result;
            }
        }
        selection.paths = {current.snapshot->Path()};
    } else {
        for (auto& selected_path : selection.paths) {
            if (cancelled) return result;
            if (request.operation == wire::Operation::PickFolder) {
                const DWORD attributes = GetFileAttributesW(fs::NormalizePath(selected_path).c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return result;
            } else {
                if (!(request.options & wire::NoDereferenceLinks) &&
                    _wcsicmp(std::filesystem::path(selected_path).extension().c_str(), L".lnk") == 0) {
                    fs::DirEntry shortcut;
                    if (!ResolveLink(fs::NormalizePath(selected_path), shortcut, [&] { return cancelled.load(); }))
                        return result;
                    selected_path = path::FriendlyPathText(shortcut.link_target);
                }
                if (cancelled || !CheckOpenFile(selected_path, request.options, cancelled)) return result;
            }
        }
    }
    if (!cancelled) {
        result.accepted = true;
        result.selection = std::move(selection);
    }
    return result;
}

struct PendingValidation {
    std::mutex mutex;
    std::optional<Validation> ready;
};

struct PipeReader {
    ~PipeReader() { Stop(); }
    void Stop() {
        SetEvent(stop.Get());
        if (thread.joinable()) thread.join();
    }
    wire::PipeHandle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::thread thread;
};

// Sends the upstream-style event snapshots on the authenticated picker channel.
// A slow caller callback can keep the current action pending without blocking
// this dialog's message loop or accumulating more work on the pipe.
class DialogEvents {
public:
    DialogEvents(wire::Channel& channel, const wire::RequestId& id, std::atomic_bool& cancelled,
                 std::atomic<DWORD>& error)
        : channel_(channel), id_(id), cancelled_(cancelled), error_(error) {}
    ~DialogEvents() { Stop(); }
    bool Start() {
        if (!stop_) return false;
        writer_ = std::thread([&] {
            try { Write(); } catch (...) { Fail(ERROR_NOT_ENOUGH_MEMORY); }
        });
        return true;
    }
    void Stop() {
        SetEvent(stop_.Get());
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        changed_.notify_one();
        if (writer_.joinable()) writer_.join();
    }
    uint32_t Queue(wire::DialogEvent event, bool needs_reply = false) {
        std::lock_guard lock(mutex_);
        if (stopped_ || cancelled_) return 0;
        if (queue_.size() >= 32 || next_ == MAXDWORD) {
            Fail(ERROR_BUFFER_OVERFLOW);
            return 0;
        }
        event.serial = ++next_;
        queue_.push_back({std::move(event), needs_reply});
        changed_.notify_one();
        return next_;
    }
    bool Read(const wire::DialogEventReply& reply) {
        std::lock_guard lock(mutex_);
        if (reply.serial != active_ || !active_ ||
            (reply.overwrite && active_kind_ != wire::EventKind::Overwrite)) return false;
        if (active_needs_reply_) replies_.push_back(reply);
        active_ = 0;
        changed_.notify_one();
        return true;
    }
    std::optional<wire::DialogEventReply> Take(uint32_t serial) {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find(replies_, serial, &wire::DialogEventReply::serial);
        if (found == replies_.end()) return std::nullopt;
        const auto reply = *found;
        replies_.erase(found);
        return reply;
    }
private:
    struct Pending {
        wire::DialogEvent event;
        bool needs_reply;
    };
    void Fail(DWORD error) {
        if (WaitForSingleObject(stop_.Get(), 0) != WAIT_TIMEOUT) return;
        error_.store(error);
        cancelled_.store(true);
    }
    void Write() {
        for (;;) {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return stopped_ || (!active_ && !queue_.empty()); });
            if (stopped_) return;
            auto pending = std::move(queue_.front());
            queue_.pop_front();
            active_ = pending.event.serial;
            active_needs_reply_ = pending.needs_reply;
            active_kind_ = pending.event.kind;
            lock.unlock();
            const auto sent = channel_.Send({id_, std::move(pending.event)}, {stop_.Get(), 5000});
            if (!sent) { Fail(sent.error); return; }
        }
    }
    wire::Channel& channel_;
    const wire::RequestId& id_;
    std::atomic_bool& cancelled_;
    std::atomic<DWORD>& error_;
    wire::PipeHandle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Pending> queue_;
    std::vector<wire::DialogEventReply> replies_;
    uint32_t next_ = 0, active_ = 0;
    wire::EventKind active_kind_{};
    bool active_needs_reply_ = false, stopped_ = false;
    std::thread writer_;
};

wire::Response ShowBridgePicker(const wire::Request& request, HWND owner, DWORD caller_pid, bool& return_foreground,
                               const std::shared_ptr<std::atomic_bool>& cancelled, DialogEvents* events,
                               const std::function<bool(wire::Response)>& publish_result) {
    if (!Supports(request)) return Response(request, wire::Status::Unsupported, ERROR_NOT_SUPPORTED);

    AppPrefs prefs;
    const auto config = storage::ConfigurationRoot();
    std::wstring json;
    if (!config.empty() && ReadUtf8File(config + L"\\app.json", json)) prefs.FromJson(json);
    path::InitializeKnownFolderLabels();
    l10n::Initialize(GetModuleHandleW(nullptr), prefs.language);
    ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(prefs.text_render));
    const bool dark = ui::ShouldUseDarkMode(prefs.theme_mode == 1 ? ui::ThemeMode::Light :
        prefs.theme_mode == 2 ? ui::ThemeMode::Dark : ui::ThemeMode::Auto);
    const auto accent = ResolveAccentColor(prefs, dark);

    ui::FilePickerSpec spec;
    spec.mode = request.operation == wire::Operation::PickFolder ? ui::PickerMode::Folder :
        request.operation == wire::Operation::SaveFile ? ui::PickerMode::Save : ui::PickerMode::Open;
    spec.initial_path = Wide(request.initial_directory);
    spec.filename = Wide(request.filename);
    spec.title = Wide(request.title);
    spec.ok_button_label = Wide(request.ok_button_label);
    spec.file_name_label = Wide(request.file_name_label);
    spec.custom_button_label = Wide(request.button_label);
    spec.filter_index = request.filter_index;
    for (const auto& filter : request.filters) spec.filters.push_back({Wide(filter.label), Wide(filter.pattern)});
    spec.allow_multiselect = (request.options & wire::AllowMultiSelect) != 0;
    const bool force_hidden = (request.options & wire::ShowHidden) != 0;
    spec.visibility = {force_hidden || prefs.show_hidden_files, force_hidden || prefs.show_protected_os_files};
    spec.verify_copies = prefs.verify_copies;
    spec.zebra_rows = prefs.list_zebra_rows;
    SetFilePickerFolderViews(spec, prefs);
    spec.load_places = LoadTakeoverPickerPlaces;
    bool use_windows = false;
    std::wstring native_folder;
    if (request.required_capabilities & wire::NativeOpenFolder) {
        spec.load_places = [](const auto& cancelled) {
            auto places = LoadTakeoverPickerPlaces(cancelled);
            if (cancelled() || !fs::AnyPortableDevicePresent()) return places;
            const picker_shell::Apartment apartment;
            if (FAILED(apartment.Result())) return places;
            for (auto& device : fs::MakeShellDeviceSource()->ListDevices()) {
                if (cancelled()) break;
                Microsoft::WRL::ComPtr<IShellItem> item;
                if (FAILED(shell_id::CreateItem(device.id, IID_PPV_ARGS(&item)))) continue;
                auto id = shell_id::FromItem(item.Get());
                if (!id.empty()) places.portable_devices.push_back({
                    std::move(device.name), std::move(id), L"\xE8EA", {}, false, true});
            }
            return places;
        };
        spec.on_open_native_folder = [&](std::wstring id) {
            native_folder = std::move(id);
            use_windows = true;
        };
    }
    bool active_prompt = false;
    HWND picker = nullptr;
    std::shared_ptr<PendingValidation> pending;
    std::optional<wire::Response> accepted;
    std::optional<ui::FilePickerSnapshot> snapshot;
    uint32_t folder_event = 0, file_ok_event = 0, overwrite_event = 0;
    std::optional<Validation> overwrite_choice;
    const auto send_event = [&](wire::EventKind kind, const ui::FilePickerSnapshot& state,
                                std::wstring_view target = {}, const wire::Response* candidate = nullptr,
                                bool needs_reply = false) {
        wire::DialogEvent event;
        event.kind = kind;
        event.window = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(picker));
        event.filter_index = candidate ? candidate->filter_index : static_cast<uint32_t>(state.filter_index);
        event.folder.assign(state.directory.begin(), state.directory.end());
        event.filename.assign(state.filename.begin(), state.filename.end());
        event.target.assign(target.begin(), target.end());
        for (const auto& path : state.selections) event.selections.emplace_back(path.begin(), path.end());
        if (candidate) event.candidates = candidate->paths;
        return events->Queue(std::move(event), needs_reply);
    };
    if (events) {
        spec.on_snapshot = [&](const ui::FilePickerSnapshot& state) {
            using enum wire::EventKind;
            if (!snapshot || snapshot->directory != state.directory) send_event(FolderChanged, state);
            if (!snapshot || snapshot->filter_index != state.filter_index) send_event(TypeChanged, state);
            if ((!snapshot && !state.selections.empty()) || (snapshot && snapshot->selections != state.selections))
                send_event(SelectionChanged, state);
            if (snapshot != state) snapshot = state;
        };
        spec.begin_folder_change = [&](const ui::FilePickerSnapshot& state, const std::wstring& directory) {
            folder_event = send_event(wire::EventKind::FolderChanging, state, directory, nullptr, true);
            return folder_event != 0;
        };
        spec.poll_folder_change = [&]() -> std::optional<bool> {
            if (const auto reply = events->Take(folder_event)) {
                const auto hr = static_cast<HRESULT>(reply->result);
                return hr == S_OK || hr == E_NOTIMPL;
            }
            return std::nullopt;
        };
        if (!request.button_label.empty()) {
            spec.on_custom_button = [&](const ui::FilePickerSnapshot& state) {
                send_event(wire::EventKind::ControlActivating, state);
                send_event(wire::EventKind::ButtonClicked, state);
            };
        }
    }
    const auto start = [&](const ui::FilePickerResult& choice, std::optional<ui::SaveConfirmationSnapshot> consent) {
        if (*cancelled) return false;
        try {
            auto task = std::make_shared<PendingValidation>();
            std::thread([task, request, choice, consent = std::move(consent), cancelled] {
                Validation result;
                try { result = ValidateSelection(request, choice, consent, *cancelled); }
                catch (...) { }
                std::lock_guard lock(task->mutex);
                task->ready = std::move(result);
            }).detach();
            pending = std::move(task);
            return true;
        } catch (...) {
            return false;
        }
    };
    spec.external_handoff = owner != nullptr;
    spec.on_ready = [&](HWND window) {
        picker = window;
        if (events) {
            ui::FilePickerSnapshot initial;
            initial.filter_index = request.filter_index;
            send_event(wire::EventKind::Ready, initial);
        }
    };
    spec.should_close = [&] { return cancelled->load(); };
    spec.begin_cancel = [&] {
        cancelled->store(true);
        return true;
    };
    spec.on_use_windows = [&] { use_windows = true; };
    DWORD owner_pid = 0;
    const DWORD owner_thread = GetWindowThreadProcessId(owner, &owner_pid);
    const auto owner_available = [&] {
        DWORD current_pid = 0;
        return owner_thread && GetWindowThreadProcessId(owner, &current_pid) == owner_thread && current_pid == owner_pid &&
            IsWindowVisible(owner) && !IsIconic(owner);
    };
    ULONGLONG handoff_deadline = 0;
    if (publish_result) spec.defer_hide = [&](HWND dialog, bool picked) {
        if (use_windows || !picker || !owner_available()) return false;
        if (!handoff_deadline) {
            if (GetForegroundWindow() != dialog) return false;
            auto result = picked && accepted ? *accepted : Response(request, wire::Status::Cancelled);
            if (!publish_result(std::move(result))) return false;
        }
        return ui::DeferFilePickerHide(dialog, owner, handoff_deadline);
    };
    spec.on_foreground_close = [&] {
        if (owner_available() && IsWindowEnabled(owner) && SetForegroundWindow(owner)) return;
        return_foreground = AllowSetForegroundWindow(caller_pid) != FALSE;
    };
    spec.begin_submit = [&](const ui::FilePickerResult& choice) {
        if (active_prompt || pending) return false;
        return start(choice, std::nullopt);
    };
    spec.poll_submit = [&]() -> std::optional<ui::PickerSubmitReply> {
        if (*cancelled) return std::nullopt;
        if (file_ok_event) {
            const auto reply = events->Take(file_ok_event);
            if (!reply) return std::nullopt;
            file_ok_event = 0;
            const auto hr = static_cast<HRESULT>(reply->result);
            if (hr == S_OK || hr == E_NOTIMPL) return ui::PickerSubmitReply{true, false, {}};
            accepted.reset();
            return ui::PickerSubmitReply{false, true, {}, true};
        }
        std::optional<Validation> outcome;
        uint32_t overwrite = 0;
        if (overwrite_event) {
            const auto reply = events->Take(overwrite_event);
            if (!reply) return std::nullopt;
            overwrite_event = 0;
            overwrite = reply->overwrite;
            outcome = std::exchange(overwrite_choice, std::nullopt);
        } else if (pending) {
            {
                std::lock_guard lock(pending->mutex);
                outcome = std::exchange(pending->ready, std::nullopt);
            }
            if (!outcome) return std::nullopt;
            pending.reset();
            if (events && snapshot && outcome->confirm) {
                overwrite_event = send_event(wire::EventKind::Overwrite, *snapshot, outcome->confirm->Path(), nullptr, true);
                if (!overwrite_event) return ui::PickerSubmitReply{false, true, {}};
                overwrite_choice = std::move(outcome);
                return std::nullopt;
            }
        }
        if (!outcome) return std::nullopt;
        if (outcome->confirm && !*cancelled) {
            if (overwrite == 1 && start(outcome->selection, std::move(outcome->confirm))) return std::nullopt;
            if (overwrite == 2) return ui::PickerSubmitReply{false, true, {}, true};
            ui::ConfirmDialogSpec confirmation;
            confirmation.title = l10n::Get(I::PickerOverwriteTitle);
            const auto filename = std::filesystem::path(outcome->confirm->Path()).filename().wstring();
            confirmation.message = l10n::Format(I::PickerOverwriteMessage, filename.c_str());
            confirmation.confirm_text = l10n::Get(I::Replace);
            confirmation.danger = true;
            confirmation.cancel_is_default = true;
            confirmation.still_valid = [&] { return !*cancelled && IsWindow(picker); };
            active_prompt = true;
            const bool replace = ui::ShowConfirmDialog(picker, confirmation, dark, accent);
            active_prompt = false;
            if (replace && start(outcome->selection, std::move(outcome->confirm))) return std::nullopt;
            return ui::PickerSubmitReply{false, true, {}};
        }
        if (outcome->accepted) {
            auto reply = Response(request, wire::Status::Accepted);
            reply.filter_index = static_cast<uint32_t>(outcome->selection.filter_index);
            for (const auto& path : outcome->selection.paths) reply.paths.emplace_back(path.begin(), path.end());
            std::vector<uint8_t> encoded;
            if (wire::Encode({wire::RequestId{1}, reply}, encoded) != wire::CodecError::None)
                return ui::PickerSubmitReply{false, true, l10n::Pick(
                    L"选择的文件路径过长或过多，请减少所选文件。", L"The selection is too large. Select fewer files.")};
            accepted = std::move(reply);
            if (events && snapshot) {
                file_ok_event = send_event(wire::EventKind::FileOk, *snapshot, {}, &*accepted, true);
                if (file_ok_event) return std::nullopt;
                accepted.reset();
                return ui::PickerSubmitReply{false, true, {}};
            }
            return ui::PickerSubmitReply{true, false, {}};
        }
        if (outcome->error) return ui::PickerSubmitReply{false, true,
            format::FileOperationErrorText(HRESULT_FROM_WIN32(outcome->error))};
        return ui::PickerSubmitReply{false, true, l10n::Get(I::PickerSubmitFailed)};
    };
    ui::FilePickerResult result;
    const bool picked = !*cancelled && ui::ShowFilePicker(owner, spec, dark, accent, result);
    if (*cancelled) return Response(request, wire::Status::Cancelled);
    if (use_windows) {
        auto reply = Response(request, wire::Status::UseNative);
        if (!native_folder.empty()) reply.paths.emplace_back(native_folder.begin(), native_folder.end());
        return reply;
    }
    if (!picker) return Response(request, wire::Status::Unsupported, ERROR_NOT_SUPPORTED);
    return picked && accepted ? std::move(*accepted) : Response(request, wire::Status::Cancelled);
}
} // namespace

int RunFileDialogBridgeHost(const wchar_t* pipe, const wchar_t* parent_pid, const wchar_t* owner_window,
                            const wchar_t* parent_token) {
    uint64_t pid = 0, owner_value = 0;
    if (!pipe || !ParseNumber(parent_pid, MAXDWORD, pid) || !pid ||
        (owner_window && !ParseNumber(owner_window, std::numeric_limits<uintptr_t>::max(), owner_value))) return 2;
    wire::PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, static_cast<DWORD>(pid)));
    if (!parent) return StartupFailure(StartupStage::Caller, GetLastError());
    wire::PipeHandle query;
    if (parent_token) {
        uint64_t token_value = 0;
        if (!ParseNumber(parent_token, (std::numeric_limits<uintptr_t>::max)(), token_value) || !token_value)
            return StartupFailure(StartupStage::Connect, ERROR_INVALID_HANDLE);
        query.Reset(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(token_value)));
        DWORD flags = 0;
        if (!GetHandleInformation(query.Get(), &flags) || !(flags & HANDLE_FLAG_INHERIT) ||
            !SetHandleInformation(query.Get(), HANDLE_FLAG_INHERIT, 0))
            return StartupFailure(StartupStage::Connect, ERROR_INVALID_HANDLE);
    }
    wire::Channel channel;
    constexpr std::wstring_view prefix = L"handle:";
    const bool inherited_connection = std::wstring_view(pipe).starts_with(prefix);
    wire::TransportResult connected;
    if (inherited_connection) {
        uint64_t endpoint = 0;
        if (!ParseNumber(pipe + prefix.size(), (std::numeric_limits<uintptr_t>::max)(), endpoint) || !endpoint)
            return StartupFailure(StartupStage::Connect, ERROR_INVALID_HANDLE);
        connected = wire::ConnectInherited(wire::PipeHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(endpoint))),
                                             parent.Get(), channel, query.Get());
    } else {
        connected = wire::Connect(pipe, parent.Get(), channel, {nullptr, 10000}, query.Get());
    }
    query.Reset();
    if (!connected)
        return StartupFailure(StartupStage::Connect, connected.error);
    wire::Message message;
    bool foreground_handoff = false;
    if (const auto received = wire::ReceivePickerSessionRequest(channel, message, foreground_handoff); !received)
        return StartupFailure(StartupStage::Negotiate, received.error);
    const auto* request = std::get_if<wire::Request>(&message.body);
    if (!request) return 3;
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return StartupFailure(StartupStage::Apartment, HRESULT_CODE(ole));
    struct OleGuard { ~OleGuard() { OleUninitialize(); } } ole_guard;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::atomic<DWORD> read_error = ERROR_SUCCESS;
    std::optional<DialogEvents> events;
    PipeReader receiver;
    if (!receiver.stop) return StartupFailure(StartupStage::Events, ERROR_NOT_ENOUGH_MEMORY);
    try {
        if (request->required_capabilities & wire::DialogEvents) {
            events.emplace(channel, message.request_id, *cancelled, read_error);
            if (!events->Start()) return StartupFailure(StartupStage::Events, ERROR_NOT_ENOUGH_MEMORY);
        }
        receiver.thread = std::thread([&] {
            try {
                for (;;) {
                    wire::Message packet;
                    const auto result = channel.Receive(packet, {receiver.stop.Get(), INFINITE});
                    if (!result && WaitForSingleObject(receiver.stop.Get(), 0) == WAIT_OBJECT_0) return;
                    if (!result) { read_error = result.error; break; }
                    if (packet.request_id == message.request_id) {
                        if (std::holds_alternative<wire::Cancel>(packet.body)) break;
                        const auto* reply = std::get_if<wire::DialogEventReply>(&packet.body);
                        if (events && reply && events->Read(*reply)) continue;
                    }
                    read_error = ERROR_INVALID_DATA;
                    break;
                }
            } catch (...) { read_error = ERROR_NOT_ENOUGH_MEMORY; }
            *cancelled = true;
        });
    } catch (...) { return StartupFailure(StartupStage::Events, ERROR_NOT_ENOUGH_MEMORY); }
    if (request->required_capabilities & wire::CallerWindow) owner_value = request->caller_window;
    HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner_value));
    DWORD owner_pid = 0;
    bool owner_valid = !owner || GetWindowThreadProcessId(owner, &owner_pid);
    if (owner_valid && owner && owner_pid != pid) {
        DWORD owner_session = 0, host_session = 0;
        // Only use this as a placement reference. The client on our inherited
        // channel holds the owner guard and monitors identity and modality.
        owner_valid = inherited_connection && !IsWindowEnabled(owner) &&
            ProcessIdToSessionId(owner_pid, &owner_session) &&
            ProcessIdToSessionId(GetCurrentProcessId(), &host_session) && owner_session == host_session;
    }
    const auto send = [&](wire::Response result) {
        *cancelled = true;
        if (events) events->Stop();
        receiver.Stop();
        if (const DWORD error = read_error.load()) result = Response(*request, wire::Status::Failed, error);
        return static_cast<bool>(channel.Send({message.request_id, std::move(result)}, {nullptr, 5000}));
    };
    bool published = false;
    std::jthread publisher;
    std::function<bool(wire::Response)> publish_result;
    if (foreground_handoff && owner_valid && owner) publish_result = [&](wire::Response result) {
        try {
            publisher = std::jthread([&, result = std::move(result)]() mutable {
                try { published = send(std::move(result)); } catch (...) {}
            });
            return true;
        } catch (const std::system_error&) { return false; }
    };
    bool return_foreground = false;
    auto result = !owner_valid
        ? Response(*request, wire::Status::Unsupported, ERROR_NOT_SUPPORTED)
        : ShowBridgePicker(*request, owner, static_cast<DWORD>(pid), return_foreground, cancelled,
                           events ? &*events : nullptr, publish_result);
    if (publisher.joinable()) {
        publisher.join();
        if (!published) return 4;
    } else if (!send(std::move(result))) return 4;
    return return_foreground ? wire::kPickerHostReturnForeground : 0;
}

} // namespace pulse::app
