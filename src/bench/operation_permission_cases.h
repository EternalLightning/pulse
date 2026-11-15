// SPDX-License-Identifier: Apache-2.0
#pragma once

// Included after the integration runner's Session and fixture helpers.
class ReadOnlyAcl {
public:
    explicit ReadOnlyAcl(const fs::path& path) : path_(path) {
        DWORD size = 0;
        GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &size);
        original_.resize(size);
        if (!size || !GetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION, original_.data(), size, &size))
            throw std::runtime_error("cannot snapshot fixture ACL");
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("cannot read test identity");
        std::vector<BYTE> user;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        user.resize(size);
        const bool queried = GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE;
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!queried || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid))
            throw std::runtime_error("cannot encode test identity");
        const auto sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("cannot build fixture ACL");
        const bool applied = SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            descriptor) != FALSE;
        LocalFree(descriptor);
        if (!applied) throw std::runtime_error("cannot protect fixture ACL");
    }
    ~ReadOnlyAcl() {
        if (!SetFileSecurityW(path_.c_str(), DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            original_.data())) Check(false, "restore the isolated fixture ACL");
    }
    ReadOnlyAcl(const ReadOnlyAcl&) = delete;
    ReadOnlyAcl& operator=(const ReadOnlyAcl&) = delete;
private:
    fs::path path_;
    std::vector<BYTE> original_;
};

void PermissionOperations(const fs::path& root, HWND owner, bool exercise_uac) {
    Session session(root, owner);
    const auto source = root / L"source" / L"same.txt", destination = root / L"protected";
    Write(source, "permission source");
    const auto present = [&] {
        if (const auto conflict = session.manager.PendingConflict())
            session.manager.ResolveConflict(conflict->token, ConflictChoice::Replace, false);
        session.ConfirmFixtureDelete();
    };
    const auto locked_directory = root / L"locked";
    Write(locked_directory / L"same.txt", "locked original");
    HANDLE locked = CreateFileW((locked_directory / L"same.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (locked == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create isolated sharing violation");
    std::unique_ptr<void, decltype(&CloseHandle)> locked_lease(locked, &CloseHandle);
    const auto locked_status = session.Run(Request(OpType::Copy, {source}, locked_directory), present);
    Check(locked_status.phase == OpPhase::Failed && locked_status.lock_retry_available &&
        !locked_status.elevation_retry_available && Read(locked_directory / L"same.txt") == "locked original",
        "a real sharing violation offers lock retry instead of administrator permission");
    locked_lease.reset();
    fs::create_directories(destination);
    Write(destination / L"same.txt", "protected original");
    Write(destination / L"delete.txt", "delete original");
    ReadOnlyAcl folder_acl(destination);
    ReadOnlyAcl target_acl(destination / L"same.txt");
    ReadOnlyAcl delete_acl(destination / L"delete.txt");
    HANDLE denied = CreateFileW((destination / L"same.txt").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    const auto access_error = GetLastError();
    if (denied != INVALID_HANDLE_VALUE) CloseHandle(denied);
    Check(denied == INVALID_HANDLE_VALUE && access_error == ERROR_ACCESS_DENIED,
        "ACL fixture denies ordinary writes while granting Administrators full access");
    if (denied != INVALID_HANDLE_VALUE || access_error != ERROR_ACCESS_DENIED)
        throw std::runtime_error("permission fixture does not require administrator access under this token");
    for (const auto type : {OpType::Copy, OpType::Move, OpType::Rename, OpType::CreateFolder,
        OpType::CreateTextFile, OpType::RealDelete, OpType::RecycleDelete}) {
        auto request = Request(type, {source}, destination);
        if (type == OpType::Rename || type == OpType::RealDelete || type == OpType::RecycleDelete)
            request.sources = {(destination / L"delete.txt").wstring()};
        if (type == OpType::Rename) request.new_name = L"renamed.txt";
        if (type == OpType::CreateFolder || type == OpType::CreateTextFile)
            request.sources = {(destination / (type == OpType::CreateFolder ? L"new folder" : L"new.txt")).wstring()};
        const auto status = session.Run(request, present);
        Check(status.phase == OpPhase::Failed && status.elevation_retry_available &&
            !status.lock_retry_available && session.manager.IsElevationFailureCurrent(status.task_id) &&
            Read(source) == "permission source" && Read(destination / L"same.txt") == "protected original" &&
            Read(destination / L"delete.txt") == "delete original" && !fs::exists(destination / L"new folder") &&
            !fs::exists(destination / L"new.txt") && !fs::exists(destination / L"renamed.txt"),
            "protected operation fails without mutation and offers the Pulse permission decision");
        session.manager.CancelElevationRetry(status.task_id);
        Check(!session.manager.IsElevationFailureCurrent(status.task_id), "declined permission request is retired");
    }
    const auto batch_first = root / L"batch" / L"first.txt";
    Write(batch_first, "completed batch item");
    auto batch = Request(OpType::BatchRename, {batch_first, destination / L"delete.txt"});
    batch.new_names = {L"done.txt", L"blocked.txt"};
    const auto batch_status = session.Run(batch, present);
    Check(batch_status.phase == OpPhase::Failed && batch_status.completed_items == 1 &&
        batch_status.elevation_retry_available && batch_status.elevation_source_path == (destination / L"delete.txt").wstring() &&
        !fs::exists(batch_first) && Read(root / L"batch" / L"done.txt") == "completed batch item" &&
        Read(destination / L"delete.txt") == "delete original",
        "partial batch rename offers permission only for the unfinished source and retains its completed item");
    session.manager.CancelElevationRetry(batch_status.task_id);
    auto status = session.Run(Request(OpType::Copy, {source}, destination), present);
    const auto incoming = root / L"source" / L"changed-source.txt";
    fs::rename(source, incoming);
    Write(source, "replacement source must not run");
    auto before = status.completed_ops;
    Check(session.manager.RetryElevatedOperation(status.task_id), "current Pulse consent accepts one exact pending request");
    if (!Wait([&] {
        present();
        return session.manager.Status().completed_ops > before && !session.manager.HasPendingFileOperations();
    }, 60000)) throw std::runtime_error("permission retry did not terminate");
    status = session.manager.Status();
    Check(status.phase == OpPhase::Failed && status.failure_hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) &&
        !status.elevation_retry_available && Read(destination / L"same.txt") == "protected original",
        "source swapped during the Pulse permission prompt is rejected before UAC or mutation");
    if (exercise_uac) {
        status = session.Run(Request(OpType::Copy, {source}, destination), present);
        before = status.completed_ops;
        Check(session.manager.RetryElevatedOperation(status.task_id), "explicit permission consent queues the real UAC retry");
        if (!Wait([&] {
            present();
            return session.manager.Status().completed_ops > before && !session.manager.HasPendingFileOperations();
        }, 60000)) throw std::runtime_error("real UAC retry did not terminate");
        status = session.manager.Status();
        Check(status.phase == OpPhase::Completed && Read(destination / L"same.txt") == "replacement source must not run",
            "real Windows UAC grants the protected copy; final result and destination bytes agree");
    }
}
