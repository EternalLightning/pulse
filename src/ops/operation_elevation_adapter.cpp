// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "operation_elevation_adapter.h"
#include "operation_presentation.h"
#include "../common/localization.h"
#include "../common/file_operation_error.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <filesystem>
#include <objbase.h>

namespace pulse::ops {
namespace {
constexpr uint32_t kVersion = 2, kRequestMagic = 0x31514550, kSnapshotMagic = 0x31534550,
                   kControlMagic = 0x31434550;
constexpr uint32_t kMaxCount = 65536, kMaxString = 32767;
struct InvalidPayload {};
#ifdef PULSE_OPERATION_ELEVATION_TESTING
bool IsIsolatedRecycleFixture(const std::wstring& original, const std::wstring& payload) {
    std::error_code error;
    const auto destination_parent = std::filesystem::weakly_canonical(
        std::filesystem::path(original).parent_path(), error);
    if (error || destination_parent.filename() != L"destination")
        return false;
    const auto payload_parent = std::filesystem::weakly_canonical(
        std::filesystem::path(payload).parent_path(), error);
    if (error || payload_parent.filename() != L"recycle" ||
        payload_parent.parent_path() != destination_parent.parent_path())
        return false;
    const auto root = destination_parent.parent_path();
    const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), error);
    if (error || root.parent_path() != temporary ||
        !root.filename().wstring().starts_with(L"pulse-elevation-adapter-"))
        return false;
    for (const auto& directory : {root, std::filesystem::path(original).parent_path(),
                                  std::filesystem::path(payload).parent_path()}) {
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
    }
    const auto name = std::filesystem::path(payload).filename().wstring();
    return name.size() > 2 && name.starts_with(L"$R");
}
#endif
class Writer {
public:
    std::vector<uint8_t> bytes;
    template <class T> void Number(T value) {
        static_assert(std::is_arithmetic_v<T>);
        if (bytes.size() + sizeof(T) > kOperationElevationMaxPayload)
            throw InvalidPayload{};
        const auto* data = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(T));
    }
    void Bool(bool value) {
        Number<uint8_t>(value ? 1 : 0);
    }
    void String(const std::wstring& value) {
        if (value.size() > kMaxString || value.find(L'\0') != std::wstring::npos)
            throw InvalidPayload{};
        Number<uint32_t>(static_cast<uint32_t>(value.size()));
        for (wchar_t c : value)
            Number<uint16_t>(static_cast<uint16_t>(c));
    }
    template <class T, class F> void List(const std::vector<T>& values, F write) {
        if (values.size() > kMaxCount)
            throw InvalidPayload{};
        Number<uint32_t>(static_cast<uint32_t>(values.size()));
        for (const auto& value : values)
            write(value);
    }
    void Strings(const std::vector<std::wstring>& values) {
        List(values, [&](const auto& value) { String(value); });
    }
    void Identity(const FileIdentity& identity) {
        Bool(identity.extended);
        Number<uint64_t>(identity.value.VolumeSerialNumber);
        for (BYTE value : identity.value.FileId.Identifier)
            Number<uint8_t>(value);
    }
    void FileId(const FILE_ID_INFO& id) {
        Identity({id, true});
    }
    void Time(FILETIME value) {
        Number<uint32_t>(value.dwLowDateTime);
        Number<uint32_t>(value.dwHighDateTime);
    }
};
class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {
        if (bytes.size() > kOperationElevationMaxPayload)
            throw InvalidPayload{};
    }
    template <class T> T Number() {
        static_assert(std::is_arithmetic_v<T>);
        if (bytes_.size() - position_ < sizeof(T))
            throw InvalidPayload{};
        T value{};
        std::memcpy(&value, bytes_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return value;
    }
    bool Bool() {
        const auto value = Number<uint8_t>();
        if (value > 1)
            throw InvalidPayload{};
        return value != 0;
    }
    template <class E> E Enum(E maximum) {
        const auto value = Number<uint32_t>();
        if (value > static_cast<uint32_t>(maximum))
            throw InvalidPayload{};
        return static_cast<E>(value);
    }
    std::wstring String() {
        const auto count = Number<uint32_t>();
        if (count > kMaxString || count > (bytes_.size() - position_) / sizeof(uint16_t))
            throw InvalidPayload{};
        std::wstring value;
        value.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto c = Number<uint16_t>();
            if (!c)
                throw InvalidPayload{};
            value.push_back(static_cast<wchar_t>(c));
        }
        return value;
    }
    template <class T, class F> std::vector<T> List(F read) {
        const auto count = Number<uint32_t>();
        if (count > kMaxCount || count > bytes_.size() - position_)
            throw InvalidPayload{};
        std::vector<T> values;
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            values.push_back(read());
        return values;
    }
    std::vector<std::wstring> Strings() {
        return List<std::wstring>([&] { return String(); });
    }
    FileIdentity Identity(bool required = false) {
        FileIdentity value;
        value.extended = Bool();
        value.value.VolumeSerialNumber = Number<uint64_t>();
        for (auto& byte : value.value.FileId.Identifier)
            byte = Number<uint8_t>();
        if (required && !value.Valid())
            throw InvalidPayload{};
        if (!value.extended)
            for (size_t i = 8; i < 16; ++i)
                if (value.value.FileId.Identifier[i])
                    throw InvalidPayload{};
        return value;
    }
    FILE_ID_INFO FileId() {
        const auto value = Identity(true);
        if (!value.extended)
            throw InvalidPayload{};
        return value.value;
    }
    FILETIME Time() {
        FILETIME value;
        value.dwLowDateTime = Number<uint32_t>();
        value.dwHighDateTime = Number<uint32_t>();
        return value;
    }
    void End() {
        if (position_ != bytes_.size())
            throw InvalidPayload{};
    }
    void Header(uint32_t magic) {
        if (Number<uint32_t>() != magic || Number<uint32_t>() != kVersion)
            throw InvalidPayload{};
    }

private:
    std::span<const uint8_t> bytes_;
    size_t position_ = 0;
};
bool IsAbsolutePath(const std::wstring& value) {
    if (value.empty() || value.size() > kMaxString || value.find(L'\0') != std::wstring::npos ||
        value.find(L'/') != std::wstring::npos || value.starts_with(L"\\\\.\\"))
        return false;
    const size_t offset = value.starts_with(L"\\\\?\\") ? 4 : 0;
    if (value.size() >= offset + 3 &&
        ((value[offset] >= L'A' && value[offset] <= L'Z') ||
         (value[offset] >= L'a' && value[offset] <= L'z')) &&
        value[offset + 1] == L':' && value[offset + 2] == L'\\')
        return true;
    if (offset && !value.starts_with(L"\\\\?\\UNC\\"))
        return false;
    if (!offset && !value.starts_with(L"\\\\"))
        return false;
    const size_t begin = offset ? 8 : 2;
    const size_t server = value.find(L'\\', begin);
    if (server == std::wstring::npos || server == begin)
        return false;
    const size_t share = value.find(L'\\', server + 1);
    return server + 1 < value.size() && (share == std::wstring::npos || share > server + 1);
}
void Path(const std::wstring& value, bool optional = false) {
    if ((!optional || !value.empty()) && !IsAbsolutePath(value))
        throw InvalidPayload{};
}
void Paths(const std::vector<std::wstring>& values) {
    for (const auto& path : values)
        Path(path);
}
bool IsDelete(OpType type) {
    return type == OpType::RecycleDelete || type == OpType::RealDelete || type == OpType::EmptyRecycle;
}
bool IsTerminal(OpPhase phase) {
    return phase == OpPhase::Completed || phase == OpPhase::Failed || phase == OpPhase::Cancelled;
}
void Leaf(const std::wstring& value) {
    if (value.empty() || value == L"." || value == L".." ||
        value.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
        throw InvalidPayload{};
}
void ValidateRequest(const OpRequest& value) {
    Paths(value.sources);
    Path(value.dest_dir, true);
    Paths(value.recycle_paths);
    Paths(value.undo_cleanup_directories);
    if (value.type == OpType::Copy || value.type == OpType::Move) {
        if (value.sources.empty())
            throw InvalidPayload{};
        Path(value.dest_dir);
    }
    if (value.type == OpType::Rename || value.type == OpType::BatchRename) {
        if (value.sources.empty() || (value.type == OpType::Rename && value.sources.size() != 1))
            throw InvalidPayload{};
        if (value.type == OpType::Rename && value.sources[0].size() > 3)
            Leaf(value.new_name);
        if (value.type == OpType::BatchRename) {
            if (value.new_names.size() != value.sources.size())
                throw InvalidPayload{};
            for (const auto& name : value.new_names)
                Leaf(name);
        }
    }
    if (value.is_undo &&
        (value.type == OpType::Move || value.type == OpType::Rename || value.type == OpType::BatchRename) &&
        value.undo_produced_identities.size() != value.sources.size())
        throw InvalidPayload{};
    if (value.is_undo && value.type == OpType::Move) {
        if (value.new_names.size() != value.sources.size())
            throw InvalidPayload{};
        Paths(value.new_names);
    }
    if (value.type == OpType::CreateFolder || value.type == OpType::CreateTextFile) {
        if (value.sources.size() != 1)
            throw InvalidPayload{};
    }
    if (value.type == OpType::RestoreRecycle &&
        (value.sources.empty() || value.sources.size() != value.recycle_paths.size() ||
         value.sources.size() != value.recycle_undo_identities.size()))
        throw InvalidPayload{};
    for (const auto& target : value.delete_targets) {
        Path(target.path);
        Paths(target.physical_paths);
    }
    for (const auto& identity : value.undo_produced_identities)
        if (!identity.Valid())
            throw InvalidPayload{};
}
void WriteRequest(Writer& w, const OpRequest& value) {
    w.Number<uint32_t>(static_cast<uint32_t>(value.type));
    w.Strings(value.sources);
    w.String(value.dest_dir);
    w.String(value.new_name);
    w.Strings(value.new_names);
    w.Number<uint32_t>(static_cast<uint32_t>(value.collision_policy));
    w.Bool(value.is_undo);
    w.Number<uint64_t>(value.undo_revision);
    w.Number<uint32_t>(static_cast<uint32_t>(value.delete_origin));
    w.List(value.delete_targets, [&](const auto& target) {
        w.String(target.path);
        w.Strings(target.physical_paths);
        w.Bool(target.recycle_item);
    });
    w.Strings(value.recycle_paths);
    w.List(value.recycle_undo_identities, [&](const auto& identity) {
        w.FileId(identity.payload);
        w.FileId(identity.index);
    });
    w.List(value.undo_produced_identities, [&](const auto& identity) { w.Identity(identity); });
    w.Strings(value.undo_cleanup_directories);
}
OpRequest ReadRequest(Reader& r) {
    OpRequest value;
    value.type = r.Enum(OpType::BatchRename);
    value.sources = r.Strings();
    value.dest_dir = r.String();
    value.new_name = r.String();
    value.new_names = r.Strings();
    value.collision_policy = r.Enum(CollisionPolicy::KeepBoth);
    value.is_undo = r.Bool();
    value.undo_revision = r.Number<uint64_t>();
    value.delete_origin = r.Enum(DeleteOrigin::EmptyRecycle);
    value.delete_targets = r.List<DeleteRequestTarget>([&] {
        DeleteRequestTarget target;
        target.path = r.String();
        target.physical_paths = r.Strings();
        target.recycle_item = r.Bool();
        return target;
    });
    value.recycle_paths = r.Strings();
    value.recycle_undo_identities = r.List<RecycleUndoIdentity>([&] {
        RecycleUndoIdentity id;
        id.payload = r.FileId();
        id.index = r.FileId();
        return id;
    });
    value.undo_produced_identities = r.List<FileIdentity>([&] { return r.Identity(true); });
    value.undo_cleanup_directories = r.Strings();
    ValidateRequest(value);
    return value;
}
struct WireReference {
    FileIdentity identity;
    uint64_t handle = 0;
};
struct WirePlan {
    TransferPlanEntry entry;
    uint64_t handle = 0;
};
struct WireRequest {
    OpRequest request;
    uint64_t task_id = 0;
    uint32_t parent_pid = 0;
    uint64_t owner = 0;
    std::wstring language;
    bool verify = false, rename_identity = false;
    FileIdentity rename_source_id;
    std::vector<std::wstring> completed;
    std::vector<WirePlan> plan;
    std::vector<WireReference> references;
    std::vector<DestinationGuard::ElevationSnapshot> source_guard, destination_guard;
};
void WriteGuard(Writer& w, const std::vector<DestinationGuard::ElevationSnapshot>& entries) {
    w.List(entries, [&](const auto& entry) {
        const auto& value = entry.snapshot;
        w.String(entry.path);
        w.Bool(value.exists);
        w.Bool(value.directory);
        w.Number<uint32_t>(value.error_code);
        w.Identity(value.id);
        w.Number<uint32_t>(value.links);
        w.List(value.reparse, [&](BYTE byte) { w.Number<uint8_t>(byte); });
        w.Identity(value.resolved_id);
        w.String(value.resolved_path);
        w.Number<uint64_t>(entry.reference_handle);
        w.Number<uint64_t>(entry.resolved_reference_handle);
    });
}
std::vector<DestinationGuard::ElevationSnapshot> ReadGuard(Reader& r) {
    return r.List<DestinationGuard::ElevationSnapshot>([&] {
        DestinationGuard::ElevationSnapshot entry;
        auto& value = entry.snapshot;
        entry.path = r.String();
        Path(entry.path);
        value.exists = r.Bool();
        value.directory = r.Bool();
        value.error_code = r.Number<uint32_t>();
        value.id = r.Identity(value.exists);
        value.links = r.Number<uint32_t>();
        value.reparse = r.List<BYTE>([&] { return r.Number<uint8_t>(); });
        if (value.reparse.size() > MAXIMUM_REPARSE_DATA_BUFFER_SIZE)
            throw InvalidPayload{};
        value.resolved_id = r.Identity(!value.reparse.empty());
        value.resolved_path = r.String();
        entry.reference_handle = r.Number<uint64_t>();
        entry.resolved_reference_handle = r.Number<uint64_t>();
        if (value.exists && !entry.reference_handle)
            throw InvalidPayload{};
        if (!value.reparse.empty()) {
            Path(value.resolved_path);
            if (!entry.resolved_reference_handle)
                throw InvalidPayload{};
        }
        if (!value.exists &&
            (entry.reference_handle || entry.resolved_reference_handle || !value.reparse.empty()))
            throw InvalidPayload{};
        return entry;
    });
}
std::vector<uint8_t> EncodeRequest(const WireRequest& value) {
    Writer w;
    w.Number<uint32_t>(kRequestMagic);
    w.Number<uint32_t>(kVersion);
    WriteRequest(w, value.request);
    w.Number<uint64_t>(value.task_id);
    w.Number<uint32_t>(value.parent_pid);
    w.Number<uint64_t>(value.owner);
    w.String(value.language);
    w.Bool(value.verify);
    w.Bool(value.rename_identity);
    w.Identity(value.rename_source_id);
    w.Strings(value.completed);
    w.List(value.plan, [&](const auto& planned) {
        const auto& e = planned.entry;
        w.String(e.source);
        w.String(e.destination);
        w.Bool(e.directory);
        w.Bool(e.reparse);
        w.Bool(e.destination_preexisting);
        w.Bool(e.directory_rename_only);
        w.Number<uint64_t>(e.bytes);
        w.Number<uint32_t>(e.attributes);
        w.Time(e.created);
        w.Time(e.accessed);
        w.Time(e.modified);
        w.Identity(e.source_identity.identity);
        w.Number<uint64_t>(planned.handle);
    });
    w.List(value.references, [&](const auto& ref) {
        w.Identity(ref.identity);
        w.Number<uint64_t>(ref.handle);
    });
    WriteGuard(w, value.source_guard);
    WriteGuard(w, value.destination_guard);
    return std::move(w.bytes);
}
WireRequest DecodeRequest(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    r.Header(kRequestMagic);
    WireRequest value;
    value.request = ReadRequest(r);
    value.task_id = r.Number<uint64_t>();
    value.parent_pid = r.Number<uint32_t>();
    value.owner = r.Number<uint64_t>();
    if (!value.task_id || !value.parent_pid)
        throw InvalidPayload{};
    value.language = r.String();
    if (!l10n::IsLanguageId(value.language))
        throw InvalidPayload{};
    value.verify = r.Bool();
    value.rename_identity = r.Bool();
    value.rename_source_id = r.Identity(value.rename_identity);
    value.completed = r.Strings();
    Paths(value.completed);
    value.plan = r.List<WirePlan>([&] {
        WirePlan p;
        auto& e = p.entry;
        e.source = r.String();
        e.destination = r.String();
        Path(e.source);
        Path(e.destination);
        e.directory = r.Bool();
        e.reparse = r.Bool();
        e.destination_preexisting = r.Bool();
        e.directory_rename_only = r.Bool();
        e.bytes = r.Number<uint64_t>();
        e.attributes = r.Number<uint32_t>();
        e.created = r.Time();
        e.accessed = r.Time();
        e.modified = r.Time();
        e.source_identity.identity = r.Identity(true);
        p.handle = r.Number<uint64_t>();
        if (!p.handle || (e.directory_rename_only && (!e.directory || e.reparse)))
            throw InvalidPayload{};
        return p;
    });
    value.references = r.List<WireReference>([&] {
        WireReference ref;
        ref.identity = r.Identity();
        ref.handle = r.Number<uint64_t>();
        if ((ref.handle != 0) != ref.identity.Valid())
            throw InvalidPayload{};
        return ref;
    });
    if (!value.references.empty() && value.references.size() != value.request.sources.size())
        throw InvalidPayload{};
    value.source_guard = ReadGuard(r);
    value.destination_guard = ReadGuard(r);
    if (value.source_guard.empty())
        throw InvalidPayload{};
    const bool creation =
        value.request.type == OpType::CreateFolder || value.request.type == OpType::CreateTextFile;
    if (!creation && value.references.size() != value.request.sources.size())
        throw InvalidPayload{};
    if ((value.request.type == OpType::Copy || value.request.type == OpType::Move) &&
        value.destination_guard.empty())
        throw InvalidPayload{};
    r.End();
    return value;
}

void WriteStatus(Writer& w, const OpStatus& value) {
    w.Bool(value.active);
    w.Number<uint32_t>(static_cast<uint32_t>(value.type));
    w.Number<uint32_t>(static_cast<uint32_t>(value.phase));
    w.Number<uint64_t>(value.task_id);
    w.Number<float>(value.percent);
    w.String(value.summary);
    w.String(value.last_error);
    w.String(value.source_label);
    w.String(value.destination_label);
    w.String(value.current_item);
    w.Number<uint64_t>(value.total_bytes);
    w.Number<uint64_t>(value.transferred_bytes);
    w.Number<uint64_t>(value.total_items);
    w.Number<uint64_t>(value.completed_items);
    w.Number<double>(value.bytes_per_second);
    w.Number<double>(value.peak_bytes_per_second);
    w.Number<uint64_t>(value.eta_seconds);
    w.Number<uint64_t>(value.deletes_without_mutation);
    w.Number<uint32_t>(static_cast<uint32_t>(value.failure_hr));
    w.String(value.failed_path);
    w.List(value.restore_results, [&](const auto& item) {
        w.String(item.original_path);
        w.String(item.recycle_path);
        w.String(item.destination);
        w.Bool(item.restored);
        w.Number<uint32_t>(static_cast<uint32_t>(item.error));
    });
}
OpStatus ReadStatus(Reader& r) {
    OpStatus value;
    value.active = r.Bool();
    value.type = r.Enum(OpType::BatchRename);
    value.phase = r.Enum(OpPhase::Cancelled);
    value.task_id = r.Number<uint64_t>();
    value.percent = r.Number<float>();
    value.summary = r.String();
    value.last_error = r.String();
    value.source_label = r.String();
    value.destination_label = r.String();
    value.current_item = r.String();
    value.total_bytes = r.Number<uint64_t>();
    value.transferred_bytes = r.Number<uint64_t>();
    value.total_items = r.Number<uint64_t>();
    value.completed_items = r.Number<uint64_t>();
    value.bytes_per_second = r.Number<double>();
    value.peak_bytes_per_second = r.Number<double>();
    value.eta_seconds = r.Number<uint64_t>();
    value.deletes_without_mutation = r.Number<uint64_t>();
    value.failure_hr = static_cast<HRESULT>(r.Number<uint32_t>());
    value.failed_path = r.String();
    if (!std::isfinite(value.percent) || !std::isfinite(value.bytes_per_second) ||
        !std::isfinite(value.peak_bytes_per_second) || value.percent < -1 || value.percent > 100 ||
        value.bytes_per_second < 0 || value.peak_bytes_per_second < 0)
        throw InvalidPayload{};
    Path(value.failed_path, true);
    value.restore_results = r.List<RestoreItemResult>([&] {
        RestoreItemResult item;
        item.original_path = r.String();
        Path(item.original_path);
        item.recycle_path = r.String();
        item.destination = r.String();
        Path(item.recycle_path);
        Path(item.destination, true);
        item.restored = r.Bool();
        item.error = static_cast<HRESULT>(r.Number<uint32_t>());
        return item;
    });
    return value;
}
void WriteConflict(Writer& w, const ConflictItemInfo& value) {
    w.Number<uint64_t>(value.token);
    w.Number<uint64_t>(value.task_id);
    w.String(value.source);
    w.String(value.destination);
    w.Number<uint64_t>(value.source_size);
    w.Number<uint64_t>(value.destination_size);
    w.Time(value.source_modified);
    w.Time(value.destination_modified);
    w.Bool(value.source_is_directory);
    w.Bool(value.destination_is_directory);
    w.Number<uint64_t>(value.remaining);
    w.Bool(value.link_confirmation);
    w.Number<uint32_t>(static_cast<uint32_t>(value.link_impact.kind));
    w.String(value.link_impact.path);
    w.String(value.link_impact.resolved_path);
    w.Number<uint32_t>(value.link_impact.link_count);
    w.List(value.rows, [&](const ConflictRowInfo& row) {
        w.Number<uint64_t>(row.row_id);
        w.String(row.source);
        w.String(row.destination);
        w.Number<uint64_t>(row.source_size);
        w.Number<uint64_t>(row.destination_size);
        w.Time(row.source_modified);
        w.Time(row.destination_modified);
        w.Bool(row.source_is_directory);
        w.Bool(row.destination_is_directory);
        w.Bool(row.source_is_reparse);
        w.Bool(row.destination_is_reparse);
    });
}
ConflictItemInfo ReadConflict(Reader& r) {
    ConflictItemInfo value;
    value.token = r.Number<uint64_t>();
    value.task_id = r.Number<uint64_t>();
    value.source = r.String();
    value.destination = r.String();
    Path(value.destination);
    value.source_size = r.Number<uint64_t>();
    value.destination_size = r.Number<uint64_t>();
    value.source_modified = r.Time();
    value.destination_modified = r.Time();
    value.source_is_directory = r.Bool();
    value.destination_is_directory = r.Bool();
    value.remaining = static_cast<size_t>(r.Number<uint64_t>());
    value.link_confirmation = r.Bool();
    Path(value.source, value.link_confirmation);
    value.link_impact.kind = r.Enum(DestinationLinkKind::HardLink);
    value.link_impact.path = r.String();
    value.link_impact.resolved_path = r.String();
    value.link_impact.link_count = r.Number<uint32_t>();
    Path(value.link_impact.path, !value.link_confirmation);
    Path(value.link_impact.resolved_path, true);
    std::set<uint64_t> row_ids;
    value.rows = r.List<ConflictRowInfo>([&] {
        ConflictRowInfo row;
        row.row_id = r.Number<uint64_t>();
        if (!row.row_id || !row_ids.insert(row.row_id).second) throw InvalidPayload{};
        row.source = r.String();
        row.destination = r.String();
        Path(row.source);
        Path(row.destination);
        row.source_size = r.Number<uint64_t>();
        row.destination_size = r.Number<uint64_t>();
        row.source_modified = r.Time();
        row.destination_modified = r.Time();
        row.source_is_directory = r.Bool();
        row.destination_is_directory = r.Bool();
        row.source_is_reparse = r.Bool();
        row.destination_is_reparse = r.Bool();
        return row;
    });
    if (value.link_confirmation && !value.rows.empty()) throw InvalidPayload{};
    if (!value.token)
        throw InvalidPayload{};
    return value;
}
void WriteDelete(Writer& w, const DeleteConfirmation& value) {
    w.Number<uint64_t>(value.token);
    w.Number<uint64_t>(value.plan.task_id);
    w.Number<uint32_t>(static_cast<uint32_t>(value.plan.origin));
    w.List(value.plan.targets, [&](const auto& target) {
        w.String(target.path);
        w.Strings(target.physical_paths);
        w.Number<uint32_t>(static_cast<uint32_t>(target.disposition));
        w.String(target.reason);
    });
}
DeleteConfirmation ReadDelete(Reader& r) {
    DeleteConfirmation value;
    value.token = r.Number<uint64_t>();
    value.plan.task_id = r.Number<uint64_t>();
    value.plan.origin = r.Enum(DeleteOrigin::EmptyRecycle);
    value.plan.targets = r.List<DeleteTarget>([&] {
        DeleteTarget target;
        target.path = r.String();
        target.physical_paths = r.Strings();
        Path(target.path);
        Paths(target.physical_paths);
        target.disposition = r.Enum(DeleteDisposition::RecycleRequested);
        target.reason = r.String();
        return target;
    });
    if (!value.token || value.plan.targets.empty())
        throw InvalidPayload{};
    return value;
}
struct CompletionRecord {
    CompletedOperation completion;
    std::vector<FileIdentity> identities;
};
struct Snapshot {
    uint64_t serial = 0;
    OpStatus status;
    std::vector<CompletionRecord> completed;
    std::vector<DeleteOutcome> deletes;
    std::optional<ConflictItemInfo> conflict;
    std::optional<DeleteConfirmation> deletion;
};
std::vector<uint8_t> EncodeSnapshot(const Snapshot& value) {
    Writer w;
    w.Number<uint32_t>(kSnapshotMagic);
    w.Number<uint32_t>(kVersion);
    w.Number<uint64_t>(value.serial);
    WriteStatus(w, value.status);
    w.List(value.completed, [&](const auto& record) {
        const auto& c = record.completion;
        w.Number<uint32_t>(static_cast<uint32_t>(c.type));
        w.Number<uint64_t>(c.task_id);
        w.Strings(c.sources);
        w.Strings(c.destinations);
        w.Bool(c.volume_label.has_value());
        if (c.volume_label)
            w.String(*c.volume_label);
        w.List(record.identities, [&](const auto& id) { w.Identity(id); });
    });
    w.List(value.deletes, [&](const auto& d) {
        w.Number<uint64_t>(d.task_id);
        w.Bool(d.mutated);
        w.Bool(d.uncertain);
    });
    w.Bool(value.conflict.has_value());
    if (value.conflict)
        WriteConflict(w, *value.conflict);
    w.Bool(value.deletion.has_value());
    if (value.deletion)
        WriteDelete(w, *value.deletion);
    return std::move(w.bytes);
}
Snapshot DecodeSnapshot(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    r.Header(kSnapshotMagic);
    Snapshot value;
    value.serial = r.Number<uint64_t>();
    value.status = ReadStatus(r);
    value.completed = r.List<CompletionRecord>([&] {
        CompletionRecord record;
        auto& c = record.completion;
        c.type = r.Enum(OpType::BatchRename);
        c.task_id = r.Number<uint64_t>();
        c.sources = r.Strings();
        c.destinations = r.Strings();
        Paths(c.sources);
        Paths(c.destinations);
        if (r.Bool())
            c.volume_label = r.String();
        record.identities = r.List<FileIdentity>([&] { return r.Identity(true); });
        return record;
    });
    value.deletes = r.List<DeleteOutcome>([&] {
        DeleteOutcome d;
        d.task_id = r.Number<uint64_t>();
        d.mutated = r.Bool();
        d.uncertain = r.Bool();
        return d;
    });
    if (r.Bool())
        value.conflict = ReadConflict(r);
    if (r.Bool())
        value.deletion = ReadDelete(r);
    r.End();
    return value;
}
struct Control {
    bool cancel = false, paused = false;
    uint64_t conflict_token = 0;
    ConflictChoice choice = ConflictChoice::Cancel;
    bool apply_all = false;
    std::vector<ConflictResolution> conflict_rows;
    uint64_t delete_token = 0;
    bool delete_accepted = false;
};
std::vector<uint8_t> EncodeControl(const Control& value) {
    Writer w;
    w.Number<uint32_t>(kControlMagic);
    w.Number<uint32_t>(kVersion);
    w.Bool(value.cancel);
    w.Bool(value.paused);
    w.Number<uint64_t>(value.conflict_token);
    w.Number<uint32_t>(static_cast<uint32_t>(value.choice));
    w.Bool(value.apply_all);
    w.List(value.conflict_rows, [&](const ConflictResolution& row) {
        w.Number<uint64_t>(row.row_id);
        w.Number<uint32_t>(static_cast<uint32_t>(row.choice));
    });
    w.Number<uint64_t>(value.delete_token);
    w.Bool(value.delete_accepted);
    return std::move(w.bytes);
}
Control DecodeControl(std::span<const uint8_t> bytes) {
    Reader r(bytes);
    r.Header(kControlMagic);
    Control value;
    value.cancel = r.Bool();
    value.paused = r.Bool();
    value.conflict_token = r.Number<uint64_t>();
    value.choice = r.Enum(ConflictChoice::Continue);
    value.apply_all = r.Bool();
    std::set<uint64_t> row_ids;
    value.conflict_rows = r.List<ConflictResolution>([&] {
        ConflictResolution row;
        row.row_id = r.Number<uint64_t>();
        row.choice = r.Enum(ConflictChoice::Continue);
        if (!row.row_id || !row_ids.insert(row.row_id).second ||
            (row.choice != ConflictChoice::Replace && row.choice != ConflictChoice::Skip &&
             row.choice != ConflictChoice::KeepBoth)) throw InvalidPayload{};
        return row;
    });
    if (!value.conflict_rows.empty() && (!value.conflict_token || value.apply_all || value.cancel))
        throw InvalidPayload{};
    value.delete_token = r.Number<uint64_t>();
    value.delete_accepted = r.Bool();
    r.End();
    return value;
}
FileObjectReference DuplicateReference(HANDLE parent, uint64_t value, const FileIdentity& expected) {
    HANDLE raw = nullptr;
    if (!value || !expected.Valid() ||
        !DuplicateHandle(parent, reinterpret_cast<HANDLE>(value), GetCurrentProcess(), &raw, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        throw InvalidPayload{};
    FileObjectReference result;
    result.identity = expected;
    result.handle = std::make_shared<FileObjectReference::Lease>(raw);
    FileIdentity live;
    if (!ReadFileIdentity(raw, live) || live != expected)
        throw InvalidPayload{};
    return result;
}
std::wstring ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size())
        return {};
    path.resize(count);
    return path;
}
std::wstring ShellHelperPath(const std::wstring& executable) {
    if (executable.empty()) return {};
    auto path = std::filesystem::path(executable);
    path.replace_filename(L"Pulse.Shell.exe");
    return path.wstring();
}
} // namespace

struct OperationElevationAdapter::ParentState {
    OpsManager& manager;
    const OpsManager::QueueItem& item;
    uint64_t completed_ops = 0, serial = 0, deletes_without_mutation = 0;
    size_t imported_completions = 0, imported_deletes = 0;
    std::vector<std::wstring> proven_sources;
    std::atomic<bool> malformed{false};
};
struct OperationElevationAdapter::WorkerState {
    DWORD parent_pid = 0;
};
bool OperationElevationAdapter::ParentCancelled(void* raw) {
    const auto& state = *static_cast<ParentState*>(raw);
    return state.malformed || state.manager.stopping_.load() ||
           state.manager.shell_cancel_requested_.load() || state.manager.lock_retry_cancel_.load() ||
           state.manager.transfer_cancel_.load();
}
std::vector<uint8_t> OperationElevationAdapter::ParentProgress(void* raw, std::span<const uint8_t> bytes) {
    auto& state = *static_cast<ParentState*>(raw);
    auto& manager = state.manager;
    Control control;
    try {
        const auto snapshot = DecodeSnapshot(bytes);
        if (snapshot.serial <= state.serial || snapshot.status.task_id != state.item.seq ||
            snapshot.status.type != state.item.req.type ||
            snapshot.completed.size() < state.imported_completions ||
            snapshot.deletes.size() < state.imported_deletes)
            throw InvalidPayload{};
        if (snapshot.conflict && snapshot.conflict->task_id != state.item.seq)
            throw InvalidPayload{};
        if (snapshot.deletion && snapshot.deletion->plan.task_id != state.item.seq)
            throw InvalidPayload{};
        for (size_t i = state.imported_completions; i < snapshot.completed.size(); ++i) {
            const auto& record = snapshot.completed[i];
            auto completed = record.completion;
            if (completed.task_id != state.item.seq && completed.task_id != 0)
                throw InvalidPayload{};
            if (completed.type != state.item.req.type &&
                !(IsDelete(state.item.req.type) && IsDelete(completed.type)))
                throw InvalidPayload{};
            OpRequest req = state.item.req;
            req.type = completed.type;
            req.sources = completed.sources;
            if (req.is_undo) {
                req.undo_produced_identities.clear();
                if (req.type == OpType::RestoreRecycle) {
                    req.recycle_paths.clear();
                    req.recycle_undo_identities.clear();
                }
                for (const auto& source : req.sources) {
                    const auto found =
                        std::find(state.item.req.sources.begin(), state.item.req.sources.end(), source);
                    const size_t index = static_cast<size_t>(found - state.item.req.sources.begin());
                    if (found != state.item.req.sources.end() &&
                        index < state.item.req.undo_produced_identities.size())
                        req.undo_produced_identities.push_back(
                            state.item.req.undo_produced_identities[index]);
                    if (req.type == OpType::RestoreRecycle && index < state.item.req.recycle_paths.size() &&
                        index < state.item.req.recycle_undo_identities.size()) {
                        req.recycle_paths.push_back(state.item.req.recycle_paths[index]);
                        req.recycle_undo_identities.push_back(state.item.req.recycle_undo_identities[index]);
                    }
                }
            }
            manager.PushUndo(req, &completed.destinations, &record.identities);
            {
                std::lock_guard lock(manager.mutex_);
                const auto imported = std::find_if(
                    manager.completions_.rbegin(), manager.completions_.rend(), [&](const auto& c) {
                        return c.type == completed.type && c.sources == completed.sources &&
                               c.destinations == completed.destinations;
                    });
                if (imported != manager.completions_.rend()) {
                    imported->task_id = state.item.seq;
                    imported->volume_label = completed.volume_label;
                }
            }
            state.proven_sources.insert(state.proven_sources.end(), completed.sources.begin(),
                                        completed.sources.end());
            if (completed.type == OpType::RealDelete)
                manager.InvalidateRecycleUndo(completed.sources);
            ++state.imported_completions;
        }
        {
            std::lock_guard lock(manager.mutex_);
            for (size_t i = state.imported_deletes; i < snapshot.deletes.size(); ++i) {
                auto outcome = snapshot.deletes[i];
                if (outcome.task_id != state.item.seq)
                    throw InvalidPayload{};
                manager.delete_outcomes_.push_back(outcome);
                ++state.imported_deletes;
            }
            manager.status_ = snapshot.status;
            if (state.item.retry_batch_status)
                ApplyOperationContinuation(manager.status_, *state.item.retry_batch_status);
            manager.status_.completed_ops = state.completed_ops;
            manager.status_.deletes_without_mutation += state.deletes_without_mutation;
            manager.status_.lock_retry_available = false;
        }
        state.serial = snapshot.serial;
        if (manager.notify_)
            manager.notify_();
        control.cancel = ParentCancelled(raw);
        control.paused = manager.transfer_pause_.load();
        if (snapshot.conflict && !control.cancel) {
            const auto worker_token = snapshot.conflict->token;
            uint64_t token = 0;
            {
                std::lock_guard lock(manager.transfer_control_mutex_);
                manager.pending_conflict_ = snapshot.conflict;
                token = manager.next_conflict_token_++;
                manager.pending_conflict_->token = token;
                manager.resolved_conflict_token_ = 0;
                manager.resolved_conflict_rows_.clear();
                manager.pending_conflict_->still_valid = [&manager, task = state.item.seq, token] {
                    std::lock_guard guard(manager.transfer_control_mutex_);
                    return manager.pending_conflict_ && manager.pending_conflict_->task_id == task &&
                           manager.pending_conflict_->token == token;
                };
            }
            if (manager.notify_)
                manager.notify_();
            {
                std::unique_lock lock(manager.transfer_control_mutex_);
                while (manager.resolved_conflict_token_ != token && !ParentCancelled(raw))
                    manager.transfer_control_cv_.wait_for(lock, std::chrono::milliseconds(50));
                control.conflict_token = worker_token;
                control.cancel = ParentCancelled(raw);
                control.choice = control.cancel ? ConflictChoice::Cancel : manager.resolved_conflict_choice_;
                control.apply_all = !control.cancel && manager.resolved_conflict_apply_all_;
                if (!control.cancel) control.conflict_rows = std::move(manager.resolved_conflict_rows_);
                manager.resolved_conflict_rows_.clear();
                manager.pending_conflict_.reset();
            }
        }
        if (snapshot.deletion && !control.cancel) {
            const auto prepared = manager.delete_service_.Prepare(snapshot.deletion->plan);
            if (prepared.decision == DeleteDecision::Blocked)
                control.cancel = true;
            else {
                manager.delete_active_.store(true);
                if (manager.notify_)
                    manager.notify_();
                std::unique_lock lock(manager.delete_wait_mutex_);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
                while (manager.delete_service_.Pending() && !ParentCancelled(raw) &&
                       !manager.delete_cancel_.load()) {
                    manager.delete_wait_cv_.wait_for(lock, std::chrono::milliseconds(50));
                    if (std::chrono::steady_clock::now() >= deadline) {
                        manager.delete_service_.Cancel();
                        break;
                    }
                }
                control.delete_token = snapshot.deletion->token;
                control.delete_accepted = !ParentCancelled(raw) && !manager.delete_cancel_.load() &&
                                          manager.delete_service_.TakeAccepted(prepared.token).has_value();
                manager.delete_active_.store(false);
                manager.delete_service_.Cancel();
            }
        }
    } catch (...) {
        state.malformed = true;
        control.cancel = true;
    }
    return EncodeControl(control);
}

void OperationElevationAdapter::RunParentImpl(OpsManager& manager, const OpsManager::QueueItem& item,
                                              const std::wstring& helper, bool native_consent) {
    ParentState state{manager, item};
    {
        std::lock_guard lock(manager.mutex_);
        state.completed_ops = manager.status_.completed_ops;
        state.deletes_without_mutation = manager.status_.deletes_without_mutation;
    }
    manager.shell_cancel_requested_.store(false);
    manager.transfer_cancel_.store(false);
    manager.transfer_pause_.store(false);
    manager.transfer_active_.store(item.req.type == OpType::Copy || item.req.type == OpType::Move);
    manager.delete_cancel_.store(false);
    manager.SetStatus([&](OpStatus& status) {
        if (item.retry_batch_status) status = *item.retry_batch_status;
        status.active = true;
        status.task_id = item.seq;
        status.type = item.retry_batch_status ? item.retry_batch_status->type : item.req.type;
        status.phase = OpPhase::Running;
        status.summary = l10n::Get(l10n::StringId::PermissionOperationContinue);
        status.last_error.clear();
        status.failed_path.clear();
        status.failure_hr = S_OK;
        status.lock_retry_available = false;
        status.locked_path.clear(); status.lock_owners.clear();
    });
    OperationElevationResult result;
    bool helper_missing = false;
    try {
        WireRequest wire;
        wire.request = item.req;
        wire.task_id = item.seq;
        wire.parent_pid = GetCurrentProcessId();
        wire.owner = reinterpret_cast<uint64_t>(manager.UiWindow());
        wire.verify = manager.VerifyCopies();
        wire.language = l10n::LanguageId(l10n::effective_language());
        wire.rename_identity = item.retry_rename_identity;
        wire.rename_source_id = item.rename_source_id;
        wire.completed = item.retry_completed_sources;
        for (const auto& entry : item.retry_transfer_plan) {
            const bool done = std::any_of(
                item.retry_completed_sources.begin(), item.retry_completed_sources.end(),
                [&](const auto& root) {
                    return entry.source.size() >= root.size() && entry.source.starts_with(root) &&
                           (entry.source.size() == root.size() || entry.source[root.size()] == L'\\');
                });
            if (!done)
                wire.plan.push_back({entry, reinterpret_cast<uint64_t>(entry.source_identity.Get())});
        }
        for (const auto& ref : item.retry_source_references)
            wire.references.push_back({ref.identity, reinterpret_cast<uint64_t>(ref.Get())});
        if (item.retry_source_directories)
            wire.source_guard = item.retry_source_directories->ExportForElevation();
        if (item.retry_destination_guard)
            wire.destination_guard = item.retry_destination_guard->ExportForElevation();
        const auto payload = EncodeRequest(wire);
        (void)DecodeRequest(payload);
        if (IsDelete(item.req.type)) {
            {
                std::lock_guard lock(manager.mutex_);
                manager.active_item_ = item;
            }
            manager.PersistJournal();
        }
#ifdef PULSE_OPERATION_ELEVATION_TESTING
        if (!native_consent)
            result =
                RunOperationElevationForTest(helper, payload, {&state, &ParentProgress, &ParentCancelled});
        else
#else
        (void)native_consent;
#endif
        {
            const DWORD attributes = GetFileAttributesW(helper.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                const DWORD last_error = GetLastError();
                const DWORD error = last_error ? last_error : ERROR_GEN_FAILURE;
                helper_missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
                result.error = HRESULT_FROM_WIN32(error);
            } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
                helper_missing = true;
                result.error = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            } else {
                result = RunElevatedOperation(helper, manager.UiWindow(), payload,
                                              {&state, &ParentProgress, &ParentCancelled});
            }
        }
        if (result.response.empty() && state.serial && FAILED(result.error))
            result.outcome_unknown = true;
        if (!result.response.empty()) {
            ParentProgress(&state, result.response);
            if (state.malformed) {
                result.error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
                result.outcome_unknown = true;
            }
        }
    } catch (...) {
        result.error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    manager.transfer_active_.store(false);
    manager.delete_active_.store(false);
    manager.delete_service_.Cancel();
    {
        std::lock_guard lock(manager.transfer_control_mutex_);
        manager.pending_conflict_.reset();
    }
    manager.SetStatus([&](OpStatus& status) {
        status.active = false;
        status.completed_ops = state.completed_ops + 1;
        status.lock_retry_available = false;
        if (result.outcome_unknown || state.malformed) {
            status.phase = OpPhase::Failed;
            status.failure_hr = FAILED(result.error) ? result.error : E_FAIL;
            status.last_error = l10n::Get(l10n::StringId::PermissionOperationOutcomeUnknown);
            status.summary = status.last_error;
        } else if (result.cancelled || result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            status.phase = OpPhase::Cancelled;
            status.failure_hr = S_OK;
            status.last_error.clear();
            status.summary = l10n::Get(l10n::StringId::Cancel);
        } else if ((FAILED(result.error) || !state.serial) && (!state.serial || !IsTerminal(status.phase))) {
            status.phase = OpPhase::Failed;
            status.failure_hr = FAILED(result.error) ? result.error : E_FAIL;
            status.last_error = format::FileOperationErrorText(status.failure_hr);
            status.failed_path = helper_missing ? helper : item.retry_failed_path;
            status.summary = status.last_error;
        } else if (!IsTerminal(status.phase)) {
            status.phase = OpPhase::Failed;
            status.failure_hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            status.last_error = l10n::Get(l10n::StringId::OperationFailedMessage);
            status.summary = status.last_error;
        }
    });
    const bool uncertain = result.outcome_unknown || state.malformed.load();
    {
        std::lock_guard lock(manager.mutex_);
        const uint64_t recovery_sequence = item.recovery_sequence ? item.recovery_sequence : item.seq;
        std::erase_if(manager.pending_recovery_,
                      [&](const auto& entry) { return entry.sequence == recovery_sequence; });
        manager.scheduled_recovery_.erase(recovery_sequence);
        if (uncertain) {
            RecoveryEntry recovery{recovery_sequence, item.req, true};
            recovery.outcome_unknown = true;
            const auto completed = [&](const auto& path) {
                return std::find(state.proven_sources.begin(), state.proven_sources.end(), path) !=
                           state.proven_sources.end() ||
                       std::find(item.retry_completed_sources.begin(), item.retry_completed_sources.end(),
                                 path) != item.retry_completed_sources.end();
            };
            std::erase_if(recovery.request.sources, completed);
            for (auto& target : recovery.request.delete_targets)
                std::erase_if(target.physical_paths, completed);
            std::erase_if(recovery.request.delete_targets,
                          [](const auto& target) { return target.physical_paths.empty(); });
            // Missing terminal evidence may include mutations not represented in
            // the last acknowledged snapshot. Retain the request for inspection.
            if (recovery.request.sources.empty())
                recovery.request = item.req;
            manager.pending_recovery_.push_back(std::move(recovery));
        }
        if (manager.active_item_ && manager.active_item_->seq == item.seq)
            manager.active_item_.reset();
    }
    manager.PersistJournal();
    if (uncertain && IsDelete(item.req.type)) {
        std::lock_guard lock(manager.mutex_);
        manager.delete_outcomes_.push_back({item.seq, state.imported_completions != 0, true});
    }
    if (IsDelete(item.req.type)) {
        std::lock_guard lock(manager.mutex_);
        if (item.req.undo_revision && item.req.undo_revision == manager.undo_revision_ &&
            !manager.undo_.empty() && (state.imported_completions || uncertain)) {
            if (uncertain)
                manager.undo_.back().supported = false;
            else
                manager.undo_.pop_back();
            ++manager.undo_revision_;
        }
    }
    manager.FinishMutationUndo(item.req);
    manager.FinishRecycleUndo(item.req);
}

OperationElevationResult OperationElevationAdapter::ExecuteWorker(void* raw, std::span<const uint8_t> bytes,
                                                                  OperationElevationWorkerChannel& channel) {
    auto& context = *static_cast<WorkerState*>(raw);
    bool execution_started = false;
    try {
        const auto wire = DecodeRequest(bytes);
        if (wire.parent_pid != context.parent_pid)
            throw InvalidPayload{};
        DestinationGuard::Handle parent(OpenProcess(PROCESS_DUP_HANDLE, FALSE, wire.parent_pid));
        if (!parent)
            return {HRESULT_FROM_WIN32(GetLastError()), false, false, {}};
        OpsManager manager;
        OpsManager::QueueItem item;
        item.req = wire.request;
        // The parent owns the undo reservation and history. The helper keeps
        // deletion origin and historical identities, without a local reservation.
        item.req.undo_revision = 0;
        item.seq = wire.task_id;
        item.retry_completed_sources = wire.completed;
        item.retry_rename_identity = wire.rename_identity;
        item.rename_source_id = wire.rename_source_id;
        for (const auto& planned : wire.plan) {
            auto entry = planned.entry;
            entry.source_identity =
                DuplicateReference(parent.get(), planned.handle, entry.source_identity.identity);
            item.retry_transfer_plan.push_back(std::move(entry));
        }
        for (const auto& ref : wire.references)
            item.retry_source_references.push_back(
                ref.handle ? DuplicateReference(parent.get(), ref.handle, ref.identity)
                           : FileObjectReference{});
        const auto import_guard = [&](const auto& entries) {
            std::shared_ptr<DestinationGuard> guard;
            if (!entries.empty()) {
                guard = std::make_shared<DestinationGuard>(
                    GetFileInformationByHandleEx, item.retry_source_directories
                                                      ? item.retry_source_directories->SharedDirectoryLeases()
                                                      : nullptr);
                std::wstring error;
                if (!guard->ImportForElevation(entries, parent.get(), error))
                    throw InvalidPayload{};
            }
            return guard;
        };
        item.retry_source_directories = import_guard(wire.source_guard);
        item.retry_destination_guard = import_guard(wire.destination_guard);
        for (size_t i = 0; i < item.retry_source_references.size(); ++i) {
            if (!item.retry_source_references[i].Get()) {
                if (std::find(item.retry_completed_sources.begin(), item.retry_completed_sources.end(),
                              item.req.sources[i]) == item.retry_completed_sources.end())
                    throw InvalidPayload{};
                continue;
            }
            const auto& source_path =
                item.req.type == OpType::RestoreRecycle && i < item.req.recycle_paths.size()
                    ? item.req.recycle_paths[i]
                    : item.req.sources[i];
            const auto current = FileObjectReference::Open(source_path);
            if (!current.Get() || !item.retry_source_references[i].Matches(current.Get()))
                throw InvalidPayload{};
        }
        if (item.req.type == OpType::BatchRename && !item.req.is_undo) {
            item.req.undo_produced_identities.clear();
            for (const auto& reference : item.retry_source_references)
                item.req.undo_produced_identities.push_back(reference.identity);
        }
        if (item.req.type != OpType::Copy && item.req.type != OpType::Move) {
            std::wstring error;
            for (const auto& source : item.req.sources) {
                const auto parent_path = std::filesystem::path(source).parent_path().wstring();
                if (item.retry_source_directories &&
                    !item.retry_source_directories->PinDirectories(parent_path, error))
                    throw InvalidPayload{};
                if (item.req.type == OpType::RestoreRecycle) {
                    const size_t index = static_cast<size_t>(&source - item.req.sources.data());
                    if (index >= item.req.recycle_paths.size() || !item.retry_source_directories ||
                        !item.retry_source_directories->PinDirectories(
                            std::filesystem::path(item.req.recycle_paths[index]).parent_path().wstring(),
                            error))
                        throw InvalidPayload{};
                }
                if (item.retry_destination_guard &&
                    !item.retry_destination_guard->PinDirectories(parent_path, error))
                    throw InvalidPayload{};
            }
        }
        HWND owner = reinterpret_cast<HWND>(wire.owner);
        DWORD owner_pid = 0;
        if (owner) {
            GetWindowThreadProcessId(owner, &owner_pid);
            if (owner_pid != wire.parent_pid)
                throw InvalidPayload{};
        }
        l10n::Initialize(GetModuleHandleW(nullptr), wire.language);
        manager.SetUiWindow(owner);
        manager.SetVerifyCopies(wire.verify);
        manager.elevation_worker_ = true;
        std::mutex wake_mutex;
        std::condition_variable wake;
        bool changed = true, finished = false;
        manager.Start([&] {
            std::lock_guard lock(wake_mutex);
            changed = true;
            wake.notify_all();
        });
        struct StopManager {
            OpsManager& manager;
            ~StopManager() {
                manager.Stop();
            }
        } stop_manager{manager};
        {
            std::lock_guard lock(manager.mutex_);
            manager.active_item_ = item;
            manager.status_.task_id = item.seq;
            manager.status_.type = item.req.type;
            manager.status_.active = true;
            manager.status_.phase = OpPhase::Running;
        }
        std::thread execution([&] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try {
                if (IsDelete(item.req.type))
                    manager.RunDelete(item);
                else if (item.req.type == OpType::Copy || item.req.type == OpType::Move)
                    manager.RunTransfer(item.req, item.seq, item.retry_completed_sources,
                                        item.retry_transfer_plan, item.retry_source_directories,
                                        item.retry_destination_guard);
#ifdef PULSE_OPERATION_ELEVATION_TESTING
                else if (item.req.type == OpType::RestoreRecycle &&
                         item.req.sources.size() == item.req.recycle_paths.size() &&
                         !item.req.sources.empty() &&
                         std::all_of(item.req.sources.begin(), item.req.sources.end(), [&](const auto& original) {
                             const size_t index = static_cast<size_t>(&original - item.req.sources.data());
                             return IsIsolatedRecycleFixture(original, item.req.recycle_paths[index]);
                         }))
                    manager.RunManualRecycleRestore(item.req, &IsIsolatedRecycleFixture,
                                                    item.retry_source_directories.get());
#endif
                else {
                    std::vector<FileIdentity> identities;
                    for (const auto& reference : item.retry_source_references)
                        identities.push_back(reference.identity);
                    manager.RunShellOp(item.req, item.seq,
                                       item.retry_rename_identity ? &item.rename_source_id : nullptr, false,
                                       identities.empty() ? nullptr : &identities,
                                       item.req.type == OpType::Rename || item.req.type == OpType::BatchRename ||
                                               item.req.type == OpType::RestoreRecycle
                                           ? item.retry_source_directories.get()
                                           : nullptr);
                }
            } catch (...) {
                manager.SetStatus([&](OpStatus& status) {
                    status.active = false;
                    status.phase = OpPhase::Failed;
                    status.failure_hr = E_FAIL;
                    status.last_error = l10n::Get(l10n::StringId::OperationFailedMessage);
                });
            }
            if (SUCCEEDED(com))
                CoUninitialize();
            std::lock_guard lock(wake_mutex);
            finished = true;
            changed = true;
            wake.notify_all();
        });
        execution_started = true;
        struct Join {
            std::thread& thread;
            OpsManager& manager;
            ~Join() {
                manager.CancelCurrent();
                if (thread.joinable())
                    thread.join();
            }
        } join{execution, manager};
        uint64_t serial = 0;
        bool disconnected = false;
        auto capture = [&] {
            Snapshot snapshot;
            snapshot.serial = ++serial;
            {
                std::lock_guard lock(manager.mutex_);
                snapshot.status = manager.status_;
                for (const auto& completion : manager.completions_) {
                    CompletionRecord record;
                    record.completion = completion;
                    record.completion.task_id = item.seq;
                    const auto found =
                        std::find_if(manager.undo_.begin(), manager.undo_.end(), [&](const auto& undo) {
                            return undo.type == completion.type && undo.sources == completion.sources &&
                                   (undo.destinations == completion.destinations ||
                                    completion.type == OpType::RecycleDelete);
                        });
                    const bool needs_produced_identity =
                        completion.type == OpType::Copy || completion.type == OpType::Move ||
                        completion.type == OpType::Rename || completion.type == OpType::BatchRename ||
                        completion.type == OpType::CreateFolder || completion.type == OpType::CreateTextFile;
                    // PushUndo publishes completion and undo in separate lock
                    // sections. Keep the cumulative prefix pending until its
                    // matching history entry carries the engine's produced ID.
                    if (!item.req.is_undo && needs_produced_identity && found == manager.undo_.end())
                        break;
                    if (found != manager.undo_.end()) {
                        record.identities = found->produced_identities;
                        if (completion.type == OpType::RecycleDelete)
                            record.completion.destinations = found->destinations;
                    }
                    snapshot.completed.push_back(std::move(record));
                }
                snapshot.deletes.assign(manager.delete_outcomes_.begin(), manager.delete_outcomes_.end());
            }
            snapshot.conflict = manager.PendingConflict();
            snapshot.deletion = manager.PendingDeleteConfirmation();
            return snapshot;
        };
        while (true) {
            bool done = false;
            {
                std::unique_lock lock(wake_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return changed || finished; });
                done = finished;
                changed = false;
            }
            if (channel.IsCancelled())
                manager.CancelCurrent();
            auto snapshot = capture();
            if (done)
                break;
            std::vector<uint8_t> response;
            if (!channel.ReportProgress(EncodeSnapshot(snapshot), &response)) {
                disconnected = true;
                manager.CancelCurrent();
                break;
            }
            Control control;
            try {
                control = DecodeControl(response);
            } catch (...) {
                disconnected = true;
                manager.CancelCurrent();
                break;
            }
            if (control.cancel || channel.IsCancelled())
                manager.CancelCurrent();
            else if (control.paused)
                manager.PauseCurrent();
            else
                manager.ResumeCurrent();
            if (control.conflict_token) {
                if (!snapshot.conflict || control.conflict_token != snapshot.conflict->token) {
                    disconnected = true;
                    manager.CancelCurrent();
                    break;
                }
                if (!control.conflict_rows.empty()) {
                    if (!manager.ResolveConflicts(control.conflict_token, control.conflict_rows)) {
                        disconnected = true;
                        manager.CancelCurrent();
                        break;
                    }
                } else manager.ResolveConflict(control.conflict_token, control.choice, control.apply_all);
            }
            if (control.delete_token) {
                if (!snapshot.deletion || control.delete_token != snapshot.deletion->token) {
                    disconnected = true;
                    manager.CancelCurrent();
                    break;
                }
                manager.ResolveDeleteConfirmation(control.delete_token, control.delete_accepted);
            }
        }
        if (execution.joinable())
            execution.join();
        auto final_snapshot = capture();
        final_snapshot.conflict.reset();
        final_snapshot.deletion.reset();
        if (disconnected)
            return {HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE), false, true, {}};
        const bool cancelled = final_snapshot.status.phase == OpPhase::Cancelled;
        auto response = EncodeSnapshot(final_snapshot);
        manager.Stop();
        return {S_OK, cancelled, false, std::move(response)};
    } catch (...) {
        // A missing terminal snapshot after execution must stay uncertain in the
        // parent. Exiting the isolated helper retains the transport's unknown
        // outcome rather than certifying an empty failure after a mutation.
        if (execution_started)
            ExitProcess(ERROR_INVALID_DATA);
        return {HRESULT_FROM_WIN32(ERROR_INVALID_DATA), false, false, {}};
    }
}
void OperationElevationAdapter::RunParent(OpsManager& manager, const OpsManager::QueueItem& item) {
    RunParentImpl(manager, item, ShellHelperPath(ExecutablePath()), true);
}

int OperationElevationAdapter::RunWorker(int argc, wchar_t** argv) {
    if (argc != 5 || !argv || !argv[1] || !argv[2] || !argv[3] || !argv[4] ||
        std::wstring_view(argv[1]) != L"--file-operation-worker")
        return ERROR_INVALID_PARAMETER;
    wchar_t* end = nullptr;
    const auto pid = wcstoul(argv[3], &end, 10);
    if (!pid || !end || *end)
        return ERROR_INVALID_PARAMETER;
    WorkerState state{static_cast<DWORD>(pid)};
    return RunOperationElevationWorker(argc, argv, {&state, &ExecuteWorker});
}

#ifdef PULSE_OPERATION_ELEVATION_TESTING
void OperationElevationAdapter::RunFixtureForTest(OpsManager& manager, const OpRequest& req,
                                                  const std::wstring& helper, bool native_consent,
                                                  const std::function<void()>& before_launch) {
    OpsManager::QueueItem item;
    item.req = req;
    item.seq = 1;
    if (item.req.is_undo && !item.req.undo_revision) {
        std::lock_guard lock(manager.mutex_);
        item.req.undo_revision = manager.undo_revision_;
    }
    item.retry_source_directories = std::make_shared<DestinationGuard>();
    item.retry_destination_guard = std::make_shared<DestinationGuard>();
    std::wstring error;
    for (const auto& source : req.sources) {
        const auto parent = std::filesystem::path(source).parent_path().wstring();
        if (!item.retry_source_directories->Capture(parent, error))
            throw InvalidPayload{};
        if (req.type == OpType::RestoreRecycle) {
            const size_t index = static_cast<size_t>(&source - req.sources.data());
            if (index >= req.recycle_paths.size() || index >= req.recycle_undo_identities.size() ||
                !IsIsolatedRecycleFixture(source, req.recycle_paths[index]))
                throw InvalidPayload{};
            RecycleUndoIdentity identity;
            if (!ValidateRecycleUndoPair(source, req.recycle_paths[index], identity,
                                         &req.recycle_undo_identities[index], false) ||
                !item.retry_source_directories->Capture(
                    std::filesystem::path(req.recycle_paths[index]).parent_path().wstring(), error))
                throw InvalidPayload{};
            auto reference = FileObjectReference::Open(req.recycle_paths[index]);
            if (!reference.Get())
                throw InvalidPayload{};
            item.retry_source_references.push_back(std::move(reference));
        }
        if (req.type != OpType::CreateFolder && req.type != OpType::CreateTextFile &&
            req.type != OpType::RestoreRecycle) {
            auto reference = FileObjectReference::Open(source);
            if (!reference.Get())
                throw InvalidPayload{};
            item.retry_source_references.push_back(reference);
            if (req.type == OpType::Rename) {
                item.retry_rename_identity = true;
                item.rename_source_id = reference.identity;
            }
            if (req.type == OpType::Copy || req.type == OpType::Move) {
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &info))
                    throw InvalidPayload{};
                if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    TransferPlanEntry entry;
                    entry.source = source;
                    entry.destination =
                        (std::filesystem::path(req.dest_dir) / std::filesystem::path(source).filename())
                            .wstring();
                    entry.source_identity = reference;
                    entry.attributes = info.dwFileAttributes;
                    entry.reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                    entry.bytes = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                    entry.created = info.ftCreationTime;
                    entry.accessed = info.ftLastAccessTime;
                    entry.modified = info.ftLastWriteTime;
                    const DWORD attributes = GetFileAttributesW(entry.destination.c_str());
                    entry.destination_preexisting = attributes != INVALID_FILE_ATTRIBUTES;
                    item.retry_transfer_plan.push_back(std::move(entry));
                }
            }
        }
        if (!item.retry_destination_guard->Capture(parent, error))
            throw InvalidPayload{};
    }
    if (!req.dest_dir.empty() && !item.retry_destination_guard->Capture(req.dest_dir, error))
        throw InvalidPayload{};
    if (before_launch)
        before_launch();
    RunParentImpl(manager, item, helper, native_consent);
}

namespace {
HRESULT WINAPI DenySecondFixtureCopy(PCWSTR source, PCWSTR destination,
                                     COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    if (std::filesystem::path(source).filename() == L"partial-second.txt")
        return E_ACCESSDENIED;
    return CopyFile2(source, destination, parameters);
}
} // namespace

bool OperationElevationAdapter::PartialCopyContinuationForTest(OpsManager& manager, const OpRequest& request,
                                                               const std::wstring& helper) {
    if (request.type != OpType::Copy || request.sources.size() != 2 ||
        std::filesystem::path(request.sources.back()).filename() != L"partial-second.txt")
        return false;
    const auto original_copy = manager.transfer_copy_file_;
    struct RestoreCopyHook {
        OpsManager& manager;
        decltype(&CopyFile2) original;
        ~RestoreCopyHook() {
            manager.transfer_copy_file_ = original;
        }
    } restore{manager, original_copy};
    manager.transfer_copy_file_ = &DenySecondFixtureCopy;
    manager.RunTransfer(request, 1);
    manager.transfer_copy_file_ = original_copy;
    OpsManager::QueueItem retry;
    {
        std::lock_guard lock(manager.mutex_);
        if (!manager.elevation_retry_ || manager.status_.phase != OpPhase::Failed ||
            manager.status_.failure_hr != E_ACCESSDENIED)
            return false;
        retry = *manager.elevation_retry_;
        retry.retry_batch_status = manager.status_;
        retry.elevated_retry = true;
        if (retry.retry_completed_sources != std::vector<std::wstring>{request.sources.front()} ||
            retry.retry_transfer_plan.empty())
            return false;
        manager.elevation_retry_.reset();
    }
    const auto first_completions = manager.DrainCompletions();
    if (first_completions.size() != 1 ||
        first_completions[0].sources != std::vector<std::wstring>{request.sources.front()} ||
        first_completions[0].destinations.size() != 1)
        return false;
    const auto first_path = first_completions[0].destinations.front();
    FileIdentity before;
    if (!pulse::ReadPathFileIdentity(first_path, before))
        return false;
    RunParentImpl(manager, retry, helper, false);
    const auto status = manager.Status();
    const auto final_completions = manager.DrainCompletions();
    FileIdentity after;
    return status.phase == OpPhase::Completed && status.total_items == 2 && status.completed_items == 2 &&
           pulse::ReadPathFileIdentity(first_path, after) &&
           after == before && final_completions.size() == 1 &&
           final_completions[0].sources == std::vector<std::wstring>{request.sources.back()} &&
           final_completions[0].destinations.size() == 1 && final_completions[0].task_id == retry.seq;
}

bool OperationElevationAdapter::CopyUndoHistoryValidForTest(OpsManager& manager) {
    UndoEntry expected;
    {
        std::lock_guard lock(manager.mutex_);
        if (manager.undo_.empty())
            return false;
        expected = manager.undo_.back();
    }
    if (expected.type != OpType::Copy || !expected.supported || expected.destinations.empty() ||
        expected.destinations.size() != expected.produced_identities.size())
        return false;
    for (size_t i = 0; i < expected.destinations.size(); ++i) {
        FileIdentity current;
        if (!expected.produced_identities[i].Valid() ||
            !pulse::ReadPathFileIdentity(expected.destinations[i], current) ||
            current != expected.produced_identities[i])
            return false;
    }
    OpsManager restored;
    if (!restored.UndoFromJson(manager.UndoToJson()))
        return false;
    std::lock_guard lock(restored.mutex_);
    if (restored.undo_.empty())
        return false;
    const auto& actual = restored.undo_.back();
    return actual.supported && actual.sources == expected.sources &&
           actual.destinations == expected.destinations &&
           actual.produced_identities == expected.produced_identities;
}

bool OperationElevationAdapter::RunUndoFixtureForTest(OpsManager& manager, const std::wstring& helper) {
    OpRequest inverse;
    {
        std::lock_guard lock(manager.mutex_);
        if (manager.undo_.empty())
            return false;
        const auto& history = manager.undo_.back();
        if (!history.supported || history.sources.size() != history.destinations.size())
            return false;
        inverse.is_undo = true;
        inverse.undo_revision = manager.undo_revision_;
        inverse.sources = history.destinations;
        inverse.undo_produced_identities = history.produced_identities;
        if (history.type == OpType::Move) {
            inverse.type = OpType::Move;
            inverse.dest_dir = std::filesystem::path(history.sources.front()).parent_path().wstring();
            inverse.new_names = history.sources;
        } else if (history.type == OpType::Rename) {
            inverse.type = OpType::Rename;
            inverse.new_name = std::filesystem::path(history.sources.front()).filename().wstring();
        } else
            return false;
    }
    RunFixtureForTest(manager, inverse, helper);
    return manager.Status().phase == OpPhase::Completed;
}

bool OperationElevationAdapter::UnknownRecoveryPersistenceForTest(const std::wstring& journal_path) {
    OpsManager writer;
    writer.SetJournalPath(journal_path);
    const auto source = (std::filesystem::path(journal_path).parent_path() / L"unknown-source.txt").wstring();
    uint64_t sequence = 1;
    for (const auto type : {OpType::Rename, OpType::CreateFolder, OpType::CreateTextFile, OpType::RestoreRecycle}) {
        RecoveryEntry recovery;
        recovery.sequence = sequence++;
        recovery.was_active = true;
        recovery.outcome_unknown = true;
        recovery.request.type = type;
        recovery.request.sources = {source};
        writer.pending_recovery_.push_back(std::move(recovery));
    }
    OpsManager::QueueItem active;
    active.seq = sequence;
    active.req.type = OpType::Rename;
    active.req.sources = {source};
    active.req.new_name = L"unknown-target.txt";
    active.elevated_retry = true;
    writer.active_item_ = active;
    writer.PersistJournal();
    OpsManager restored;
    restored.SetJournalPath(journal_path);
    const auto before = restored.PendingRecovery();
    if (before.entries.size() != 5 ||
        !std::all_of(before.entries.begin(), before.entries.end(), [](const auto& entry) {
            return entry.outcome_unknown && entry.was_active;
        }) || restored.RetryRecovery())
        return false;
    {
        std::lock_guard lock(restored.mutex_);
        if (!restored.queue_.empty() || !restored.scheduled_recovery_.empty() ||
            restored.pending_recovery_.size() != before.entries.size())
            return false;
    }
    restored.PersistJournal();
    OpsManager reread;
    reread.SetJournalPath(journal_path);
    const auto after = reread.PendingRecovery();
    return after.entries.size() == before.entries.size() &&
           std::all_of(after.entries.begin(), after.entries.end(), [](const auto& entry) {
               return entry.outcome_unknown && entry.was_active;
           });
}

bool OperationElevationAdapter::HelperPathChecksForTest() {
    const std::wstring root = L"C:\\Program Files\\Pulse\\";
    return ShellHelperPath(root + L"Pulse.exe") == root + L"Pulse.Shell.exe" &&
        ShellHelperPath(L"\\\\?\\" + root + L"Pulse.exe") == L"\\\\?\\" + root + L"Pulse.Shell.exe" &&
        ShellHelperPath(L"\\\\server\\share\\Pulse.exe") == L"\\\\server\\share\\Pulse.Shell.exe" &&
        ShellHelperPath(L"").empty();
}

bool OperationElevationAdapter::CodecChecksForTest() {
    try {
        WireRequest request;
        request.request.sources = {L"E:\\fixture\\source.txt"};
        request.request.dest_dir = L"E:\\fixture\\destination";
        request.task_id = 7;
        request.parent_pid = 123;
        request.language = L"en-US";
        FileIdentity id;
        id.extended = true;
        id.value.VolumeSerialNumber = 1;
        id.value.FileId.Identifier[0] = 1;
        request.references.push_back({id, 100});
        DestinationGuard::ElevationSnapshot guard;
        guard.path = L"E:\\fixture";
        request.source_guard.push_back(guard);
        request.destination_guard.push_back(guard);
        const auto valid = EncodeRequest(request);
        const auto decoded = DecodeRequest(valid);
        if (decoded.task_id != request.task_id || decoded.references[0].identity != id)
            return false;
        const auto rejects = [](const auto& payload) {
            try {
                (void)DecodeRequest(payload);
                return false;
            } catch (...) {
                return true;
            }
        };
        for (size_t count = 0; count < valid.size(); ++count)
            if (!rejects(std::span<const uint8_t>(valid.data(), count)))
                return false;
        auto changed = valid;
        changed.push_back(0);
        if (!rejects(changed))
            return false;
        changed = valid;
        changed[0] ^= 1;
        if (!rejects(changed))
            return false;
        changed = valid;
        changed[4] = static_cast<uint8_t>(kVersion + 1);
        if (!rejects(changed))
            return false;
        changed = valid;
        changed[8] = 255;
        if (!rejects(changed))
            return false;
        request.request.sources = {L"relative\\source.txt"};
        if (!rejects(EncodeRequest(request)))
            return false;
        request.request.sources = {L"\\\\.\\PhysicalDrive0"};
        if (!rejects(EncodeRequest(request)))
            return false;
        request.request.sources = {L"E:\\fixture\\source.txt"};
        request.references[0].handle = 0;
        if (!rejects(EncodeRequest(request)))
            return false;
        request.references[0].handle = 100;
        request.references[0].identity = {};
        if (!rejects(EncodeRequest(request)))
            return false;
        Snapshot snapshot;
        snapshot.serial = 1;
        snapshot.status.task_id = 7;
        snapshot.status.percent = std::numeric_limits<float>::quiet_NaN();
        try {
            (void)DecodeSnapshot(EncodeSnapshot(snapshot));
            return false;
        } catch (...) {
        }
        Control control;
        control.choice = static_cast<ConflictChoice>(255);
        try {
            (void)DecodeControl(EncodeControl(control));
            return false;
        } catch (...) {
        }
        ConflictItemInfo conflict;
        conflict.token = 5;
        conflict.task_id = 7;
        conflict.source = L"E:\\fixture\\a";
        conflict.destination = L"E:\\fixture\\destination\\a";
        ConflictRowInfo row;
        row.row_id = 11;
        row.source = conflict.source;
        row.destination = conflict.destination;
        row.source_size = 123;
        row.destination_size = 456;
        row.source_modified = {1, 2};
        row.destination_modified = {3, 4};
        row.source_is_directory = true;
        row.destination_is_reparse = true;
        conflict.rows.push_back(row);
        row.row_id = 22;
        row.source = L"E:\\fixture\\b";
        row.destination = L"E:\\fixture\\destination\\b";
        conflict.rows.push_back(row);
        snapshot.status.percent = 0;
        snapshot.conflict = conflict;
        const auto round_trip = DecodeSnapshot(EncodeSnapshot(snapshot));
        if (!round_trip.conflict || round_trip.conflict->rows.size() != 2 ||
            round_trip.conflict->rows[0].row_id != 11 || round_trip.conflict->rows[1].source != row.source ||
            round_trip.conflict->rows[0].source_size != 123 ||
            round_trip.conflict->rows[0].destination_size != 456 ||
            round_trip.conflict->rows[0].source_modified.dwHighDateTime != 2 ||
            round_trip.conflict->rows[0].destination_modified.dwHighDateTime != 4 ||
            !round_trip.conflict->rows[0].source_is_directory ||
            !round_trip.conflict->rows[0].destination_is_reparse) return false;
        control = {};
        control.conflict_token = conflict.token;
        control.conflict_rows = {{11, ConflictChoice::Replace}, {22, ConflictChoice::KeepBoth}};
        const auto decisions = DecodeControl(EncodeControl(control));
        if (decisions.conflict_rows.size() != 2 || decisions.conflict_rows[0].row_id != 11 ||
            decisions.conflict_rows[1].choice != ConflictChoice::KeepBoth) return false;
        if (!ValidConflictResolutions(conflict.rows, decisions.conflict_rows)) return false;
        const auto rejects_control = [&](const Control& invalid) {
            try { (void)DecodeControl(EncodeControl(invalid)); return false; } catch (...) { return true; }
        };
        control.conflict_rows[1].row_id = 11;
        if (!rejects_control(control) || ValidConflictResolutions(conflict.rows, control.conflict_rows)) return false;
        control.conflict_rows[1].row_id = 33;
        if (ValidConflictResolutions(conflict.rows, control.conflict_rows)) return false;
        control.conflict_rows[1].row_id = 22;
        control.conflict_rows[1].choice = ConflictChoice::Continue;
        if (!rejects_control(control)) return false;
        control.conflict_rows[1].choice = ConflictChoice::Skip;
        control.conflict_token = 0;
        if (!rejects_control(control)) return false;
        snapshot.conflict->rows[1].row_id = 11;
        try { (void)DecodeSnapshot(EncodeSnapshot(snapshot)); return false; } catch (...) {}
        const auto rejects_snapshot = [&](const Snapshot& invalid) {
            try { (void)DecodeSnapshot(EncodeSnapshot(invalid)); return false; } catch (...) { return true; }
        };
        snapshot.conflict = conflict;
        snapshot.conflict->source.clear();
        if (!rejects_snapshot(snapshot)) return false; // Ordinary conflicts require a source.
        snapshot.conflict->link_confirmation = true;
        snapshot.conflict->rows.clear();
        snapshot.conflict->link_impact.path = conflict.destination;
        const auto link = DecodeSnapshot(EncodeSnapshot(snapshot));
        if (!link.conflict || !link.conflict->link_confirmation || !link.conflict->source.empty() ||
            link.conflict->link_impact.path != conflict.destination) return false;
        snapshot.conflict->source = L"relative\\source";
        if (!rejects_snapshot(snapshot)) return false;
        snapshot.conflict->source.clear();
        snapshot.conflict->destination = L"relative\\destination";
        if (!rejects_snapshot(snapshot)) return false;
        snapshot.conflict->destination = conflict.destination;
        snapshot.conflict->link_impact.path.clear();
        if (!rejects_snapshot(snapshot)) return false;
        return true;
    } catch (...) {
        return false;
    }
}
#endif
} // namespace pulse::ops
