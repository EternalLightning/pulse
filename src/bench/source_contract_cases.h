#pragma once
#include "../ops/ops_manager.h"
#include <winioctl.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace pulse::ops {

struct SourceContractTestProbe {
    static void EnumerationGate(OpsManager& manager, std::function<void(const std::wstring&)> gate) {
        manager.before_source_enumeration_ = std::move(gate);
    }
    static void CleanupGate(OpsManager& manager, std::function<void(const std::wstring&)> gate) {
        manager.before_source_cleanup_ = std::move(gate);
    }
    static void Execute(OpsManager& manager, OpRequest request, bool force_copy = true) {
        OpsManager::QueueItem item;
        {
            std::lock_guard lock(manager.mutex_);
            manager.running_ = true;
            manager.accepting_ = true;
            item.seq = manager.next_seq_++;
        }
        manager.force_copy_move_ = force_copy;
        item.req = std::move(request);
        manager.ExecuteFileOperation(item);
    }
    static bool ExecuteQueued(OpsManager& manager) {
        OpsManager::QueueItem item;
        {
            std::lock_guard lock(manager.mutex_);
            if (manager.queue_.empty()) return false;
            item = std::move(manager.queue_.front());
            manager.queue_.pop_front();
        }
        manager.ExecuteFileOperation(item);
        return true;
    }
};

namespace source_contract_cases {

inline bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
inline bool Mkdir(const std::wstring& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
}
inline bool Write(const std::wstring& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size();
    CloseHandle(file);
    return ok;
}
inline std::string Read(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char bytes[256]{};
    DWORD length = 0;
    const bool ok = ReadFile(file, bytes, sizeof(bytes), &length, nullptr) != FALSE;
    CloseHandle(file);
    return ok ? std::string(bytes, length) : std::string{};
}
inline FileIdentity Identity(const std::wstring& path) {
    FileIdentity result;
    ReadPathFileIdentity(path, result);
    return result;
}
inline bool Junction(const std::wstring& link, const std::wstring& target, DWORD* failure_code = nullptr) {
    if (failure_code) *failure_code = ERROR_SUCCESS;
    if (!CreateDirectoryW(link.c_str(), nullptr)) {
        if (failure_code) *failure_code = GetLastError();
        return false;
    }
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (failure_code) *failure_code = GetLastError();
        RemoveDirectoryW(link.c_str()); return false;
    }
    struct JunctionData {
        DWORD tag; WORD length; WORD reserved;
        WORD substitute_offset; WORD substitute_length; WORD print_offset; WORD print_length;
        wchar_t names[32768];
    } data{};
    const auto substitute = L"\\??\\" + target;
    const auto substitute_bytes = substitute.size() * sizeof(wchar_t);
    const auto print_bytes = target.size() * sizeof(wchar_t);
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(substitute_bytes);
    data.print_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print_bytes);
    data.length = static_cast<WORD>(8 + substitute_bytes + print_bytes + 2 * sizeof(wchar_t));
    std::memcpy(data.names, substitute.c_str(), substitute_bytes + sizeof(wchar_t));
    std::memcpy(reinterpret_cast<BYTE*>(data.names) + data.print_offset,
        target.c_str(), print_bytes + sizeof(wchar_t));
    DWORD written = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(8 + data.length), nullptr, 0, &written, nullptr) != FALSE;
    if (!ok && failure_code) *failure_code = GetLastError();
    CloseHandle(handle);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}
inline OpRequest Request(OpType type, const std::wstring& source, const std::wstring& target) {
    OpRequest request;
    request.type = type; request.sources = {source}; request.dest_dir = target;
    request.collision_policy = CollisionPolicy::Replace;
    return request;
}
inline bool SharingDenied(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
}

} // namespace source_contract_cases

inline int RunSourceContractRegressions(const std::wstring& fixture_root) {
    using namespace source_contract_cases;
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const wchar_t* label) {
        wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", label);
        ok ? ++passed : ++failed;
    };
    const std::string original = "original authorized source bytes";
    const std::string foreign = "outside bytes must stay untouched";
    if (!Mkdir(fixture_root)) return 2;

    // Each gate runs after metadata admission and before the first cursor read.
    for (const auto type : {OpType::Copy, OpType::Move}) {
        for (int mode = 0; mode < 4; ++mode) {
            const auto dir = fixture_root + L"\\ancestry-" + std::to_wstring(static_cast<int>(type)) +
                L"-" + std::to_wstring(mode);
            const auto owned = dir + L"\\owned", outside = dir + L"\\outside", target = dir + L"\\dst";
            const auto alias = dir + L"\\alias";
            auto source = owned + L"\\tree";
            const auto foreign_tree = outside + L"\\tree";
            check(Mkdir(source + L"\\nested") && Mkdir(foreign_tree + L"\\nested") && Mkdir(target) &&
                Write(source + L"\\authorized.txt", original) && Write(source + L"\\nested\\authorized.txt", original) &&
                Write(foreign_tree + L"\\foreign.txt", foreign) &&
                Write(foreign_tree + L"\\nested\\foreign.txt", foreign), L"create unique source ancestry fixture");
            if (mode == 2) {
                check(Junction(alias, owned), L"create source ancestor junction");
                source = alias + L"\\tree";
            }
            const auto foreign_file = foreign_tree + (mode == 3 ? L"\\nested\\foreign.txt" : L"\\foreign.txt");
            check(SetFileAttributesW(foreign_file.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN) != FALSE,
                L"mark isolated outside file attributes");
            const auto outside_id = Identity(foreign_file);
            const auto outside_attributes = GetFileAttributesW(foreign_file.c_str());
            const auto gate_path = mode == 3 ? source + L"\\nested" : source;
            const auto held = mode == 3 ? owned + L"\\tree\\nested-held" : owned + L"\\tree-held";
            bool attempted = false, switched = false;
            DWORD refusal = ERROR_SUCCESS;
            {
                OpsManager manager;
                SourceContractTestProbe::EnumerationGate(manager, [&](const std::wstring& path) {
                    if (path != gate_path || attempted) return;
                    attempted = true;
                    if (mode == 2) {
                        if (!RemoveDirectoryW(alias.c_str())) { refusal = GetLastError(); return; }
                        switched = Junction(alias, outside);
                    } else {
                        if (!MoveFileW(path.c_str(), held.c_str())) { refusal = GetLastError(); return; }
                        switched = mode == 0 ? Mkdir(path) && Write(path + L"\\foreign.txt", foreign)
                            : Junction(path, mode == 3 ? foreign_tree + L"\\nested" : foreign_tree);
                    }
                });
                SourceContractTestProbe::Execute(manager, Request(type, source, target));
                const auto status = manager.Status();
                wprintf(L"[diagnostic] ancestry type=%d mode=%d attempted=%d switched=%d refusal=%lu phase=%d hr=0x%08lx path=%s error=%s\n",
                    static_cast<int>(type), mode, attempted, switched, refusal, static_cast<int>(status.phase),
                    static_cast<unsigned long>(status.failure_hr), status.failed_path.c_str(), status.last_error.c_str());
                check(attempted && (switched || SharingDenied(refusal)), L"ancestry replacement is exercised or denied by active lease");
                check(Read(foreign_file) == foreign && Identity(foreign_file) == outside_id &&
                    GetFileAttributesW(foreign_file.c_str()) == outside_attributes,
                    L"source ancestry race preserves outside bytes identity and attributes");
                check(!Exists(target + L"\\tree\\foreign.txt") && !Exists(target + L"\\tree\\nested\\foreign.txt"),
                    L"Copy and Move never admit descendants through a replaced source ancestor");
                if (switched) {
                    check(status.phase == OpPhase::Failed && FAILED(status.failure_hr), L"changed source ancestry reports a failure");
                    if (mode == 0) check(Read(source + L"\\foreign.txt") == foreign,
                        L"ordinary same-name replacement retains its foreign child");
                } else {
                    check(status.phase == OpPhase::Completed && Read(target + L"\\tree\\authorized.txt") == original,
                        L"protected ancestry continues with the authorized source");
                }
            }
            // Unlink every possible reparse name before recursive fixture cleanup.
            if (mode == 2) RemoveDirectoryW(alias.c_str());
            if (switched && (mode == 1 || mode == 3)) RemoveDirectoryW(gate_path.c_str());
            SetFileAttributesW(foreign_file.c_str(), FILE_ATTRIBUTE_NORMAL);
        }
    }

    for (int mode = 0; mode < 3; ++mode) {
        const auto dir = fixture_root + L"\\content-" + std::to_wstring(mode), target = dir + L"\\dst";
        const auto source = dir + L"\\source.txt", destination = target + L"\\source.txt";
        check(Mkdir(target) && Write(source, original), L"create same-object content stability fixture");
        const auto source_id = Identity(source);
        const auto changed = mode == 0 ? original + " appended" : mode == 1 ? std::string("new")
            : std::string(original.size(), 'x');
        bool attempted = false, mutated = false, same_object = false;
        DWORD refusal = ERROR_SUCCESS;
        OpsManager manager;
        SourceContractTestProbe::CleanupGate(manager, [&](const std::wstring& path) {
            if (path != source) return;
            attempted = true;
            const HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (writer == INVALID_HANDLE_VALUE) { refusal = GetLastError(); return; }
            FileIdentity current;
            same_object = ReadFileIdentity(writer, current) && current == source_id;
            LARGE_INTEGER end{};
            const std::string append = " appended";
            const auto& written_bytes = mode == 0 ? append : changed;
            const bool positioned = mode != 0 || SetFilePointerEx(writer, end, nullptr, FILE_END);
            DWORD written = 0;
            mutated = same_object && positioned && WriteFile(writer, written_bytes.data(),
                static_cast<DWORD>(written_bytes.size()), &written, nullptr) && written == written_bytes.size() &&
                (mode != 1 || SetEndOfFile(writer));
            CloseHandle(writer);
        });
        SourceContractTestProbe::Execute(manager, Request(OpType::Move, source, target));
        const auto status = manager.Status();
        check(attempted && ((mutated && same_object) || (!mutated && SharingDenied(refusal))),
            L"append truncate and equal-length overwrite target the same object or are lease denied");
        check(Read(destination) == original, L"committed move copy retains the original content version");
        check(mutated ? status.phase == OpPhase::Failed && Read(source) == changed && Identity(source) == source_id
            : status.phase == OpPhase::Completed && !Exists(source),
            L"changed same-object content survives or an enforced write lease permits cleanup");
    }

    {
        const auto dir = fixture_root + L"\\preexisting-writer", target = dir + L"\\dst", source = dir + L"\\source.txt";
        check(Mkdir(target) && Write(source, original), L"create preexisting writer fixture");
        const auto source_id = Identity(source);
        HANDLE writer = CreateFileW(source.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        check(writer != INVALID_HANDLE_VALUE, L"retain an admitted writable handle before move starts");
        OpsManager manager;
        SourceContractTestProbe::Execute(manager, Request(OpType::Move, source, target));
        const auto status = manager.Status();
        check(status.phase == OpPhase::Failed && FAILED(status.failure_hr) && Read(source) == original &&
            Identity(source) == source_id, L"preexisting writable handle refuses destructive move cleanup");
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        const bool queued = manager.RetryLockedOperation(status.task_id, false);
        check(queued && SourceContractTestProbe::ExecuteQueued(manager) && manager.Status().phase == OpPhase::Completed &&
            !Exists(source) && Read(target + L"\\source.txt") == original,
            L"closing the writer permits authenticated locked-operation retry");
    }

    for (const auto type : {OpType::Copy, OpType::Move}) {
        const auto dir = fixture_root + L"\\retry-ancestry-" + std::to_wstring(static_cast<int>(type));
        const auto owned = dir + L"\\owned", alias = dir + L"\\alias", outside = dir + L"\\outside";
        const auto source = alias + L"\\tree", target = dir + L"\\dst", held = owned + L"\\tree";
        const auto foreign_file = outside + L"\\tree\\payload.txt";
        check(Mkdir(held) && Mkdir(target) && Mkdir(outside + L"\\tree") && Write(held + L"\\payload.txt", original) &&
            Write(foreign_file, foreign) && Junction(alias, owned), L"create failed-transfer ancestry retry fixture");
        const auto outside_id = Identity(foreign_file);
        const HANDLE writer = CreateFileW((source + L"\\payload.txt").c_str(), GENERIC_WRITE,
            FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        check(writer != INVALID_HANDLE_VALUE, L"retain writer that denies CopyFile2 read admission");
        {
            OpsManager manager;
            SourceContractTestProbe::Execute(manager, Request(type, source, target));
            const auto status = manager.Status();
            check(status.phase == OpPhase::Failed && status.lock_retry_available,
                L"source lock failure retains an authenticated retry plan");
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
            const bool unlinked = RemoveDirectoryW(alias.c_str()) != FALSE;
            const DWORD unlink_error = unlinked ? ERROR_SUCCESS : GetLastError();
            DWORD junction_error = ERROR_SUCCESS;
            const bool junction_created = unlinked && Junction(alias, outside, &junction_error);
            wprintf(L"[diagnostic] retry ancestry type=%d ancestor unlink=%d error=%lu junction=%d error=%lu\n",
                static_cast<int>(type), unlinked, unlink_error, junction_created, junction_error);
            const bool switched = unlinked && junction_created;
            check(switched, L"failed transfer releases runtime directory leases before source replacement");
            const bool queued = manager.RetryLockedOperation(status.task_id, false);
            check(queued && SourceContractTestProbe::ExecuteQueued(manager) && manager.Status().phase == OpPhase::Failed &&
                FAILED(manager.Status().failure_hr), L"locked retry refuses the replaced source ancestry for Copy and Move");
            check(Read(foreign_file) == foreign && Identity(foreign_file) == outside_id &&
                Read(held + L"\\payload.txt") == original && Read(target + L"\\tree\\payload.txt") != foreign,
                L"retry does not freshly authorize or copy the outside source tree");
        }
        RemoveDirectoryW(alias.c_str());
    }

    for (const bool via_junction : {false, true}) {
        const auto dir = fixture_root + L"\\active-lease-" + std::to_wstring(via_junction);
        const auto owned = dir + L"\\owned", alias = dir + L"\\alias", outside = dir + L"\\outside";
        const auto target = dir + L"\\dst", actual_tree = owned + L"\\tree";
        check(Mkdir(actual_tree) && Mkdir(outside + L"\\tree") && Mkdir(target) &&
            Write(actual_tree + L"\\payload.txt", original) && Write(outside + L"\\tree\\payload.txt", foreign),
            L"create active directory lease fixture");
        if (via_junction) check(Junction(alias, owned), L"create source junction ancestor for active lease probe");
        const auto source_tree = (via_junction ? alias : owned) + L"\\tree";
        const auto source = source_tree + L"\\payload.txt";
        const auto protected_directory = via_junction ? alias : source_tree;
        const auto foreign_file = outside + L"\\tree\\payload.txt";
        const auto outside_id = Identity(foreign_file);
        bool attempted = false, delete_access_denied = false, retarget_denied = false;
        DWORD delete_error = ERROR_SUCCESS, retarget_error = ERROR_SUCCESS;
        {
            OpsManager manager;
            SourceContractTestProbe::CleanupGate(manager, [&](const std::wstring& path) {
                if (path != source || attempted) return;
                attempted = true;
                // A retained descendant ID alone cannot deny opening DELETE on
                // its parent; this probes the actual directory sharing lease.
                const HANDLE mutator = CreateFileW(protected_directory.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (mutator == INVALID_HANDLE_VALUE) {
                    delete_error = GetLastError();
                    delete_access_denied = SharingDenied(delete_error);
                } else CloseHandle(mutator);
                if (via_junction) {
                    if (!RemoveDirectoryW(alias.c_str())) {
                        retarget_error = GetLastError(); retarget_denied = SharingDenied(retarget_error);
                    } else Junction(alias, outside, &retarget_error);
                } else {
                    if (!MoveFileW(source_tree.c_str(), (owned + L"\\tree-held").c_str())) {
                        retarget_error = GetLastError(); retarget_denied = SharingDenied(retarget_error);
                    }
                }
            });
            SourceContractTestProbe::Execute(manager, Request(OpType::Move, source_tree, target));
            wprintf(L"[diagnostic] active lease junction=%d DELETE error=%lu retarget error=%lu\n",
                via_junction, delete_error, retarget_error);
            check(attempted && delete_access_denied && retarget_denied,
                L"active source directory lease denies DELETE admission and ancestor retarget after copy commit");
            check(manager.Status().phase == OpPhase::Completed && Read(target + L"\\tree\\payload.txt") == original &&
                Read(foreign_file) == foreign && Identity(foreign_file) == outside_id,
                L"protected cleanup removes only authorized source and leaves outside contents intact");
        }
        if (via_junction) RemoveDirectoryW(alias.c_str());
    }

    {
        const auto dir = fixture_root + L"\\mapped-writer", target = dir + L"\\dst", source = dir + L"\\source.txt";
        check(Mkdir(target) && Write(source, original), L"create writable mapping fixture");
        const auto source_id = Identity(source);
        const HANDLE writer = CreateFileW(source.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        const HANDLE mapping = writer == INVALID_HANDLE_VALUE ? nullptr : CreateFileMappingW(writer, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, original.size()) : nullptr;
        if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
        check(mapping && view, L"writable mapping outlives its original file handle");
        const std::string changed(original.size(), 'm');
        bool attempted = false, mutated = false;
        {
            OpsManager manager;
            SourceContractTestProbe::CleanupGate(manager, [&](const std::wstring& path) {
                if (path != source || !view) return;
                attempted = true;
                std::memcpy(view, changed.data(), changed.size());
                mutated = FlushViewOfFile(view, changed.size()) != FALSE;
            });
            SourceContractTestProbe::Execute(manager, Request(OpType::Move, source, target));
            check(manager.Status().phase == OpPhase::Failed && Exists(source) && Identity(source) == source_id &&
                Read(source) == (mutated ? changed : original),
                L"writable mapping cannot let destructive cleanup discard a newer source version");
            check(!attempted || (mutated && Read(target + L"\\source.txt") == original),
                L"mapping mutation after commit preserves the earlier destination version");
        }
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
    }

    for (const bool force_copy : {false, true}) {
        const auto dir = fixture_root + L"\\positive-" + std::to_wstring(force_copy), target = dir + L"\\dst";
        check(Mkdir(target), L"create positive multi-root move fixture");
        OpRequest request; request.type = OpType::Move; request.dest_dir = target;
        for (const auto* name : {L"first", L"second"}) {
            const auto source = dir + L"\\" + name;
            check(Mkdir(source + L"\\nested\\empty") && Write(source + L"\\nested\\payload.txt", original),
                L"create positive nested source with empty directory");
            request.sources.push_back(source);
        }
        OpsManager manager;
        SourceContractTestProbe::Execute(manager, request, force_copy);
        check(manager.Status().phase == OpPhase::Completed && !Exists(request.sources[0]) && !Exists(request.sources[1]) &&
            Read(target + L"\\first\\nested\\payload.txt") == original &&
            Read(target + L"\\second\\nested\\payload.txt") == original && Exists(target + L"\\first\\nested\\empty"),
            L"same-volume rename and forced copy-delete both complete multi-directory moves");
        const auto file = dir + L"\\single.txt";
        check(Write(file, original), L"create ordinary file move control");
        SourceContractTestProbe::Execute(manager, Request(OpType::Move, file, target), force_copy);
        check(manager.Status().phase == OpPhase::Completed && !Exists(file) && Read(target + L"\\single.txt") == original,
            L"ordinary file rename and forced copy-delete remain functional");
    }

    {
        const auto dir = fixture_root + L"\\large-copy-metadata", source = dir + L"\\tree", target = dir + L"\\dst";
        const auto file = source + L"\\large.bin", product = target + L"\\tree\\large.bin";
        constexpr LONGLONG large_size = 64ll * 1024ll * 1024ll + 4096;
        check(Mkdir(source) && Mkdir(target), L"create unique large directory Copy fixture");
        const auto create_large = [&] {
            const HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (writer == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER offset{}; offset.QuadPart = large_size;
            bool ok = SetFilePointerEx(writer, offset, nullptr, FILE_BEGIN) && SetEndOfFile(writer);
            offset.QuadPart = 0;
            DWORD written = 0;
            ok = ok && SetFilePointerEx(writer, offset, nullptr, FILE_BEGIN) &&
                WriteFile(writer, original.data(), static_cast<DWORD>(original.size()), &written, nullptr) && written == original.size();
            offset.QuadPart = large_size - static_cast<LONGLONG>(foreign.size());
            ok = ok && SetFilePointerEx(writer, offset, nullptr, FILE_BEGIN) &&
                WriteFile(writer, foreign.data(), static_cast<DWORD>(foreign.size()), &written, nullptr) && written == foreign.size();
            CloseHandle(writer);
            return ok;
        };
        const auto matches_large = [&](const std::wstring& path) {
            const HANDLE reader = CreateFileW(path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (reader == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(reader, &size) && size.QuadPart == large_size;
            std::array<unsigned char, 64 * 1024> block{};
            LONGLONG position = 0;
            while (ok && position < large_size) {
                DWORD length = 0;
                ok = ReadFile(reader, block.data(), static_cast<DWORD>(block.size()), &length, nullptr) && length != 0;
                for (DWORD i = 0; ok && i < length; ++i) {
                    const auto index = position + i;
                    unsigned char expected = 0;
                    if (index < static_cast<LONGLONG>(original.size()))
                        expected = static_cast<unsigned char>(original[static_cast<size_t>(index)]);
                    else if (index >= large_size - static_cast<LONGLONG>(foreign.size()))
                        expected = static_cast<unsigned char>(foreign[static_cast<size_t>(index - large_size +
                            static_cast<LONGLONG>(foreign.size()))]);
                    ok = block[i] == expected;
                }
                position += length;
            }
            CloseHandle(reader);
            return ok && position == large_size;
        };
        check(create_large(), L"create >=64MiB source with head tail payload and zero-filled body");
        constexpr DWORD directory_metadata = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN;
        check(SetFileAttributesW(source.c_str(), directory_metadata) != FALSE,
            L"set source directory readonly and hidden metadata");
        {
            OpsManager manager;
            SourceContractTestProbe::Execute(manager, Request(OpType::Copy, source, target));
            if (!manager.Status().last_error.empty()) wprintf(L"[diagnostic] large directory Copy: %s\n", manager.Status().last_error.c_str());
            check(manager.Status().phase == OpPhase::Completed,
                L"large directory Copy completes temporary publication and metadata restoration");
            check(matches_large(product) && matches_large(file),
                L"large Copy preserves all payload and body bytes and retains its source");
            const DWORD attributes = GetFileAttributesW((target + L"\\tree").c_str());
            check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & directory_metadata) == directory_metadata,
                L"reacquired parent lease retains write-attributes access for directory metadata");
        }
        SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_NORMAL);
        SetFileAttributesW((target + L"\\tree").c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {
        const auto dir = fixture_root + L"\\cancel", source = dir + L"\\tree", target = dir + L"\\dst";
        check(Mkdir(source) && Mkdir(target) && Write(source + L"\\payload.txt", original), L"create cancellation fixture");
        OpsManager manager;
        bool cancelled = false;
        SourceContractTestProbe::EnumerationGate(manager, [&](const std::wstring&) {
            cancelled = true; manager.CancelCurrent();
        });
        SourceContractTestProbe::Execute(manager, Request(OpType::Move, source, target));
        check(cancelled && manager.Status().phase == OpPhase::Cancelled && Read(source + L"\\payload.txt") == original &&
            !Exists(target + L"\\tree"), L"cancellation before enumeration preserves sources and writes no destination");
        const auto held = dir + L"\\tree-held";
        check(MoveFileW(source.c_str(), held.c_str()) && MoveFileW(held.c_str(), source.c_str()),
            L"cancelled source admission releases directory protection");
        SourceContractTestProbe::EnumerationGate(manager, {});
        SourceContractTestProbe::Execute(manager, Request(OpType::Move, source, target));
        check(manager.Status().phase == OpPhase::Completed && !Exists(source) && Read(target + L"\\tree\\payload.txt") == original,
            L"fresh move after cancellation reacquires source protection and completes");
    }

    std::error_code error;
    std::filesystem::remove_all(fixture_root, error);
    check(!error && !Exists(fixture_root), L"unique source contract fixture is completely cleaned");
    wprintf(L"== source contract: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}

} // namespace pulse::ops
