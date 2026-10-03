#include "../index/index_caller_visibility.h"
#include "../index/index_raw_storage_security.h"
#include "../index/index_request_limits.h"
#include "../index/network_agent_protocol.h"
#include "../index/change_tracking.h"
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!value) ++failures;
}
bool SetAcl(const std::wstring& path, const std::wstring& sddl) {
    PSECURITY_DESCRIPTOR security = nullptr; PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr)) return false;
    const DWORD error = GetSecurityDescriptorDacl(security, &present, &acl, &defaulted) && present
        ? SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    const bool ok = error == ERROR_SUCCESS;
    if (!ok) std::cout << "[INFO] fixture ACL update failed Win32=" << error << '\n';
    LocalFree(security); return ok;
}
void Touch(const std::wstring& path) {
    pulse::index::transport::Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(file.get() != INVALID_HANDLE_VALUE, "create isolated metadata fixture");
}
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::index;
    std::cout << std::unitbuf;
    if (argc > 1 && (std::wstring_view(argv[1]) == L"--metadata-budget-only" || std::wstring_view(argv[1]) == L"--visibility-bench")) {
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"index_security_perf_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(root / L"level1/level2");
        std::vector<std::wstring> paths; paths.reserve(10000);
        for (unsigned i = 0; i < 10000; ++i) {
            auto path = (root / L"level1/level2" / (L"item-" + std::to_wstring(i))).wstring();
            transport::Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.get() == INVALID_HANDLE_VALUE) { Check(false, "create performance fixture"); break; }
            paths.push_back(std::move(path));
        }
        HANDLE primary = nullptr, token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &primary))
            DuplicateTokenEx(primary, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token);
        transport::Handle process_token(primary), caller(token);
        const auto started = GetTickCount64(); size_t visible = 0;
        CallerVisibility policy(caller.get(), 2000, true);
        for (const auto& path : paths) if (policy.Visible(path)) ++visible;
        if (!policy.ValidateDirectories()) visible = 0;
        std::cout << "[METRIC] 10000 candidates; visible=" << visible << "; elapsed_ms=" << GetTickCount64() - started
                  << "; incomplete=" << policy.Incomplete() << '\n';
        std::error_code error; std::filesystem::remove_all(root, error);
        Check(!error, "isolated metadata measurement cleanup");
        return failures ? 1 : 0;
    }
    const auto base = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"index_security_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(base / L"parent");
    const auto identity = transport::Identity::Current();
    Check(identity.valid && !agent::PipeName().empty() && agent::PipeName().find(identity.user) != std::wstring::npos,
        "network endpoint binds current user and logon authentication");
    auto changed = identity; ++changed.session;
    Check(!identity.Matches(changed), "different Windows session is rejected");
    changed = identity; changed.grants += L"restricted";
    Check(!identity.Matches(changed), "restricted or altered real token is rejected");
    transport::LogonSecurity endpoint_acl(identity);
    Check(endpoint_acl.get() != nullptr, "logon-only endpoint ACL fails closed");
    Check(!transport::VerifyServer(INVALID_HANDLE_VALUE, transport::SiblingIndexImage(), identity),
        "invalid server process identity is rejected");
    HANDLE primary = nullptr, duplicate = nullptr;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &primary) != FALSE &&
        DuplicateTokenEx(primary, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &duplicate),
        "capture actual caller token for metadata checks");
    transport::Handle primary_owner(primary), caller(duplicate);
    const auto parent = (base / L"parent").wstring();
    const auto file = (base / L"parent/item.txt").wstring(); Touch(file);
    const auto allow = L"D:P(A;;FA;;;" + identity.user + L")";
    Check(SetAcl(parent, allow) && SetAcl(file, allow), "install isolated owner ACLs");
    CallerVisibility visible(caller.get());
    CallerVisibility snapshot(caller.get(), 2000, true);
    Check(snapshot.Visible(file) && snapshot.ValidateDirectories(), "directory snapshot initially validates");
    Check(visible.Visible(file), "readable metadata and all listable parents are visible");
    Check(SetAcl(parent, L"D:P(D;;0x1;;;" + identity.user + L")(A;;FA;;;" + identity.user + L")"),
        "deny parent list while leaving child readable");
    CallerVisibility denied_parent(caller.get());
    Check(!denied_parent.Visible(file), "readable file does not bypass inaccessible parent metadata");
    auto adversarial = parent + L"/item.txt";
    Check(!denied_parent.Visible(adversarial), "mixed slash caller path cannot skip ancestor authorization");
    Check(!visible.Visible(file), "cached allow cannot survive a same-response security change");
    Check(!snapshot.ValidateDirectories() && snapshot.Incomplete(), "security-changed directory snapshot is rejected before response serialization");
    Check(SetAcl(parent, allow), "restore parent ACL");
    Check(SetAcl(file, L"D:P(D;;0x80;;;" + identity.user + L")(A;;FA;;;" + identity.user + L")"),
        "deny child attributes separately from contents");
    CallerVisibility denied_child(caller.get());
    Check(!denied_child.Visible(file), "metadata denial is not equivalent to content read permission");
    SetAcl(file, allow);
    CallerVisibility restored(caller.get());
    Check(restored.Visible(file), "fresh response after ACL restoration remains usable");
    Check(!restored.Visible(L"\\\\server\\share\\item") && !restored.Visible(L"C:\\foo\\..\\bar") &&
          !restored.Visible(L"\\\\.\\PhysicalDrive0"), "remote traversal and device metadata fail closed");
    CallerVisibility expired(caller.get(), 0);
    Check(!expired.Visible(file) && expired.Incomplete(), "authorization budget expiry is explicit and fails closed");
    ChangeRecord deleted; deleted.path = parent + L"\\missing.txt"; deleted.kind = ChangeKind::Deleted;
    Check(!restored.RecordVisible(deleted), "deleted historical ACL cannot be guessed from parent access");
    deleted.path = file;
    Check(!restored.RecordVisible(deleted), "recreated readable path cannot authorize a formerly deleted identity");
    Check(!IndexRequestLimits::Admit(32, false, 0, 0, 0, 0, 1), "unique session flood is bounded");
    Check(!IndexRequestLimits::Admit(0, true, 32, 0, 32, 0, 1), "client queue item flood is bounded");
    Check(!IndexRequestLimits::Admit(0, false, 0, 512 * 1024, 0, 0, 1), "client queue bytes are bounded");
    Check(!IndexRequestLimits::Admit(0, false, 0, 0, 256, 0, 1), "global queue item flood is bounded");
    struct Task { int client; int id; };
    std::deque<Task> queue{{1,1},{1,2},{1,3},{2,1},{3,1}};
    const auto first = PopFair(queue), second = PopFair(queue), third = PopFair(queue);
    Check(first.client == 1 && second.client == 2 && third.client == 3, "queued clients receive round-robin service");
    {
        ChangeTracker tracker; tracker.Open(base.wstring()); tracker.Lease(L"test-owner", true);
        const auto now = ChangeTracker::Now();
        for (unsigned i = 0; i < 5; ++i) {
            ChangeRecord record; record.time = now; record.path = parent + L"\\event-" + std::to_wstring(i);
            record.kind = ChangeKind::Created; tracker.Record(std::move(record));
        }
        const auto authorize = [](const ChangeRecord& record) { return record.path.ends_with(L"4"); };
        const auto detail = tracker.Details(L"test-owner", parent, 0, 0, 1, UINT32_MAX, authorize);
        Check(detail.records.size() == 1 && detail.records[0].path.ends_with(L"4"), "change-detail authorization precedes page limit");
        const auto summary = tracker.Summaries(L"test-owner", {parent}, 0, authorize);
        Check(summary.summaries.size() == 1 && summary.summaries[0].count == 1 && summary.summaries[0].incomplete,
            "summary aggregates only authorized history and explains incompleteness");
    }
    // No real machine tree is touched. ACL repair is inspectable even when the
    // process cannot become SYSTEM/Admin in this non-elevated test environment.
    const auto raw = (base / L"raw").wstring(); std::filesystem::create_directory(raw);
    Touch(raw + L"\\snapshot.tmp");
    const bool repaired = ProtectIndexRawTree(raw);
    if (repaired) {
        PACL acl = nullptr; PSECURITY_DESCRIPTOR security = nullptr;
        Check(GetNamedSecurityInfoW(const_cast<wchar_t*>(raw.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &acl, nullptr, &security) == ERROR_SUCCESS && acl && acl->AceCount == 2,
            "existing raw directory and temporary metadata receive only SYSTEM/Admin ACEs");
        if (security) LocalFree(security);
    } else std::cout << "[INFO] raw owner/ACL repair needs elevated Windows token; not claimed passed\n";
    SetAcl(raw, allow); SetAcl(raw + L"\\snapshot.tmp", allow);
    std::error_code cleanup; std::filesystem::remove_all(base, cleanup);
    Check(!cleanup, "isolated security fixtures clean up");
    return failures ? 1 : 0;
}
