// SPDX-License-Identifier: Apache-2.0
// Adapted from jimmgreen/pulse at b32436831a066e18448b2810bef4bf989024fc68.
// Modified for this Pulse distribution. See LICENSE and NOTICE.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace pulse::ipc::picker {

constexpr uint16_t kVersion = 1;
constexpr uint32_t kMaxMessageBytes = 1024 * 1024;
constexpr uint32_t kMaxStringUnits = 32768;
constexpr uint32_t kMaxFilters = 64;
constexpr uint32_t kMaxPaths = 1024;
enum class Operation : uint32_t { OpenFiles = 1, PickFolder, SaveFile };
// NoDereferenceLinks requires PreserveLinks: older peers reject the unknown
// required capability instead of silently returning a shortcut target.
// AdvisoryFileFilters is OpenFiles-only and requires AdvisoryFilters plus nonempty
// Filters. Unknown required capabilities fail closed on older peers. Absent option
// retains strict acceptance; this does not change which rows the filters display.
// SkipAccessValidation requires ExplicitAccessPolicy and only applies to OpenFiles.
// It does not waive existence/type validation or certify default access parity.
// LocalOpenAccessV1 is a versioned, restricted ordinary-local-file access policy,
// not native validation parity. Its matching option is mandatory; old peers reject it.
// LocalSaveAccessV1 is the SaveFile counterpart of LocalOpenAccessV1: the same
// local-save validation the in-process provider applies (ValidateLocalSaveAccessV1
// is mandatory with it), plus the native save semantics in the Save* options.
// DialogLabels carries the caller's title / OK / file-name labels; the label
// section is on the wire only when it is required, so old peers fail closed.
enum Capability : uint32_t { Open = 1, Folder = 2, Save = 4, MultiSelect = 8, Filters = 16, PreserveLinks = 32, AdvisoryFilters = 64, ExplicitAccessPolicy = 128, LocalOpenAccessV1 = 256,
                             LocalSaveAccessV1 = 512, DialogLabels = 1024, CallerWindow = 2048, DialogEvents = 4096, DialogButton = 8192 };
constexpr uint32_t kRequestCapabilities = Open | Folder | Save | MultiSelect | Filters | PreserveLinks | AdvisoryFilters | ExplicitAccessPolicy | LocalOpenAccessV1 |
    LocalSaveAccessV1 | DialogLabels | CallerWindow | DialogEvents | DialogButton;
constexpr uint32_t kSessionVersion = 2;
// Version 3 lets the host return the result while its dialog still owns the foreground.
constexpr uint32_t kSessionForegroundHandoff = 3;
constexpr uint32_t kSessionBasic = 1;
constexpr uint32_t kSessionFeatures = kSessionBasic;
enum Option : uint32_t { AllowMultiSelect = 1, ShowHidden = 2, OverwritePrompt = 4, NoDereferenceLinks = 8, AdvisoryFileFilters = 16, SkipAccessValidation = 32, ValidateLocalOpenAccessV1 = 64,
                         ValidateLocalSaveAccessV1 = 128, SaveNativeDefaults = 256, SaveNoTestFileCreate = 512, SaveRequireExistingParent = 1024,
                         SaveRequireUnencrypted = 2048, SaveSkipAccessValidation = 4096 };
enum LabelFlag : uint32_t { LabelTitle = 1, LabelOkButton = 2, LabelFileName = 4 };
constexpr uint32_t kMaxLabelUnits = 1024;
enum class Status : uint32_t { Accepted = 1, Cancelled, Unsupported, Failed, UseNative };
enum class ErrorSource : uint32_t { None, Protocol, Win32, HResult };
enum class CodecError { None, Malformed, UnsupportedVersion, UnsupportedCapability, InvalidSemantics };
using RequestId = std::array<uint8_t, 16>;
struct Filter {
    std::u16string label;
    std::u16string pattern;
    bool operator==(const Filter&) const = default;
};
struct Request {
    Operation operation = Operation::OpenFiles;
    uint32_t required_capabilities = Open;
    uint32_t options = 0;
    std::u16string initial_directory;
    std::u16string filename;
    std::u16string default_extension;
    std::vector<Filter> filters;
    uint32_t filter_index = 0; // Zero-based; zero when filters is empty.
    // DialogLabels only: which labels the caller set, and their text (a set
    // label may be empty; an unset one must be).
    uint32_t label_flags = 0;
    std::u16string title;
    std::u16string ok_button_label;
    std::u16string file_name_label;
    // CallerWindow only: the caller's window (an HWND value) for a host that
    // has no command line to receive it (a standby host). Nonzero when present.
    uint64_t caller_window = 0;
    std::u16string button_label;
    bool operator==(const Request&) const = default;
};
struct Response {
    Operation operation = Operation::OpenFiles;
    Status status = Status::Cancelled;
    ErrorSource error_source = ErrorSource::None;
    uint32_t error_code = 0;
    uint32_t filter_index = 0;
    std::vector<std::u16string> paths;
    bool operator==(const Response&) const = default;
};
struct Cancel { bool operator==(const Cancel&) const = default; };
struct SessionHello {
    uint32_t min_version = kSessionVersion;
    uint32_t max_version = kSessionVersion;
    uint32_t required_features = kSessionBasic;
    uint32_t required_capabilities = Open;
    bool operator==(const SessionHello&) const = default;
};
struct SessionWelcome {
    uint32_t version = kSessionVersion;
    uint32_t features = kSessionFeatures;
    uint32_t capabilities = kRequestCapabilities;
    bool operator==(const SessionWelcome&) const = default;
};
enum class EventKind : uint32_t {
    FolderChanging = 1, FolderChanged, SelectionChanged, TypeChanged, FileOk, Overwrite,
    ControlActivating, ButtonClicked, Ready
};
// Each event is answered before the host sends the next. The snapshot belongs
// to this callback, so caller getters never have to make a nested pipe request.
struct DialogEvent {
    uint32_t serial = 0;
    EventKind kind{};
    uint64_t window = 0;
    uint32_t filter_index = 0;
    std::u16string folder;
    std::u16string filename;
    std::u16string target;
    std::vector<std::u16string> selections;
    std::vector<std::u16string> candidates;
    bool operator==(const DialogEvent&) const = default;
};
struct DialogEventReply {
    uint32_t serial = 0;
    uint32_t result = 0;
    uint32_t overwrite = 0;
    bool operator==(const DialogEventReply&) const = default;
};
struct Message {
    RequestId request_id{};
    std::variant<Request, Response, Cancel, SessionHello, SessionWelcome, DialogEvent, DialogEventReply> body;
    bool operator==(const Message&) const = default;
};
// Output is unchanged on failure. Decode validates wire semantics; callers must
// additionally bind a response to its original request with ValidateResponse.
CodecError Encode(const Message& message, std::vector<uint8_t>& output);
CodecError Decode(std::span<const uint8_t> bytes, Message& output);
bool ValidateResponse(const Message& request, const Message& response);
bool ValidateDialogEvent(const Request& request, const DialogEvent& event);
Message MakeSessionWelcome(const Message& hello);
bool ValidateSessionWelcome(const Message& hello, const Message& welcome);
bool ValidateSessionRequest(const Message& hello, const Message& welcome, const Message& request);

} // namespace pulse::ipc::picker
