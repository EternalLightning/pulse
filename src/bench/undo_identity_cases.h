// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ops/ops_manager.h"
#include "../ops/produced_undo.h"
#include "../common/file_create.h"
#include "../common/localization.h"
#include "../shell_host/recycle_fileop.h"
#include "../ipc/file_identity_protocol.h"
#include "../ipc/delete_plan_protocol.h"
#include "../fs/fs_recycle.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <memory>
#include <thread>

namespace pulse::ops {
struct UndoIdentityTestProbe {
    static void Record(OpsManager& manager, const OpRequest& request,
                       const std::vector<std::wstring>* destinations,
                       const std::vector<FileIdentity>* identities) {
        manager.PushUndo(request, destinations, identities);
    }
    static bool TakeInverse(OpsManager& manager, OpRequest& inverse) {
        {
            std::lock_guard lock(manager.mutex_);
            manager.accepting_ = true;
        }
        manager.Undo();
        std::lock_guard lock(manager.mutex_);
        manager.accepting_ = false;
        if (manager.queue_.empty()) return false;
        inverse = std::move(manager.queue_.front().req);
        manager.queue_.pop_front();
        return true;
    }
    static void DeleteInverse(OpsManager& manager, OpRequest inverse) {
        OpsManager::QueueItem item;
        item.seq = 1; item.req = std::move(inverse);
        manager.RunDelete(item);
    }
    static std::vector<FileIdentity> ProducedIdentities(OpsManager& manager) {
        std::lock_guard lock(manager.mutex_);
        return manager.undo_.empty() ? std::vector<FileIdentity>{} : manager.undo_.back().produced_identities;
    }
};

inline int RunUndoIdentityRegressions(const std::wstring& fixture_root) {
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const wchar_t* label) {
        wprintf(L"[%s] %s\n", ok ? L"PASS" : L"FAIL", label);
        ok ? ++passed : ++failed;
    };
    struct CloseFile { void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } };
    using Handle = std::unique_ptr<void, CloseFile>;
    DWORD integrity = MAXDWORD;
    HANDLE token_raw = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) {
        Handle token(token_raw);
        DWORD size = 0;
        GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &size);
        if (size && size <= 64 * 1024) {
            std::vector<BYTE> bytes(size);
            if (GetTokenInformation(token.get(), TokenIntegrityLevel, bytes.data(), size, &size)) {
                const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
                if (IsValidSid(sid) && *GetSidSubAuthorityCount(sid))
                    integrity = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
            }
        }
    }
    wprintf(L"[ENV] produced Undo fixture process integrity RID=%lu (Medium=8192, Low=4096)\n", integrity);
    const auto create_file = [](const std::wstring& path, FileIdentity& identity) {
        Handle handle(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!handle || handle.get() == INVALID_HANDLE_VALUE) { handle.reset(); return handle; }
        const char payload[] = "isolated produced undo identity";
        DWORD written = 0;
        if (!WriteFile(handle.get(), payload, sizeof(payload), &written, nullptr) || written != sizeof(payload) ||
            !ReadFileIdentity(handle.get(), identity) || !identity.Valid()) handle.reset();
        return handle;
    };
    const auto exists = [](const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
    const auto wait_done = [](OpsManager& manager, uint64_t previous) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = manager.Status();
            if (!status.active && status.completed_ops > previous) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    const auto print_undo_status = [](OpsManager& manager, const wchar_t* operation, bool finished) {
        const auto status = manager.Status();
        wprintf(L"  %s Undo: finished=%d phase=%d hr=0x%08lX failed_path=%s last_error=%s\n",
            operation, finished ? 1 : 0, static_cast<int>(status.phase),
            static_cast<unsigned long>(status.failure_hr), status.failed_path.c_str(), status.last_error.c_str());
    };
    std::error_code error;
    std::filesystem::create_directories(fixture_root, error);
    if (error) { check(false, L"isolated produced identity fixture created"); return 1; }

    for (const auto type : {OpType::Copy, OpType::CreateTextFile, OpType::CreateFolder}) {
        const auto path = fixture_root + L"\\produced-" + std::to_wstring(static_cast<int>(type));
        FileIdentity original_id;
        Handle original;
        if (type == OpType::CreateFolder) {
            original.reset(pulse::CreateDirectoryHandle(path));
            if (original) ReadFileIdentity(original.get(), original_id);
        } else original = create_file(path, original_id);
        check(original && original_id.Valid(), L"actual created handle provides historical product identity");
        if (!original || !original_id.Valid()) continue;
        OpRequest request;
        request.type = type;
        request.sources = {type == OpType::Copy ? fixture_root + L"\\source.txt" : path};
        request.dest_dir = fixture_root;
        const std::vector<std::wstring> destinations{path};
        const std::vector<FileIdentity> identities{original_id};
        OpsManager manager;
        UndoIdentityTestProbe::Record(manager, request, type == OpType::Copy ? &destinations : nullptr, &identities);
        check(manager.CanUndo(), L"known Copy/Create products remain undoable");
        const auto history = manager.UndoToJson();
        OpsManager restored;
        check(restored.UndoFromJson(history) && restored.CanUndo(), L"historical product identities survive persistence");
        OpRequest inverse;
        const bool inverse_ready = UndoIdentityTestProbe::TakeInverse(restored, inverse);
        check(inverse_ready && inverse.sources == destinations && inverse.undo_produced_identities == identities,
            L"inverse carries the original product identity and exact path");

        // Move rather than delete the original so its FileID cannot be reused for the replacement.
        original.reset();
        const auto displaced = path + L"-original";
        const bool moved = MoveFileExW(path.c_str(), displaced.c_str(), 0) != FALSE;
        FileIdentity replacement_id;
        auto replacement = create_file(path, replacement_id);
        check(moved && replacement && replacement_id != original_id, L"same-name replacement uses a different object");
        if (inverse_ready && moved && replacement) {
            replacement.reset();
            UndoIdentityTestProbe::DeleteInverse(restored, std::move(inverse));
            const auto status = restored.Status();
            check(status.phase == OpPhase::Failed && status.failure_hr == E_ACCESSDENIED &&
                status.failed_path == path && status.last_error == pulse::l10n::Format(
                    pulse::l10n::StringId::OperationUndoResultChangedFormat, path.c_str()) && exists(path),
                L"replaced Copy/Create product is preserved with an explicit identity failure");
            const auto backend = pulse::shell::RecycleAuthorized({path}, {}, {}, identities);
            check(backend.identity_mismatch && backend.error == E_ACCESSDENIED && backend.sources.empty() && exists(path),
                L"native recycle backend refuses a replaced historical product before provider invocation");
        }
        OpsManager unknown;
        UndoIdentityTestProbe::Record(unknown, request, type == OpType::Copy ? &destinations : nullptr, nullptr);
        auto legacy_history = unknown.UndoToJson();
        const auto field = legacy_history.find(L",\"produced\":[]");
        if (field != std::wstring::npos) legacy_history.erase(field, std::wstring(L",\"produced\":[]").size());
        const auto support = legacy_history.find(L"\"sup\":false");
        if (support != std::wstring::npos) legacy_history.replace(support, 11, L"\"sup\":true");
        OpsManager legacy;
        const bool loaded = legacy.UndoFromJson(legacy_history);
        OpRequest legacy_inverse;
        check(loaded && !legacy.CanUndo() && !UndoIdentityTestProbe::TakeInverse(legacy, legacy_inverse) && exists(path),
            L"legacy unknown Copy/Create record never samples a current same-name object to authorize deletion");
    }

    {
        const auto path = fixture_root + L"\\permanent-undo-block.txt";
        FileIdentity identity;
        auto file = create_file(path, identity);
        file.reset();
        OpRequest create;
        create.type = OpType::CreateTextFile; create.sources = {path};
        const std::vector<FileIdentity> identities{identity};
        OpsManager manager;
        UndoIdentityTestProbe::Record(manager, create, nullptr, &identities);
        OpRequest inverse;
        if (UndoIdentityTestProbe::TakeInverse(manager, inverse)) {
            // This plan boundary does not access the UNC fixture: network classification precedes backend IO.
            inverse.delete_targets = {{path, {L"\\\\pulse-undo-fixture\\share\\produced.txt"}, false}};
            UndoIdentityTestProbe::DeleteInverse(manager, std::move(inverse));
            check(manager.Status().phase == OpPhase::Failed &&
                manager.Status().last_error.find(L"不会将撤销改为永久删除") != std::wstring::npos &&
                !manager.PendingDeleteConfirmation() && exists(path),
                L"an Undo plan classified as network permanent deletion is rejected before confirmation or backend IO");
        } else check(false, L"permanent Undo boundary fixture has a valid historical inverse");
    }

    const auto read_contents = [](const std::wstring& name) -> std::optional<std::string> {
        Handle file(CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file || file.get() == INVALID_HANDLE_VALUE) return std::nullopt;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.get(), &length) || length.QuadPart < 0 || length.QuadPart > 4096) return std::nullopt;
        std::string text(static_cast<size_t>(length.QuadPart), '\0');
        DWORD read = 0;
        if (!text.empty() && (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) ||
            read != text.size())) return std::nullopt;
        return text;
    };
    struct RecycledFixture { std::wstring original; FileIdentity produced; bool directory; std::string contents; };
    std::vector<RecycledFixture> recycled_fixtures;
    const auto retain_fixture = [&](const std::wstring& name, const FileIdentity& identity) {
        const bool directory = (GetFileAttributesW(name.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
        auto contents = read_contents(directory ? name + L"\\child.txt" : name);
        check(contents.has_value(), L"record exact fixture contents before native recycling");
        recycled_fixtures.push_back({name, identity, directory, contents.value_or(std::string{})});
    };
    const auto track_recycled_fixture = [&](OpsManager& manager, const std::wstring& original) {
        const auto identities = UndoIdentityTestProbe::ProducedIdentities(manager);
        check(identities.size() == 1 && identities.front().Valid(),
            L"native Undo cleanup retains the actual historical product identity");
        if (identities.size() == 1 && identities.front().Valid())
            retain_fixture(original, identities.front());
    };

    // Keep the admitted host alive through restoration; this also exercises the
    // same operation manager lifetime as the application.
    OpsManager manager;
    manager.Start([] {});
    // Verify the normal application path, including host-created identities and native recycling.
    {
        const auto file = fixture_root + L"\\normal-created.txt";
        OpRequest create;
        create.type = OpType::CreateTextFile; create.sources = {file};
        auto previous = manager.Status().completed_ops;
        manager.Submit(std::move(create));
        const bool created = wait_done(manager, previous);
        check(created && manager.Status().last_error.empty() && exists(file) && manager.CanUndo(),
            L"real CREATE_NEW completion transports the created object's identity");
        if (created && manager.CanUndo()) {
            track_recycled_fixture(manager, file);
            previous = manager.Status().completed_ops;
            manager.Undo();
            const bool finished = wait_done(manager, previous);
            print_undo_status(manager, L"CreateTextFile", finished);
            check(finished && !exists(file) && manager.Status().last_error.empty(),
                L"unchanged created file still undoes through native RecycleItem");
        }
        const auto folder = fixture_root + L"\\normal-created-folder";
        OpRequest create_folder;
        create_folder.type = OpType::CreateFolder; create_folder.sources = {folder};
        previous = manager.Status().completed_ops;
        manager.Submit(std::move(create_folder));
        const bool folder_created = wait_done(manager, previous);
        check(folder_created && manager.Status().last_error.empty() && exists(folder) && manager.CanUndo(),
            L"atomic directory creation transports the created directory's identity");
        if (folder_created && manager.CanUndo()) {
            FileIdentity child_id;
            auto child = create_file(folder + L"\\child.txt", child_id);
            check(child && child_id.Valid(), L"created directory Undo fixture contains an actual child");
            child.reset();
            track_recycled_fixture(manager, folder);
            previous = manager.Status().completed_ops;
            manager.Undo();
            const bool finished = wait_done(manager, previous);
            print_undo_status(manager, L"CreateFolder", finished);
            check(finished && !exists(folder) && manager.Status().last_error.empty(),
                L"unchanged created directory still undoes through native RecycleItem");
        }
        FileIdentity source_id;
        const auto source = fixture_root + L"\\normal-copy-source.txt";
        auto source_handle = create_file(source, source_id);
        source_handle.reset();
        const auto destination = fixture_root + L"\\copy-destination";
        std::filesystem::create_directory(destination, error);
        OpRequest copy;
        copy.type = OpType::Copy; copy.sources = {source}; copy.dest_dir = destination;
        previous = manager.Status().completed_ops;
        manager.Submit(std::move(copy));
        const auto copied = destination + L"\\normal-copy-source.txt";
        const bool copied_ok = wait_done(manager, previous);
        check(copied_ok && manager.Status().last_error.empty() && exists(copied) && manager.CanUndo(),
            L"real copy completion records the committed destination identity");
        if (copied_ok && manager.CanUndo()) {
            track_recycled_fixture(manager, copied);
            previous = manager.Status().completed_ops;
            manager.Undo();
            const bool finished = wait_done(manager, previous);
            print_undo_status(manager, L"Copy", finished);
            check(finished && !exists(copied) && exists(source) && manager.Status().last_error.empty(),
                L"unchanged copied file still undoes through native RecycleItem");
        }
        const auto ordinary_file = fixture_root + L"\\ordinary-delete.txt";
        const auto ordinary_folder = fixture_root + L"\\ordinary-delete-folder";
        FileIdentity file_id, folder_id, child_id;
        auto ordinary = create_file(ordinary_file, file_id); ordinary.reset();
        Handle ordinary_directory(pulse::CreateDirectoryHandle(ordinary_folder));
        const bool have_folder = ordinary_directory && ReadFileIdentity(ordinary_directory.get(), folder_id);
        ordinary_directory.reset();
        auto ordinary_child = create_file(ordinary_folder + L"\\child.txt", child_id); ordinary_child.reset();
        check(file_id.Valid() && have_folder && folder_id.Valid() && child_id.Valid(),
            L"ordinary recycle fixtures have file and nonempty-directory identities");
        if (file_id.Valid() && have_folder && folder_id.Valid() && child_id.Valid()) {
            retain_fixture(ordinary_file, file_id); retain_fixture(ordinary_folder, folder_id);
            OpRequest remove; remove.type = OpType::RecycleDelete;
            remove.sources = {ordinary_file, ordinary_folder};
            previous = manager.Status().completed_ops;
            manager.Submit(std::move(remove));
            const bool removed = wait_done(manager, previous);
            print_undo_status(manager, L"OrdinaryRecycle", removed);
            check(removed && manager.Status().phase == OpPhase::Completed &&
                !exists(ordinary_file) && !exists(ordinary_folder),
                L"ordinary native recycle handles a file and nonempty directory");
            if (removed && manager.Status().phase == OpPhase::Completed) {
                previous = manager.Status().completed_ops;
                manager.Undo();
                const bool restored = wait_done(manager, previous);
                FileIdentity restored_file, restored_folder;
                check(restored && manager.Status().phase == OpPhase::Completed &&
                    ReadPathFileIdentity(ordinary_file, restored_file) && restored_file == file_id &&
                    ReadPathFileIdentity(ordinary_folder, restored_folder) && restored_folder == folder_id,
                    L"ordinary delete Undo restores the original file and directory objects through the production path");
                const std::string expected = std::string("isolated produced undo identity", sizeof("isolated produced undo identity"));
                check(read_contents(ordinary_file) == expected &&
                    read_contents(ordinary_folder + L"\\child.txt") == expected,
                    L"ordinary delete Undo preserves exact file and directory-child contents");
            }
        }
    }
    // Resolve only this run's exact originals and pin their validated $R/$I
    // objects before the production restore. Never walk or empty a Bin folder.
    std::vector<fs::DirEntry> recycle_entries;
    const auto recycle_root = std::filesystem::path(fixture_root).root_name().wstring() + L"\\$Recycle.Bin";
    const bool enumerated = fs::EnumerateRecycleBinAtRoot(recycle_root, recycle_entries);
    check(enumerated, L"enumerate current-user recycle evidence for this isolated run");
    for (const auto& fixture : recycled_fixtures) {
        std::wstring payload;
        size_t matches = 0;
        for (const auto& entry : recycle_entries) {
            if (entry.full_path != fs::NormalizePath(fixture.original)) continue;
            payload = entry.recycle_path;
            ++matches;
        }
        // A failed Undo may have left its original in place; its existing failure remains visible.
        if (!matches && exists(fixture.original)) continue;
        RecycleUndoIdentity pair;
        const bool verified = enumerated && matches == 1 &&
            IsCurrentUserRecyclePayload(fixture.original, payload) &&
            ValidateRecycleUndoPair(fixture.original, payload, pair);
        check(verified, L"cleanup verifies the fixture original path and its exact $R/$I pair");
        if (!verified) continue;
        const auto index = fs::RecycleIndexPath(payload);
        const auto pin = [](const std::wstring& path) {
            Handle handle(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (handle.get() == INVALID_HANDLE_VALUE) handle.reset();
            return handle;
        };
        auto payload_handle = pin(payload);
        auto index_handle = pin(index);
        FILE_ID_INFO payload_id{}, index_id{};
        FileIdentity produced_id;
        const auto same_id = [](const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
            return a.VolumeSerialNumber == b.VolumeSerialNumber &&
                std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
        };
        const bool pinned = payload_handle && index_handle &&
            GetFileInformationByHandleEx(payload_handle.get(), FileIdInfo, &payload_id, sizeof(payload_id)) &&
            GetFileInformationByHandleEx(index_handle.get(), FileIdInfo, &index_id, sizeof(index_id)) &&
            same_id(payload_id, pair.payload) && same_id(index_id, pair.index) &&
            ReadFileIdentity(payload_handle.get(), produced_id) && produced_id == fixture.produced;
        check(pinned, L"cleanup pins the validated pair and matches the original produced object identity");
        if (!pinned) continue;
        wprintf(L"  fixture recycle evidence: original=%s payload=%s index=%s\n",
            fixture.original.c_str(), payload.c_str(), index.c_str());
        payload_handle.reset(); index_handle.reset();
        OpRequest restore;
        restore.type = OpType::RestoreRecycle; restore.is_undo = true;
        restore.sources = {fixture.original}; restore.recycle_paths = {payload};
        restore.recycle_undo_identities = {pair};
        const auto previous = manager.Status().completed_ops;
        manager.Submit(std::move(restore));
        const bool finished = wait_done(manager, previous);
        const auto status = manager.Status();
        FileIdentity restored_id;
        const bool restored = finished && status.phase == OpPhase::Completed &&
            ReadPathFileIdentity(fixture.original, restored_id) && restored_id == fixture.produced;
        check(restored, L"native Undo product restores through the exact production path with its original object identity");
        if (!restored) wprintf(L"  exact restore: phase=%d hr=0x%08lX error=%s\n",
            static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr), status.last_error.c_str());
        check(restored && read_contents(fixture.directory ? fixture.original + L"\\child.txt" : fixture.original) == fixture.contents,
            L"recycle round trip preserves exact file or directory-child contents");
        index_handle = pin(index);
        FILE_ID_INFO current_index{};
        FILE_DISPOSITION_INFO dispose{TRUE};
        const bool index_deleted = restored && index_handle &&
            GetFileInformationByHandleEx(index_handle.get(), FileIdInfo, &current_index, sizeof(current_index)) &&
            same_id(current_index, pair.index) && SetFileInformationByHandle(index_handle.get(),
                FileDispositionInfo, &dispose, sizeof(dispose));
        index_handle.reset();
        check(restored && index_deleted && !exists(payload) && !exists(index),
            L"only this fixture's verified recycle metadata is retired after its round trip");
    }
    manager.Stop();
    std::filesystem::remove_all(fixture_root, error);
    check(!error && !exists(fixture_root), L"isolated undo identity fixture cleaned");
    wprintf(L"== produced undo identity: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
}
