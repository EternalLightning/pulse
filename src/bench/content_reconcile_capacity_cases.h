// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once

namespace {
struct ContentFixtureProcess {
    PROCESS_INFORMATION process{};
    ~ContentFixtureProcess() {
        if (!process.hProcess) return;
        if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    bool Start(const std::filesystem::path& executable, std::wstring arguments) {
        auto command = L"\"" + executable.wstring() + L"\" " + arguments;
        STARTUPINFOW startup{sizeof(startup)};
        return CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    }
};
struct ContentFixtureEnvironment {
    const wchar_t* key;
    std::wstring saved;
    ContentFixtureEnvironment(const wchar_t* name, const std::wstring& value) : key(name), saved(32768, L'\0') {
        const auto n = GetEnvironmentVariableW(name, saved.data(), static_cast<DWORD>(saved.size()));
        saved.resize(n < saved.size() ? n : 0); SetEnvironmentVariableW(name, value.c_str());
    }
    ~ContentFixtureEnvironment() { SetEnvironmentVariableW(key, saved.empty() ? nullptr : saved.c_str()); }
};
HANDLE OpenContentFixturePipe(const std::wstring& pipe, DWORD timeout = 5000) {
    const auto deadline = GetTickCount64() + timeout;
    do {
        HANDLE handle = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) return handle;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return INVALID_HANDLE_VALUE;
}
bool WriteContentFixturePacket(HANDLE pipe, uint32_t type, const std::vector<uint8_t>& payload = {}) {
    const auto header = index::content::Header(type, static_cast<uint32_t>(payload.size())); DWORD n = 0;
    return WriteFile(pipe, &header, sizeof(header), &n, nullptr) && n == sizeof(header) &&
        (payload.empty() || (WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &n, nullptr) && n == payload.size()));
}
bool ReadContentFixtureBytes(HANDLE pipe, void* output, DWORD size) {
    const auto deadline = GetTickCount64() + 5000;
    auto* bytes = static_cast<uint8_t*>(output);
    while (size && GetTickCount64() < deadline) {
        DWORD available = 0, read = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (!available) { Sleep(5); continue; }
        if (!ReadFile(pipe, bytes, (std::min)(size, available), &read, nullptr) || !read) return false;
        bytes += read; size -= read;
    }
    return !size;
}
bool ReadContentFixtureDone(HANDLE pipe) {
    for (int i = 0; i < 20; ++i) {
        ipc::MsgHeader header{};
        if (!ReadContentFixtureBytes(pipe, &header, sizeof(header)) || header.magic != index::content::kMagic ||
            header.type != index::content::RSP_BATCH || header.payload_size > index::content::kMaximumPayload) return false;
        std::vector<uint8_t> payload(header.payload_size);
        if (!ReadContentFixtureBytes(pipe, payload.data(), header.payload_size)) return false;
        ipc::PayloadReader reader(payload.data(), payload.size()); uint64_t value = 0; uint32_t flags = 0, error = 0;
        if (!reader.GetU64(value) || !reader.GetU64(value) || !reader.GetU64(value) || !reader.GetU32(flags) || !reader.GetU32(error) || error) return false;
        if (flags & 1) return true;
    }
    return false;
}
}

int RunContentSharedRetentionCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto root = base / L"files", cache = base / L"feed-cache";
    std::filesystem::create_directories(root); std::filesystem::create_directories(cache);
    const auto victim = root / L"old.txt";
    Check(Write(victim, "retained_marker") && Write(root / L"healthy.txt", "healthy_marker"), "seed shared retention cache");
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto executable = std::filesystem::path(module).parent_path() / L"Pulse.Index.exe";
    const auto token = std::to_wstring(GetCurrentProcessId()) + L"-retention";
    const std::wstring pipe = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    ContentFixtureEnvironment feed_env(L"PULSE_INDEX_FEED_PIPE", pipe);
    ContentFixtureProcess host;
    Check(host.Start(executable, L"--test-host " + token + L" \"" + root.wstring() + L"\" \"" + cache.wstring() + L"\""), "start isolated retention filename feed");
    if (!host.process.hProcess) return 1;
    bool ready = false;
    const auto deadline = GetTickCount64() + 10000;
    while (!ready && GetTickCount64() < deadline) {
        IndexFeedConnection feed(nullptr); FileFeedPage page;
        ready = feed.Request(false, L"", 0, 0, page) && page.ready; if (!ready) Sleep(25);
    }
    Check(ready, "isolated retention feed ready");
    std::atomic<bool> deny{false}; std::atomic<size_t> denied{0};
    ContentIndexScanHooks hooks;
    hooks.attributes = [&](const auto& path, auto& attributes) {
        if (deny && ContentScopeKey(path) == ContentScopeKey(victim.wstring())) { ++denied; SetLastError(ERROR_ACCESS_DENIED); return false; }
        return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE;
    };
    {
        ContentIndex db((base / L"retention.sqlite").wstring(), ContentAgentMode::LegacyWriter, hooks);
        ContentIndexConfig config; config.roots = {{root.wstring()}};
        Check(db.Configure(config) && db.WaitUntilIdle(10000), "initial local cache contains both files");
        deny = true; config.shared_scope = true;
        Check(db.Configure(config) && db.WaitUntilIdle(15000), "shared crawl finishes despite injected attribute error");
        ContentSearchRequest request; request.needle = L"retained_marker";
        const auto status = db.Status();
        Check(denied && Search(db, request).size() == 1 && status.indexed_files == 2,
            "temporary attribute failure preserves old cached document");
        Check(status.root_status.size() == 1 && status.root_status[0].error == ERROR_ACCESS_DENIED &&
            status.root_status[0].state == ContentIndexRootStatus::State::AccessFailed,
            "shared incomplete crawl publishes per root failure");
        deny = false;
        config.excluded_directories.push_back(L"unused-recovery");
        Check(db.Configure(config) && db.WaitUntilIdle(15000), "shared crawl recovers after temporary attribute failure");
        Check(Search(db, request).size() == 1 && db.Status().root_status.size() == 1 && !db.Status().root_status[0].error,
            "restored attributes retain document and clear root error");
        Write(victim, "updated_marker"); request.needle = L"updated_marker";
        Check(AwaitHit(db, request, 1), "restored shared file updates cached body");
        std::filesystem::remove(victim);
        Check(AwaitHit(db, request, 0), "real deletion removes old shared document");
    }
    HANDLE control = OpenContentFixturePipe(pipe);
    if (control != INVALID_HANDLE_VALUE) {
        const auto header = MakeIndexHdr(REQ_IDX_TEST_SHUTDOWN, 0, 0); DWORD n = 0;
        WriteFile(control, &header, sizeof(header), &n, nullptr); CloseHandle(control);
    }
    Check(WaitForSingleObject(host.process.hProcess, 5000) == WAIT_OBJECT_0, "isolated retention feed exits");
    return failed ? 1 : 0;
}

int RunContentCapacityCases(const std::filesystem::path& base) {
    using namespace pulse::index;
    const auto profile = base / L"profile";
    std::filesystem::create_directories(profile);
    ContentFixtureEnvironment profile_env(L"LOCALAPPDATA", profile.wstring());
    ContentFixtureEnvironment timing_env(L"PULSE_CONTENT_TIMING_DIR", (base / L"timing").wstring());
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto executable = std::filesystem::path(module);
    const auto token = std::to_wstring(GetCurrentProcessId()) + L"-capacity";
    const auto pipe = content::PipeName(token);
    ContentFixtureProcess host;
    Check(host.Start(executable, L"--capacity-agent-child " + token + L" " + std::to_wstring(GetCurrentProcessId()) +
        L" \"" + (base / L"capacity.sqlite").wstring() + L"\""), "start isolated persistent capacity agent");
    if (!host.process.hProcess) return 1;
    bool ready = false; ContentIndexStatus last_status; bool parsed_status = false;
    const auto ready_deadline = GetTickCount64() + 10000;
    while (!ready && GetTickCount64() < ready_deadline) {
        HANDLE connection = OpenContentFixturePipe(pipe);
        if (connection == INVALID_HANDLE_VALUE) break;
        ipc::MsgHeader header{};
        if (WriteContentFixturePacket(connection, content::REQ_STATUS) && ReadContentFixtureBytes(connection, &header, sizeof(header)) &&
            header.type == content::RSP_STATUS && header.payload_size <= content::kMaximumPayload) {
            std::vector<uint8_t> payload(header.payload_size);
            if (ReadContentFixtureBytes(connection, payload.data(), header.payload_size)) {
                ipc::PayloadReader reader(payload.data(), payload.size()); ContentIndexStatus status;
                parsed_status = content::GetStatus(reader, status); last_status = status;
                ready = parsed_status && !status.error && !status.indexing && !status.pending_files;
            }
        }
        CloseHandle(connection);
        if (!ready) Sleep(25);
    }
    Check(ready, "isolated capacity cache initializes before subscriptions");
    if (!ready) printf("CAPACITY parsed=%d error=%lu pending=%llu indexing=%d process_wait=%lu\n",
        parsed_status, last_status.error, last_status.pending_files, last_status.indexing,
        WaitForSingleObject(host.process.hProcess, 0));
    if (!ready) return 1;
    std::vector<HANDLE> subscriptions;
    bool initialized = true;
    for (uint64_t generation = 1; generation <= 16 && initialized; ++generation) {
        HANDLE connection = OpenContentFixturePipe(pipe);
        if (connection == INVALID_HANDLE_VALUE) { initialized = false; break; }
        subscriptions.push_back(connection);
        ContentSearchRequest request; request.generation = generation; request.needle = L"capacity_marker";
        initialized = WriteContentFixturePacket(connection, content::REQ_SUBSCRIBE, content::SerializeSearchRequest(request)) &&
            ReadContentFixtureDone(connection);
    }
    Check(initialized && subscriptions.size() == 16, "sixteen independent subscriptions reach initial completion");
    Sleep(150);
    bool alive = initialized && WaitForSingleObject(host.process.hProcess, 0) == WAIT_TIMEOUT;
    for (HANDLE connection : subscriptions) alive &= PeekNamedPipe(connection, nullptr, 0, nullptr, nullptr, nullptr) != FALSE;
    Check(alive, "full capacity preserves agent and all existing subscriptions");
    if (!alive) {
        for (HANDLE connection : subscriptions) CloseHandle(connection);
        return failed ? 1 : 0;
    }
    HANDLE overflow = OpenContentFixturePipe(pipe);
    bool rejected = false;
    if (overflow != INVALID_HANDLE_VALUE) {
        ContentSearchRequest request; request.generation = 17; request.needle = L"capacity_marker";
        ipc::MsgHeader header{};
        if (WriteContentFixturePacket(overflow, content::REQ_SUBSCRIBE, content::SerializeSearchRequest(request)) &&
            ReadContentFixtureBytes(overflow, &header, sizeof(header)) && header.payload_size <= content::kMaximumPayload) {
            std::vector<uint8_t> payload(header.payload_size);
            if (ReadContentFixtureBytes(overflow, payload.data(), header.payload_size)) {
                ipc::PayloadReader reader(payload.data(), payload.size()); uint64_t v = 0; uint32_t flags = 0, error = 0;
                rejected = reader.GetU64(v) && reader.GetU64(v) && reader.GetU64(v) && reader.GetU32(flags) && reader.GetU32(error) && (flags & 1) && error == ERROR_BUSY;
            }
        }
        CloseHandle(overflow);
    }
    Check(rejected && WaitForSingleObject(host.process.hProcess, 0) == WAIT_TIMEOUT,
        "seventeenth subscription receives explicit busy while original sessions survive");
    auto control = [&](uint32_t type, uint64_t generation = 0) {
        HANDLE connection = OpenContentFixturePipe(pipe);
        if (connection == INVALID_HANDLE_VALUE) return false;
        ipc::PayloadWriter writer; if (type == content::REQ_CANCEL) writer.PutU64(generation);
        ipc::MsgHeader header{};
        bool ok = WriteContentFixturePacket(connection, type, writer.data()) && ReadContentFixtureBytes(connection, &header, sizeof(header)) &&
            header.type == content::RSP_STATUS && header.payload_size <= content::kMaximumPayload;
        if (ok) { std::vector<uint8_t> payload(header.payload_size); ok = ReadContentFixtureBytes(connection, payload.data(), header.payload_size); }
        CloseHandle(connection); return ok;
    };
    Check(control(content::REQ_CANCEL, 1), "reserved connection admits cancellation at query capacity");
    bool retired = subscriptions.empty();
    const auto retire_deadline = GetTickCount64() + 5000;
    while (!retired && GetTickCount64() < retire_deadline) {
        retired = !PeekNamedPipe(subscriptions.front(), nullptr, 0, nullptr, nullptr, nullptr);
        if (!retired) Sleep(10);
    }
    Check(retired, "cancelled original subscription releases its pipe instance");
    HANDLE replacement = OpenContentFixturePipe(pipe);
    bool recovered = false;
    if (replacement != INVALID_HANDLE_VALUE) {
        ContentSearchRequest request; request.generation = 18; request.needle = L"capacity_marker";
        recovered = WriteContentFixturePacket(replacement, content::REQ_SUBSCRIBE, content::SerializeSearchRequest(request)) && ReadContentFixtureDone(replacement);
    }
    Check(recovered, "subscription admission recovers after cancellation frees capacity");
    bool remaining_alive = true;
    for (size_t i = 1; i < subscriptions.size(); ++i) remaining_alive &= PeekNamedPipe(subscriptions[i], nullptr, 0, nullptr, nullptr, nullptr) != FALSE;
    Check(remaining_alive, "other original subscriptions remain connected through recovery");
    Check(control(content::REQ_SHUTDOWN), "reserved connection admits shutdown at query capacity");
    for (HANDLE connection : subscriptions) CloseHandle(connection);
    if (replacement != INVALID_HANDLE_VALUE) CloseHandle(replacement);
    Check(WaitForSingleObject(host.process.hProcess, 5000) == WAIT_OBJECT_0, "capacity agent shuts down without forced termination");
    return failed ? 1 : 0;
}
