// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ops/ops_manager.h"
#include "ops/transfer_storage.h"
#include "ops/transfer_target_listing.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace pulse::ops {
struct TransferIntegrationProbe {
    static void Initialize(OpsManager& manager, decltype(&CopyFile2) copy, bool move = false,
                           bool verify = false) {
        manager.running_ = true;
        manager.accepting_ = true;
        manager.transfer_copy_file_ = copy;
        manager.transfer_parallel_override_ = true;
        manager.force_copy_move_ = move;
        manager.verify_copies_.store(verify);
    }
    static void Notify(OpsManager& manager, std::function<void()> notify) { manager.notify_ = std::move(notify); }
    static void IdentityQuery(OpsManager& manager, decltype(&GetFileInformationByHandleEx) query) {
        manager.transfer_identity_query_ = query;
    }
    static void Cancel(OpsManager& manager) { manager.transfer_cancel_.store(true); }
    static void Run(OpsManager& manager, const OpRequest& request) { manager.RunTransfer(request, 41); }
    static bool UndoMatches(const OpsManager& manager, const OpRequest& request) {
        if (manager.undo_.size() != 1) return false;
        const auto& undo = manager.undo_.back();
        if (undo.type != request.type || undo.sources != request.sources ||
                undo.destinations.size() != request.sources.size() ||
                undo.produced_identities.size() != request.sources.size()) return false;
        for (size_t i = 0; i < undo.sources.size(); ++i) {
            if (undo.destinations[i] != (std::filesystem::path(request.dest_dir) /
                    std::filesystem::path(request.sources[i]).filename()).wstring() ||
                    !undo.produced_identities[i].Valid()) return false;
        }
        return true;
    }
    static bool RunMoveUndo(OpsManager& manager) {
        manager.Undo();
        if (manager.queue_.size() != 1) return false;
        auto item = std::move(manager.queue_.front());
        manager.queue_.pop_front();
        manager.ExecuteFileOperation(item);
        return manager.undo_.empty() && manager.Status().phase == OpPhase::Completed;
    }
};
} // namespace pulse::ops

namespace {
namespace fs = std::filesystem;
using namespace pulse::ops;
int failures = 0;
void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
void Write(const fs::path& path, size_t size, char value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::string block(65536, value);
    while (size) {
        const size_t count = (std::min)(size, block.size());
        output.write(block.data(), static_cast<std::streamsize>(count));
        size -= count;
    }
    if (!output) throw std::runtime_error("fixture write failed");
}
bool Content(const fs::path& path, size_t size, char value) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return bytes.size() == size && std::all_of(bytes.begin(), bytes.end(),
        [value](char byte) { return byte == value; });
}
void Maximum(std::atomic<int>& maximum, int value) {
    int previous = maximum.load();
    while (previous < value && !maximum.compare_exchange_weak(previous, value)) {}
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
bool RenameFixtureObject(const fs::path& source, const fs::path& destination) {
    if (MoveFileW(source.c_str(), destination.c_str())) return true;
    HANDLE handle = CreateFileW(source.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto name = destination.wstring();
    std::vector<unsigned char> buffer(offsetof(FILE_RENAME_INFO, FileName) + name.size() * sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
    const bool renamed = SetFileInformationByHandle(handle, FileRenameInfo, rename, static_cast<DWORD>(buffer.size())) != FALSE;
    const DWORD error = renamed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    SetLastError(error);
    return renamed;
}
enum class Mode { Normal, Cancel, Fail, ReplaceSource, IdentityMismatch, ReplaceDestination };
struct CopyState {
    Mode mode = Mode::Normal;
    OpsManager* manager = nullptr;
    fs::path destination;
    std::atomic<int> active{0}, maximum{0}, large_active{0}, large_maximum{0};
    std::atomic<int> calls{0};
    std::atomic<DWORD> injection_error{ERROR_SUCCESS};
    std::atomic<DWORD> retained_copy_read_error{ERROR_SUCCESS};
    FileIdentity identity_target;
    std::atomic<int> mutated_identity_queries{0};
    bool verify = false;
    std::atomic<bool> small_overlapped{false}, injected{false}, injection_ok{true};
    bool monotonic = true, bounded = true, progress_during_copy = false;
    uint64_t previous_bytes = 0;
    int verifications = 0;
};
CopyState* state = nullptr; // one synchronous RunTransfer; it joins all workers before returning
BOOL WINAPI ControlledIdentityQuery(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS info_class, LPVOID output, DWORD size) {
    BOOL result = GetFileInformationByHandleEx(handle, info_class, output, size);
    if (info_class != FileIdInfo || size < sizeof(FILE_ID_INFO)) return result;
    if (!result) {
        const DWORD error = GetLastError();
        if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
                error != ERROR_INVALID_FUNCTION && error != ERROR_CALL_NOT_IMPLEMENTED) return FALSE;
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (!GetFileInformationByHandle(handle, &legacy)) return FALSE;
        const ULONGLONG index = (static_cast<ULONGLONG>(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
        auto& identity = *static_cast<FILE_ID_INFO*>(output);
        identity = {};
        identity.VolumeSerialNumber = legacy.dwVolumeSerialNumber;
        std::memcpy(identity.FileId.Identifier, &index, sizeof(index));
        result = TRUE;
    }
    auto& identity = *static_cast<FILE_ID_INFO*>(output);
    if (state && state->mode == Mode::IdentityMismatch && state->injected.load() &&
            identity.VolumeSerialNumber == state->identity_target.value.VolumeSerialNumber &&
            std::memcmp(&identity.FileId, &state->identity_target.value.FileId, sizeof(identity.FileId)) == 0) {
        identity.FileId.Identifier[0] ^= 0x80;
        ++state->mutated_identity_queries;
    }
    return result;
}
struct ProgressForward {
    PCOPYFILE2_PROGRESS_ROUTINE original = nullptr;
    void* context = nullptr;
    CopyState* observed = nullptr;
    bool large = false;
};
COPYFILE2_MESSAGE_ACTION CALLBACK ForwardProgress(const COPYFILE2_MESSAGE* message, void* raw) {
    auto& forward = *static_cast<ProgressForward*>(raw);
    if (forward.large && message->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED) Sleep(60);
    if (forward.observed->mode == Mode::Cancel &&
            message->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED && !forward.observed->injected.exchange(true))
        TransferIntegrationProbe::Cancel(*forward.observed->manager);
    return forward.original ? forward.original(message, forward.context) : COPYFILE2_PROGRESS_CONTINUE;
}
HRESULT WINAPI ControlledCopy(PCWSTR source, PCWSTR destination, COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    auto& observed = *state;
    const auto size = fs::file_size(source);
    const bool large = size >= 1024 * 1024;
    Maximum(observed.maximum, ++observed.active);
    if (large) Maximum(observed.large_maximum, ++observed.large_active);
    else if (observed.large_active.load()) observed.small_overlapped.store(true);
    ++observed.calls;
    struct Retire {
        CopyState& observed;
        bool large;
        ~Retire() { if (large) --observed.large_active; --observed.active; }
    } retire{observed, large};
    // Hold lanes long enough to observe actual overlap, including on fast SSDs.
    Sleep(150);
    if (!large && observed.large_active.load()) observed.small_overlapped.store(true);
    if (observed.mode == Mode::IdentityMismatch) observed.injected.store(true);
    if (observed.mode == Mode::ReplaceSource && !observed.injected.exchange(true)) {
        const fs::path old = fs::path(source).wstring() + L".original";
        if (!RenameFixtureObject(source, old)) {
            observed.injection_ok.store(false);
            observed.injection_error.store(GetLastError());
        }
        else Write(source, static_cast<size_t>(size), 'R');
    }
    auto forwarded = *parameters;
    ProgressForward progress{parameters->pProgressRoutine, parameters->pvCallbackContext, &observed, large};
    forwarded.pProgressRoutine = ForwardProgress;
    forwarded.pvCallbackContext = &progress;
    const HRESULT result = CopyFile2(source, destination, &forwarded);
    if (SUCCEEDED(result) && observed.verify) {
        const HANDLE read = CreateFileW(destination, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (read == INVALID_HANDLE_VALUE) observed.retained_copy_read_error.store(GetLastError());
        else CloseHandle(read);
    }
    if (SUCCEEDED(result) && observed.mode == Mode::ReplaceDestination && !observed.injected.exchange(true))
        Write(observed.destination / fs::path(source).filename(), 29, 'D');
    if (SUCCEEDED(result) && observed.mode == Mode::Fail) return E_ACCESSDENIED;
    return result;
}
struct Case {
    fs::path root, source, destination;
    std::vector<size_t> sizes;
    OpRequest request;
    Case(const fs::path& fixture, const wchar_t* name, std::vector<size_t> bytes)
        : root(fixture / name), source(root / L"source"), destination(root / L"destination"), sizes(std::move(bytes)) {
        fs::create_directories(source);
        fs::create_directories(destination);
        request.dest_dir = destination.wstring();
        for (size_t i = 0; i < sizes.size(); ++i) {
            const auto path = source / (std::to_wstring(i) + L".bin");
            Write(path, sizes[i], static_cast<char>('a' + i));
            request.sources.push_back(path.wstring());
        }
    }
    bool Sources(bool present = true) const {
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (present ? !Content(request.sources[i], sizes[i], static_cast<char>('a' + i))
                        : fs::exists(request.sources[i])) return false;
        }
        return true;
    }
    bool Destinations() const {
        for (size_t i = 0; i < sizes.size(); ++i)
            if (!Content(destination / fs::path(request.sources[i]).filename(), sizes[i], static_cast<char>('a' + i)))
                return false;
        return true;
    }
    bool NoTemporary() const {
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            const auto name = item.path().filename().wstring();
            if (name.find(L".pulse-copy-") != std::wstring::npos ||
                    name.find(L".pulse-backup-") != std::wstring::npos) return false;
        }
        return true;
    }
};
void Configure(OpsManager& manager, CopyState& observed, const Case& test, bool move = false, bool verify = false) {
    observed.manager = &manager;
    observed.destination = test.destination;
    observed.verify = verify;
    if (observed.mode == Mode::IdentityMismatch &&
            !pulse::ReadPathFileIdentity(test.request.sources.front(), observed.identity_target))
        throw std::runtime_error("identity mismatch fixture cannot identify source");
    state = &observed;
    TransferIntegrationProbe::Initialize(manager, ControlledCopy, move, verify);
    if (observed.mode == Mode::IdentityMismatch) TransferIntegrationProbe::IdentityQuery(manager, ControlledIdentityQuery);
    TransferIntegrationProbe::Notify(manager, [&manager, &observed, case_name = test.root.filename().wstring()] {
        const auto status = manager.Status();
        if (status.phase == OpPhase::Running) {
            observed.monotonic &= status.transferred_bytes >= observed.previous_bytes;
            observed.bounded &= status.transferred_bytes <= status.total_bytes;
            observed.previous_bytes = status.transferred_bytes;
            observed.progress_during_copy |= observed.active.load() > 0 && status.transferred_bytes > 0 &&
                status.transferred_bytes < status.total_bytes;
        }
        if (status.phase == OpPhase::Verifying) ++observed.verifications;
        if (status.phase == OpPhase::Completed || status.phase == OpPhase::Failed || status.phase == OpPhase::Cancelled) {
            std::printf("[DIAG] case=%s phase=%d hr=0x%08lX failed=%s error=%s calls=%d peak=%d large_peak=%d "
                "small_overlap=%d progress=%d monotonic=%d bounded=%d bytes=%llu/%llu injection_error=%lu retained_copy_read_error=%lu mutated_id_queries=%d\n",
                Utf8(case_name).c_str(), static_cast<int>(status.phase), static_cast<unsigned long>(status.failure_hr),
                Utf8(status.failed_path).c_str(), Utf8(status.last_error).c_str(), observed.calls.load(), observed.maximum.load(),
                observed.large_maximum.load(), static_cast<int>(observed.small_overlapped.load()),
                static_cast<int>(observed.progress_during_copy), static_cast<int>(observed.monotonic),
                static_cast<int>(observed.bounded), static_cast<unsigned long long>(status.transferred_bytes),
                static_cast<unsigned long long>(status.total_bytes), observed.injection_error.load(),
                observed.retained_copy_read_error.load(), observed.mutated_identity_queries.load());
        }
    });
}
void RunTests(const fs::path& fixture) {
    constexpr size_t mib = 1024 * 1024;
    {
        const auto folder = fixture / L"listing";
        fs::create_directories(folder);
        Write(folder / L"Present.txt", 17, 'L');
        TransferTargetListing listing(folder.wstring(), 16);
        Check(listing.SurelyAbsent(L"missing.txt") && !listing.SurelyAbsent(L"present.TXT") &&
            !listing.SurelyAbsent(L"missing.txt.") && !listing.SurelyAbsent(L"MISSING~1.TXT") &&
            !listing.SurelyAbsent(L"missing:stream"), "bulk target listing keeps case aliases and ambiguous Win32 names conservative");
        listing.Added(L"arrived.txt");
        Check(!listing.SurelyAbsent(L"ARRIVED.txt") &&
            !TransferTargetListing(folder.wstring(), 1).SurelyAbsent(L"missing.txt"),
            "bulk target listing tracks admitted names and small batches use direct probes");
    }
    {
        Case test(fixture, L"parallel", {12001, 25003, 33007, 8101, 9001, 18001});
        OpsManager manager;
        CopyState observed;
        Configure(manager, observed, test);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(manager.Status().phase == OpPhase::Completed && observed.maximum >= 2,
            "nonconflicting production copy uses concurrent real CopyFile2 calls");
        Check(test.Sources() && test.Destinations() && test.NoTemporary() &&
            TransferIntegrationProbe::UndoMatches(manager, test.request), "parallel contents, sources, cleanup and copy undo identities");
    }
    {
        Case test(fixture, L"mixed", {8 * mib, 2 * mib, 31003, 71009});
        OpsManager manager;
        CopyState observed;
        Configure(manager, observed, test);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(manager.Status().phase == OpPhase::Completed && observed.small_overlapped && observed.large_maximum == 1,
            "large/large/small lookahead overlaps small work with one large stream");
        Check(observed.monotonic && observed.bounded && observed.progress_during_copy &&
            manager.Status().transferred_bytes == manager.Status().total_bytes && test.Destinations() && test.NoTemporary(),
            "live large-file aggregate progress is bounded and monotonic with correct final bytes");
    }
    {
        Case test(fixture, L"move", {38003, 19009, 8009, 9901});
        test.request.type = OpType::Move;
        OpsManager manager;
        CopyState observed;
        Configure(manager, observed, test, true);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(manager.Status().phase == OpPhase::Completed && observed.maximum >= 2 &&
            test.Sources(false) && test.Destinations() && TransferIntegrationProbe::UndoMatches(manager, test.request),
            "forced cross-volume move commits copies then deletes checked sources and records undo");
        Check(TransferIntegrationProbe::RunMoveUndo(manager) && test.Sources() &&
            fs::is_empty(test.destination) && test.NoTemporary(), "production move undo restores all contents and consumes undo entry");
    }
    for (const auto mode : {Mode::Cancel, Mode::Fail}) {
        Case test(fixture, mode == Mode::Cancel ? L"cancel" : L"failure", {4 * mib, 17011, 23003});
        test.request.type = OpType::Move;
        OpsManager manager;
        CopyState observed;
        observed.mode = mode;
        Configure(manager, observed, test, true);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(manager.Status().phase == (mode == Mode::Cancel ? OpPhase::Cancelled : OpPhase::Failed) &&
            test.Sources() && fs::is_empty(test.destination) && test.NoTemporary(),
            mode == Mode::Cancel ? "cancel joins workers, removes pending copies and preserves move sources"
                                 : "copy failure removes all pending copies and preserves move sources");
    }
    {
        Case test(fixture, L"source-rename-blocked", {37003});
        DestinationGuard::Handle deny_rename(CreateFileW(test.request.sources[0].c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        const bool held = deny_rename.get() != INVALID_HANDLE_VALUE;
        if (!held) deny_rename.release();
        OpsManager manager;
        CopyState observed;
        observed.mode = Mode::ReplaceSource;
        Configure(manager, observed, test);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(held && !observed.injection_ok && observed.injection_error == ERROR_SHARING_VIOLATION &&
            manager.Status().phase == OpPhase::Completed && test.Sources() && test.Destinations() &&
            !fs::exists(test.request.sources[0] + L".original") && test.NoTemporary(),
            "denied source rename preserves the original source and successfully copied content");
    }
    {
        Case test(fixture, L"source-identity-mismatch", {37003, 19001});
        test.request.type = OpType::Move;
        OpsManager manager;
        CopyState observed;
        observed.mode = Mode::IdentityMismatch;
        Configure(manager, observed, test, true);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(observed.mutated_identity_queries > 0 && manager.Status().phase == OpPhase::Failed &&
            manager.Status().failure_hr == HRESULT_FROM_WIN32(ERROR_FILE_INVALID) && test.Sources() &&
            fs::is_empty(test.destination) && test.NoTemporary(),
            "source-only identity mismatch rejects parallel move and preserves sources without committed copies");
    }
    {
        Case test(fixture, L"destination-change", {37003});
        OpsManager manager;
        CopyState observed;
        observed.mode = Mode::ReplaceDestination;
        Configure(manager, observed, test);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(manager.Status().phase == OpPhase::Failed && test.Sources() &&
            Content(test.destination / L"0.bin", 29, 'D') && test.NoTemporary(),
            "late destination creation rejects commit without overwriting the new object");
    }
    for (const bool verify : {false, true}) {
        Case test(fixture, verify ? L"verify" : L"conflict", {31003, 17011, 23003});
        if (!verify) {
            test.request.collision_policy = CollisionPolicy::Replace;
            for (const auto& source : test.request.sources) Write(test.destination / fs::path(source).filename(), 11, 'X');
        }
        OpsManager manager;
        CopyState observed;
        Configure(manager, observed, test, false, verify);
        TransferIntegrationProbe::Run(manager, test.request);
        Check(manager.Status().phase == OpPhase::Completed && observed.maximum == 1 &&
            test.Sources() && test.Destinations() && test.NoTemporary() && (!verify || observed.verifications == 3),
            verify ? "hash verification retains serial copy fallback and verifies every file"
                   : "replacement conflicts retain serial guarded copy fallback");
    }
}
} // namespace

int main() {
    const auto parent = (fs::current_path() / L"bench_data").lexically_normal();
    const auto fixture = parent / (L"transfer-pipeline-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    try {
        fs::create_directories(parent);
        if (!fs::create_directory(fixture)) throw std::runtime_error("fixture already exists");
        const auto policy = TransferPolicy(fixture.wstring(), fixture.wstring());
        Check(policy.threads >= 2, "fixture storage admits at least two copy lanes");
        if (policy.threads >= 2) RunTests(fixture);
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    const auto cleanup = fs::absolute(fixture).lexically_normal();
    const bool owned = cleanup.parent_path() == fs::absolute(parent).lexically_normal() &&
        cleanup.filename() == fixture.filename() && !fs::is_symlink(cleanup);
    Check(owned, "validate unique absolute fixture path before recursive cleanup");
    if (owned) {
        std::error_code error;
        fs::remove_all(cleanup, error);
        Check(!error && !fs::exists(cleanup), "isolated fixture cleanup completes");
    }
    std::printf("TRANSFER PIPELINE failures=%d\n", failures);
    return failures ? 1 : 0;
}
