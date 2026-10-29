#pragma once

#include "../shell_host/permanent_fileop.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace pulse::bench {

inline std::string ReadPermanentFixture(const std::filesystem::path& name) {
    const HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::cout << "[DIAG] shared fixture read open Win32=" << GetLastError() << '\n';
        return {};
    }
    std::string bytes;
    char buffer[4096];
    DWORD count = 0;
    for (;;) {
        if (!ReadFile(handle, buffer, sizeof(buffer), &count, nullptr)) {
            std::cout << "[DIAG] shared fixture read Win32=" << GetLastError() << '\n';
            bytes.clear();
            break;
        }
        if (!count) break;
        bytes.append(buffer, count);
    }
    CloseHandle(handle);
    return bytes;
}

inline bool PermanentFixtureReaderNeedsDeleteSharing(const std::filesystem::path& name) {
    const HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD error = GetLastError();
    std::cout << "[DIAG] frozen fixture read without FILE_SHARE_DELETE Win32=" << error << '\n';
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return handle == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION;
}

inline BOOL WINAPI RejectPermanentDisposition(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID value, DWORD bytes) {
    if (kind == FileDispositionInfo) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    const BOOL changed = SetFileInformationByHandle(handle, kind, value, bytes);
    const DWORD error = GetLastError();
    if (kind == FileBasicInfo) {
        FILE_BASIC_INFO observed{};
        const BOOL queried = GetFileInformationByHandleEx(handle, FileBasicInfo, &observed, sizeof(observed));
        std::cout << "[DIAG] fault fixture basic set requested=" << static_cast<FILE_BASIC_INFO*>(value)->FileAttributes
            << " success=" << changed << " Win32=" << (changed ? ERROR_SUCCESS : error)
            << " queried=" << queried << " observed=" << observed.FileAttributes << '\n';
    }
    SetLastError(error);
    return changed;
}

template <typename Check>
void RunPermanentReadonlyFailureFixture(const std::filesystem::path& root, Check&& check) {
    FileIdentity identity;
    const auto readonly_failure = root / L"read-only-failure.txt";
    std::ofstream(readonly_failure) << "preserve attributes";
    const bool assigned = SetFileAttributesW(readonly_failure.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE;
    const bool identified = ReadPathFileIdentity(readonly_failure.wstring(), identity);
    const DWORD original_attributes = GetFileAttributesW(readonly_failure.c_str());
    const bool prepared = assigned && identified && original_attributes != INVALID_FILE_ATTRIBUTES &&
        (original_attributes & FILE_ATTRIBUTE_READONLY);
    check(prepared, "prepare an observable read-only disposition failure");
    std::cout << "[DIAG] fault fixture path attributes before capture=" << original_attributes
        << " set_success=" << assigned << '\n';
    if (!prepared) {
        std::cout << "[BLOCKED] read-only rollback precondition unavailable; no disposition injection executed\n";
        SetFileAttributesW(readonly_failure.c_str(), FILE_ATTRIBUTE_NORMAL);
        return;
    }
    {
        shell::PermanentDeletionItem retained;
        DWORD error = ERROR_SUCCESS;
        std::wstring failed_path;
        const bool captured = retained.Capture(readonly_failure.wstring(), identity, error);
        const bool removed = captured && retained.Delete({}, failed_path, error, &RejectPermanentDisposition);
        const DWORD attributes = GetFileAttributesW(readonly_failure.c_str());
        std::cout << "[DIAG] disposition rejected result=" << removed << " Win32=" << error << " attributes=" << attributes << '\n';
        check(PermanentFixtureReaderNeedsDeleteSharing(readonly_failure),
            "failed deletion retains its authorized DELETE handle until release");
        FileIdentity preserved_identity;
        check(captured && !removed && error == ERROR_ACCESS_DENIED && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_READONLY) && ReadPathFileIdentity(readonly_failure.wstring(), preserved_identity) &&
            preserved_identity == identity && ReadPermanentFixture(readonly_failure) == "preserve attributes",
            "failed disposition preserves bytes and restores the original read-only attribute");
    }
    SetFileAttributesW(readonly_failure.c_str(), FILE_ATTRIBUTE_NORMAL);
}

template <typename Check>
void RunPermanentFileopFixture(const std::filesystem::path& root, Check&& check) {
    namespace fs = std::filesystem;
    fs::create_directories(root / L"tree" / L"nested");
    std::ofstream(root / L"tree" / L"nested" / L"read-only.txt") << "delete fixture";
    check(SetFileAttributesW((root / L"tree" / L"nested" / L"read-only.txt").c_str(), FILE_ATTRIBUTE_READONLY) != FALSE &&
        SetFileAttributesW((root / L"tree" / L"nested").c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
        "prepare read-only file and directory descendants");
    const auto source = root / L"source.txt", moved = root / L"moved.txt";
    std::ofstream(source) << "original";
    FileIdentity identity;
    check(ReadPathFileIdentity(source.wstring(), identity), "capture original permanent-delete identity");
    fs::rename(source, moved); std::ofstream(source) << "replacement";
    auto result = shell::DeletePermanentAuthorized({source.wstring()}, {identity}, {});
    check(result.error == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) && result.deleted.empty() &&
        ReadPermanentFixture(source) == "replacement" && ReadPermanentFixture(moved) == "original",
        "same-name replacement after authorization fails closed and preserves both objects");
    const auto preceding = root / L"preflight.txt";
    std::ofstream(preceding) << "preflight preserved";
    FileIdentity preceding_identity;
    check(ReadPathFileIdentity(preceding.wstring(), preceding_identity), "capture the preceding root identity");
    result = shell::DeletePermanentAuthorized({preceding.wstring(), source.wstring()}, {preceding_identity, identity}, {});
    check(result.error == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) && result.deleted.empty() &&
        ReadPermanentFixture(preceding) == "preflight preserved" && ReadPermanentFixture(source) == "replacement",
        "a later root identity mismatch rejects the whole batch before any root mutation");
    check(ReadPathFileIdentity(source.wstring(), identity), "capture the replacement for its own authorized delete");
    {
        shell::PermanentDeletionItem frozen;
        DWORD error = ERROR_SUCCESS;
        check(frozen.Capture(source.wstring(), identity, error), "freeze the authorized deletion handle");
        check(PermanentFixtureReaderNeedsDeleteSharing(source),
            "independent readers require delete sharing while the authorized DELETE handle is retained");
        const bool renamed = MoveFileW(source.c_str(), (root / L"cannot-replace.txt").c_str()) != FALSE;
        const DWORD rename_error = GetLastError();
        check(!renamed && rename_error == ERROR_SHARING_VIOLATION,
            "frozen object prevents rename before its mutation");
        const auto control_source = root / L"replace-control-source.txt", control_target = root / L"replace-control-target.txt";
        std::ofstream(control_source) << "control replacement";
        std::ofstream(control_target) << "control occupant";
        FileIdentity control_source_identity, control_target_identity;
        check(ReadPathFileIdentity(control_source.wstring(), control_source_identity) &&
            ReadPathFileIdentity(control_target.wstring(), control_target_identity) &&
            control_source_identity != control_target_identity, "capture distinct identities for the unfrozen replacement control");
        const bool control_replaced = MoveFileExW(control_source.c_str(), control_target.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        const DWORD control_error = control_replaced ? ERROR_SUCCESS : GetLastError();
        std::cout << "[DIAG] unfrozen same-directory replacement result=" << control_replaced << " Win32=" << control_error << '\n';
        FileIdentity control_result_identity;
        check(control_replaced && !fs::exists(control_source) && ReadPathFileIdentity(control_target.wstring(), control_result_identity) &&
            control_result_identity == control_source_identity && ReadPermanentFixture(control_target) == "control replacement",
            "same-directory replacement succeeds for an unfrozen control object");
        const auto contender = root / L"replacement-attempt.txt";
        std::ofstream(contender) << "contender";
        FileIdentity contender_identity;
        check(ReadPathFileIdentity(contender.wstring(), contender_identity), "capture the replacement contender identity");
        const bool replaced = MoveFileExW(contender.c_str(), source.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        const DWORD replacement_error = GetLastError();
        std::cout << "[DIAG] frozen same-name replacement result=" << replaced << " Win32=" << replacement_error << '\n';
        FileIdentity preserved_source, preserved_contender;
        check(!replaced && (replacement_error == ERROR_SHARING_VIOLATION || replacement_error == ERROR_ACCESS_DENIED) &&
            ReadPathFileIdentity(source.wstring(), preserved_source) && preserved_source == identity &&
            ReadPathFileIdentity(contender.wstring(), preserved_contender) && preserved_contender == contender_identity &&
            ReadPermanentFixture(source) == "replacement" && ReadPermanentFixture(contender) == "contender",
            "frozen object rejects a same-name replacement and preserves both objects");
        std::wstring failed_path;
        check(frozen.Delete({}, failed_path, error) && !fs::exists(source),
            "permanent deletion acts through the captured handle");
    }

    const auto tree = root / L"tree";
    check(ReadPathFileIdentity(tree.wstring(), identity), "capture the nonempty directory identity");
    result = shell::DeletePermanentAuthorized({tree.wstring()}, {identity}, {});
    check(SUCCEEDED(result.error) && result.deleted == std::vector<std::wstring>{tree.wstring()} && !fs::exists(tree),
        "native permanent delete removes a nonempty directory and read-only descendants");
    RunPermanentReadonlyFailureFixture(root, check);

    const auto cancelled = root / L"cancelled.txt", cancelled_later = root / L"cancelled-later.txt";
    std::ofstream(cancelled) << "cancelled permanent fixture";
    std::ofstream(cancelled_later) << "cancelled later fixture";
    FileIdentity later_identity;
    check(ReadPathFileIdentity(cancelled.wstring(), identity) && ReadPathFileIdentity(cancelled_later.wstring(), later_identity),
        "capture cancellation fixture identities");
    result = shell::DeletePermanentAuthorized({cancelled.wstring()}, {identity}, [] { return true; });
    check(result.cancelled && result.deleted.empty() && ReadPermanentFixture(cancelled) == "cancelled permanent fixture",
        "cancellation before capture leaves the authorized root intact");
    size_t cancel_checks = 0;
    result = shell::DeletePermanentAuthorized({cancelled.wstring(), cancelled_later.wstring()}, {identity, later_identity},
        [&] { return ++cancel_checks > 2; });
    check(result.cancelled && result.deleted.empty() && ReadPermanentFixture(cancelled) == "cancelled permanent fixture" &&
        ReadPermanentFixture(cancelled_later) == "cancelled later fixture",
        "cancellation after all roots are captured leaves both roots intact");
    bool cancel_after_first = false;
    result = shell::DeletePermanentAuthorized({cancelled.wstring(), cancelled_later.wstring()}, {identity, later_identity},
        [&] { return cancel_after_first; }, [&](size_t completed, size_t total, const std::wstring& name) {
            check(completed == 1 && total == 2 && name == cancelled.wstring(), "progress names the completed captured root");
            cancel_after_first = true;
        });
    check(result.cancelled && result.deleted == std::vector<std::wstring>{cancelled.wstring()} && !fs::exists(cancelled) &&
        ReadPermanentFixture(cancelled_later) == "cancelled later fixture",
        "cancellation after one root reports only the completed root and preserves the next root");

    FileIdentity drive_identity;
    ReadPathFileIdentity(root.root_path().wstring(), drive_identity);
    result = shell::DeletePermanentAuthorized({root.root_path().wstring()}, {drive_identity}, {});
    check(FAILED(result.error) && result.deleted.empty(), "native deletion rejects a drive root before enumeration");

    const auto outside = root / L"outside", link = root / L"directory-link";
    fs::create_directories(outside); std::ofstream(outside / L"keep.txt") << "outside bytes";
    if (CreateSymbolicLinkW(link.c_str(), outside.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        check(ReadPathFileIdentity(link.wstring(), identity), "capture the directory reparse leaf identity");
        result = shell::DeletePermanentAuthorized({link.wstring()}, {identity}, {});
        check(SUCCEEDED(result.error) && !fs::exists(link) && ReadPermanentFixture(outside / L"keep.txt") == "outside bytes",
            "selected directory reparse point is removed without traversing its target");
        if (CreateSymbolicLinkW(link.c_str(), outside.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
            const auto through_link = link / L"keep.txt";
            check(ReadPathFileIdentity(through_link.wstring(), identity), "capture the descendant identity through a reparse ancestor");
            result = shell::DeletePermanentAuthorized({through_link.wstring()}, {identity}, {});
            check(FAILED(result.error) && ReadPermanentFixture(outside / L"keep.txt") == "outside bytes",
                "reparse ancestor is rejected before opening or deleting its descendant");
            fs::remove(link);
        } else check(false, "recreate the directory reparse point for the ancestor rejection fixture");
    } else std::cout << "[SKIP] directory symlink fixture unavailable, Win32=" << GetLastError() << '\n';
}

} // namespace pulse::bench
