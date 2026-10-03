#include "../common/user_storage.h"
#include "index_migration.h"
#include "index_protocol.h"
#include "index_transport_security.h"
#include "user_index_storage.h"
#include "../common/current_user_security.h"
#include <windows.h>

namespace pulse::storage {
bool ApplyUserIndexCheckpoint(std::wstring& error) {
    const auto target = Pending(Kind::Index);
    if (target.empty()) return true;
    index::IndexMigration migration;
    if (!index::CopyIndexForMigration(UserIndexRoot(), target, migration, error)) {
        RecordError(Kind::Index, error);
        return false;
    }
    if (!Commit(Kind::Index, migration.target, error)) {
        index::DiscardIndexMigrationCopies(migration);
        RecordError(Kind::Index, error);
        return false;
    }
    // Keep the original checkpoint as a recovery copy. It cannot be written by
    // this helper again once its active directory has been frozen at startup.
    RecordError(Kind::Index, {});
    return true;
}

bool ApplyUserIndex(std::wstring& error) {
    return ApplyUserIndexWithHost(index::kPipeName, index::kMutexName, error);
}

bool ApplyUserIndexWithHost(const std::wstring& pipe_name, const std::wstring& mutex_name,
                            std::wstring& error) {
    if (Pending(Kind::Index).empty()) return true;
    CurrentUserSecurityAttributes security;
    HANDLE mutex = CreateMutexW(security ? security.get() : nullptr, FALSE, mutex_name.c_str());
    if (!mutex) {
        error = L"无法锁定索引目录，原索引已保留。";
        RecordError(Kind::Index, error);
        return false;
    }
    DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait == WAIT_TIMEOUT) {
        HANDLE pipe = CreateFileW(pipe_name.c_str(), (FILE_GENERIC_WRITE & ~FILE_APPEND_DATA), 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            const auto request = index::MakeIndexHdr(index::REQ_IDX_STORAGE_SHUTDOWN, 0, 0);
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (operation.hEvent) {
                DWORD written = 0;
                if (!WriteFile(pipe, &request, sizeof(request), &written, &operation) &&
                    GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(operation.hEvent, 3000) != WAIT_OBJECT_0)
                        CancelIoEx(pipe, &operation);
                    GetOverlappedResult(pipe, &operation, &written, TRUE);
                }
                CloseHandle(operation.hEvent);
            }
            CloseHandle(pipe);
        }
        wait = WaitForSingleObject(mutex, 60000);
    }
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        error = L"索引进程尚未停止写入，迁移未执行，原索引已保留。请关闭其他 Pulse 窗口后重试。";
        RecordError(Kind::Index, error);
        return false;
    }
    const bool ok = ApplyUserIndexCheckpoint(error);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return ok;
}
}
