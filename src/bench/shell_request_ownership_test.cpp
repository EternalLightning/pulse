// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// Include the production dispatcher so early returns are tested without copying it.
// Build this isolated target with /Gy /Gw and link /OPT:REF.
#include <windows.h>
#include <shobjidl.h>
#include <array>
#include <cstdlib>
#include <new>
#include <type_traits>

HRESULT WINAPI OwnershipCoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static int WINAPI UnusedShellHostMain(HINSTANCE, HINSTANCE, LPWSTR, int);
#define CoCreateInstance OwnershipCoCreateInstance
#define wWinMain UnusedShellHostMain
#pragma warning(push)
#pragma warning(disable: 4505)
#include "../shell_host/main.cpp"
#pragma warning(pop)
#undef wWinMain
#undef CoCreateInstance

namespace {
struct Allocation {
    void* address = nullptr;
    unsigned releases = 0;
};
std::array<Allocation, 8> allocations;
size_t allocation_count = 0;
bool observing = false;
int failures = 0;
int device_calls = 0;
int recycle_calls = 0;
int com_calls = 0;
HRESULT device_result = S_OK;
bool device_throws = false;

void Observe(void* address) {
    if (address) allocations[allocation_count++] = {address, 0};
}
void ReleaseAllocation(void* address) noexcept {
    bool duplicate = false;
    if (observing && address) {
        for (size_t i = 0; i < allocation_count; ++i) {
            if (allocations[i].address == address) duplicate |= ++allocations[i].releases > 1;
        }
    }
    if (!duplicate) std::free(address);
}
void CheckOwnership(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    failures += !passed;
}
void RunCase(uint32_t type, const char* label, bool authorized = false, bool throws = false) {
    auto* request = new Request;
    request->type = type;
    request->id = 100;
    if (type != REQ_NEW_FOLDER && type != REQ_NEW_FILE)
        request->sources.push_back(L"C:\\PulseOwnershipFixture\\" + std::wstring(80, L'x') + L".txt");
    request->device_operation.verb.assign(80, L'v');
    request->device_operation.name.assign(80, L'n');
    request->device_operation.ids.emplace_back(80, L'i');
    if (authorized) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            delete request;
            CheckOwnership(false, "read current host epoch");
            return;
        }
        request->delete_epoch = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        static uint64_t token = 0;
        request->delete_token = ++token;
    } else {
        request->expected_identities.resize(1);
    }
    allocation_count = 0;
    Observe(request);
    if (!request->sources.empty()) {
        Observe(request->sources.data());
        Observe(request->sources.front().data());
    }
    Observe(request->device_operation.verb.data());
    Observe(request->device_operation.name.data());
    Observe(request->device_operation.ids.data());
    Observe(request->device_operation.ids.front().data());
    Observe(request->expected_identities.data());
    const int device_before = device_calls, recycle_before = recycle_calls, com_before = com_calls;
    device_throws = throws;
    observing = true;
    bool caught = false;
    try {
        HostWndProc(nullptr, WM_EXEC_REQUEST, 0, reinterpret_cast<LPARAM>(request));
    } catch (int) {
        caught = true;
    }
    observing = false;
    bool released_once = caught == throws;
    for (size_t i = 0; i < allocation_count; ++i) released_once &= allocations[i].releases == 1;
    if (type == REQ_DEVICE_OPERATION) released_once &= device_calls == device_before + 1;
    if (type == REQ_AUTHORIZED_RECYCLE && authorized) released_once &= recycle_calls == recycle_before + 1;
    if (type == UINT32_MAX) released_once &= com_calls == com_before;
    CheckOwnership(released_once, label);
}
}

void* operator new(size_t size) {
    if (void* address = std::malloc(size ? size : 1)) return address;
    throw std::bad_alloc();
}
void operator delete(void* address) noexcept { ReleaseAllocation(address); }
void operator delete(void* address, size_t) noexcept { ReleaseAllocation(address); }

HRESULT WINAPI OwnershipCoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID* result) {
    ++com_calls;
    *result = nullptr;
    return E_FAIL;
}

namespace pulse::shell {
// These side-effect boundaries do not touch devices, recycle bins, or helper processes.
ContextSessionProcesses::~ContextSessionProcesses() = default;
ShellLaunchProcesses::~ShellLaunchProcesses() = default;
HRESULT ExecuteDeviceOperation(const DeviceOperation&) {
    ++device_calls;
    if (device_throws) throw 1;
    return device_result;
}
RecycleResult RecycleAuthorized(const std::vector<std::wstring>&,
    const RecycleCancelled&, const RecycleProgress&, const std::vector<FileIdentity>&) {
    ++recycle_calls;
    RecycleResult result;
    result.error = E_INVALIDARG;
    return result;
}
}

int main() {
    static_assert(std::is_same_v<decltype(&ExecuteRequest), void(*)(std::unique_ptr<Request>)>);
    g.running.store(false);
    RunCase(REQ_DEVICE_OPERATION, "device success releases the request and all nested payloads once");
    device_result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    RunCase(REQ_DEVICE_OPERATION, "device cancellation releases the request and all nested payloads once");
    device_result = E_FAIL;
    RunCase(REQ_DEVICE_OPERATION, "device failure releases the request and all nested payloads once");
    RunCase(REQ_DEVICE_OPERATION, "device exception unwinds the owning request exactly once", false, true);
    RunCase(REQ_DELETE_RECYCLE, "legacy recycle rejection releases the request once");
    RunCase(REQ_REALDELETE, "legacy permanent-delete rejection releases the request once");
    RunCase(REQ_AUTHORIZED_DELETE, "authorization rejection releases the request once");
    // The permanent-delete request deliberately has no identities, so its real
    // validator returns E_INVALIDARG before opening or changing any path.
    RunCase(REQ_AUTHORIZED_DELETE, "admitted permanent-delete validation failure releases the request once", true);
    RunCase(REQ_AUTHORIZED_RECYCLE, "admitted recycle completion releases the request once", true);
    RunCase(REQ_NEW_FOLDER, "empty new-folder request releases the request once");
    RunCase(REQ_NEW_FILE, "empty new-file request releases the request once");
    RunCase(UINT32_MAX, "unknown request rejects without COM setup and releases the request once");
    return failures ? 1 : 0;
}
