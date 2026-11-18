// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Host transport adapted from jimmgreen/pulse b32436831a066e18448b2810bef4bf989024fc68.

#include "file_dialog_bridge_host.h"
#include "app_commands.h"
#include "app_prefs.h"
#include "../common/localization.h"
#include "../common/picker_user_storage.h"
#include "../common/user_storage.h"
#include "../common/utf8_file.h"
#include "../ipc/picker_session.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/file_picker_dialog.h"
#include "../ui/file_save_access_policy.h"
#include "../ui/typography.h"

#include <ole2.h>
#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse::app {
namespace {
namespace wire = ipc::picker;
using I = l10n::StringId;

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
        return !(request.options & wire::AllowMultiSelect);
    if (request.operation == wire::Operation::OpenFiles)
        return request.default_extension.empty() &&
            (request.options & (wire::ValidateLocalOpenAccessV1 | wire::SkipAccessValidation));
    return (request.options & wire::ValidateLocalSaveAccessV1) && (request.options & wire::OverwritePrompt);
}

bool CheckOpenFile(const std::wstring& source, uint32_t options, const std::atomic_bool& cancelled) {
    picker_user_storage::Path storage;
    if (!picker_user_storage::Parse(source, storage)) return false;
    const auto attributes = GetFileAttributesW(storage.value.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    const auto extension = std::filesystem::path(storage.value).extension().wstring();
    if (!(options & wire::NoDereferenceLinks) && _wcsicmp(extension.c_str(), L".lnk") == 0) return false;
    const bool probe = (options & wire::ValidateLocalOpenAccessV1) != 0;
    const auto error = picker_user_storage::CheckAncestors(storage, probe,
        [](const std::wstring& root) { return GetDriveTypeW(root.c_str()); },
        [](const std::wstring& part, DWORD& attrs) {
            attrs = GetFileAttributesW(part.c_str());
            return attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : DWORD{ERROR_SUCCESS};
        }, [&] { return cancelled.load(); });
    if (error || cancelled) return false;
    if (!probe) return true;
    wire::PipeHandle file(CreateFileW(storage.value.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file || GetFileType(file.Get()) != FILE_TYPE_DISK) return false;
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(file.Get(), FileBasicInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (picker_user_storage::kUnsupportedAttributes | FILE_ATTRIBUTE_DIRECTORY))) return false;
    std::wstring final_path(wire::kMaxStringUnits, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file.Get(), final_path.data(), static_cast<DWORD>(final_path.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return length && length < final_path.size() && !cancelled &&
        picker_user_storage::SamePath(storage.value, std::wstring_view(final_path.data(), length));
}

struct Validation {
    bool accepted = false;
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
            current = ui::CaptureSaveConfirmation(save, cancelled_check);
        }
        if (current.state == ui::SaveRevalidationState::Failed || !current.snapshot || cancelled) return result;
        if (current.state == ui::SaveRevalidationState::ConfirmOverwrite) {
            result.selection = std::move(selection);
            result.confirm = std::move(current.snapshot);
            return result;
        }
        if (!(request.options & wire::SaveSkipAccessValidation)) {
            auto checked = ui::ValidateLocalSaveAccessV1(*current.snapshot, cancelled_check, {},
                (request.options & wire::SaveRequireUnencrypted) != 0);
            if (checked.state == ui::SaveAccessState::NeedsConfirmation) {
                result.selection = std::move(selection);
                result.confirm = std::move(checked.confirmation);
                return result;
            }
            if (checked.state != ui::SaveAccessState::Ready) return result;
        }
        selection.paths = {current.snapshot->Path()};
    } else {
        for (const auto& path : selection.paths) {
            if (cancelled) return result;
            if (request.operation == wire::Operation::PickFolder) {
                const DWORD attributes = GetFileAttributesW(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return result;
            } else if (!CheckOpenFile(path, request.options, cancelled)) {
                return result;
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

wire::Response ShowBridgePicker(const wire::Request& request, HWND owner,
                               const std::shared_ptr<std::atomic_bool>& cancelled) {
    if (!Supports(request)) return Response(request, wire::Status::Unsupported, ERROR_NOT_SUPPORTED);

    AppPrefs prefs;
    const auto config = storage::ConfigurationRoot();
    std::wstring json;
    if (!config.empty() && ReadUtf8File(config + L"\\app.json", json)) prefs.FromJson(json);
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
    spec.filter_index = request.filter_index;
    for (const auto& filter : request.filters) spec.filters.push_back({Wide(filter.label), Wide(filter.pattern)});
    spec.allow_multiselect = (request.options & wire::AllowMultiSelect) != 0;
    spec.show_hidden = prefs.show_hidden_files || (request.options & wire::ShowHidden);
    bool use_windows = false;
    bool active_prompt = false;
    HWND picker = nullptr;
    std::shared_ptr<PendingValidation> pending;
    ui::FilePickerResult accepted;
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
    spec.on_ready = [&](HWND window) { picker = window; };
    spec.should_close = [&] { return cancelled->load(); };
    spec.on_use_windows = [&] { use_windows = true; };
    spec.begin_submit = [&](const ui::FilePickerResult& choice) {
        if (active_prompt || pending) return false;
        return start(choice, std::nullopt);
    };
    spec.poll_submit = [&]() -> std::optional<ui::PickerSubmitReply> {
        if (!pending) return std::nullopt;
        std::optional<Validation> outcome;
        {
            std::lock_guard lock(pending->mutex);
            outcome = std::exchange(pending->ready, std::nullopt);
        }
        if (!outcome) return std::nullopt;
        pending.reset();
        if (outcome->confirm && !*cancelled) {
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
            accepted = std::move(outcome->selection);
            return ui::PickerSubmitReply{true, false, {}};
        }
        return ui::PickerSubmitReply{false, true, l10n::Get(I::PickerSubmitFailed)};
    };
    ui::FilePickerResult result;
    const bool picked = !*cancelled && ui::ShowFilePicker(owner, spec, dark, accent, result);
    if (*cancelled) return Response(request, wire::Status::Cancelled);
    if (use_windows) return Response(request, wire::Status::UseNative);
    if (!picker) return Response(request, wire::Status::Unsupported, ERROR_NOT_SUPPORTED);
    if (!picked || accepted.paths.empty()) return Response(request, wire::Status::Cancelled);
    auto reply = Response(request, wire::Status::Accepted);
    reply.filter_index = static_cast<uint32_t>(accepted.filter_index);
    for (const auto& path : accepted.paths) reply.paths.emplace_back(path.begin(), path.end());
    return reply;
}
} // namespace

int RunFileDialogBridgeHost(const wchar_t* pipe, const wchar_t* parent_pid, const wchar_t* owner_window) {
    uint64_t pid = 0, owner_value = 0;
    if (!pipe || !ParseNumber(parent_pid, MAXDWORD, pid) || !pid ||
        (owner_window && !ParseNumber(owner_window, std::numeric_limits<uintptr_t>::max(), owner_value))) return 2;
    wire::PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, static_cast<DWORD>(pid)));
    wire::Channel channel;
    if (!parent || !wire::Connect(pipe, parent.Get(), channel)) return 3;
    wire::Message message;
    if (!wire::ReceivePickerSessionRequest(channel, message)) return 3;
    const auto* request = std::get_if<wire::Request>(&message.body);
    if (!request) return 3;
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return 3;
    struct OleGuard { ~OleGuard() { OleUninitialize(); } } ole_guard;
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    std::atomic<DWORD> read_error = ERROR_SUCCESS;
    PipeReader receiver;
    if (!receiver.stop) return 3;
    try {
        receiver.thread = std::thread([&] {
            wire::Message packet;
            const auto result = channel.Receive(packet, {receiver.stop.Get(), INFINITE});
            if (!result && WaitForSingleObject(receiver.stop.Get(), 0) == WAIT_OBJECT_0) return;
            if (!result) read_error = result.error;
            else if (packet.request_id != message.request_id || !std::holds_alternative<wire::Cancel>(packet.body))
                read_error = ERROR_INVALID_DATA;
            *cancelled = true;
        });
    } catch (...) { return 3; }
    if (request->required_capabilities & wire::CallerWindow) owner_value = request->caller_window;
    HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner_value));
    DWORD owner_pid = 0;
    if (!GetWindowThreadProcessId(owner, &owner_pid) || owner_pid != pid) owner = nullptr;
    auto result = ShowBridgePicker(*request, owner, cancelled);
    *cancelled = true;
    receiver.Stop();
    if (const DWORD error = read_error.load()) result = Response(*request, wire::Status::Failed, error);
    return channel.Send({message.request_id, std::move(result)}, {nullptr, 5000}) ? 0 : 4;
}

} // namespace pulse::app
