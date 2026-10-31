// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#ifdef PULSE_WITH_SELFTEST
#include "folder_sizes.h"
#include "../index/index_feed.h"
#include <filesystem>
#include <fstream>
#include <thread>

namespace pulse::app {
namespace {
class FolderSizePipeFixture {
public:
    std::atomic<uint64_t> bytes{800};
    std::atomic<bool> available{true}, stall{false};
    std::atomic<unsigned> requests{0};
    FolderSizePipeFixture() {
        wchar_t old[256]{};
        const auto count = GetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", old, 256);
        if (count && count < 256) previous_ = old;
        name_ = L"\\\\.\\pipe\\PulseFolderSizeTest-" + std::to_wstring(GetCurrentProcessId());
        SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", name_.c_str());
        thread_ = std::thread([this] { Run(); });
        WaitForSingleObject(ready_, 3000);
    }
    ~FolderSizePipeFixture() {
        SetEvent(stop_); CancelSynchronousIo(thread_.native_handle()); thread_.join();
        CloseHandle(ready_); CloseHandle(stop_);
        SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", previous_.empty() ? nullptr : previous_.c_str());
    }
private:
    HANDLE ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread_;
    std::wstring previous_, name_;
    static bool Transfer(HANDLE pipe, void* data, DWORD count, bool write) {
        auto* bytes = static_cast<BYTE*>(data);
        while (count) {
            DWORD done = 0;
            const bool ok = (write ? WriteFile(pipe, bytes, count, &done, nullptr) : ReadFile(pipe, bytes, count, &done, nullptr)) != FALSE;
            if (!ok || !done) return false;
            count -= done; bytes += done;
        }
        return true;
    }
    void Run() {
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            HANDLE pipe = CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
            SetEvent(ready_);
            if (pipe == INVALID_HANDLE_VALUE) return;
            const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            while (connected && WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
                ipc::MsgHeader header{};
                if (!Transfer(pipe, &header, sizeof(header), false) || header.magic != index::kFeedMagic ||
                    header.type != index::kFolderSizeRequest || header.payload_size > 256 * 1024) break;
                std::vector<uint8_t> payload(header.payload_size);
                if (!Transfer(pipe, payload.data(), header.payload_size, false)) break;
                ipc::PayloadReader reader(payload.data(), payload.size()); uint32_t version = 0, count = 0;
                if (!reader.GetU32(version) || version != 1 || !reader.GetU32(count) || count > index::kFolderSizeBatch) break;
                std::vector<index::IndexedFolderSize> values;
                for (uint32_t i = 0; i < count; ++i) {
                    std::wstring path;
                    if (!reader.GetString(path)) break;
                    values.push_back({available.load(), path.ends_with(L"\\empty") ? 0 : bytes.load()});
                }
                ++requests;
                while (stall && WaitForSingleObject(stop_, 5) != WAIT_OBJECT_0) {}
                if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
                ipc::PayloadWriter writer; index::PutFolderSizes(writer, values);
                header.type = index::kFolderSizeResponse; header.payload_size = static_cast<uint32_t>(writer.data().size());
                if (!Transfer(pipe, &header, sizeof(header), true) ||
                    !Transfer(pipe, const_cast<uint8_t*>(writer.data().data()), header.payload_size, true)) break;
            }
            DisconnectNamedPipe(pipe); CloseHandle(pipe);
        }
    }
};
}
bool RunFolderSizeIndexClientTest(const std::filesystem::path& fixture, std::ofstream& log) {
    FolderSizePipeFixture pipe;
    bool ok = true;
    auto check = [&](bool passed, const char* label) { log << (passed ? "[PASS] " : "[FAIL] ") << label << std::endl; ok &= passed; };
    auto wait = [](const auto& predicate) {
        const auto until = GetTickCount64() + 5000;
        while (GetTickCount64() < until) { if (predicate()) return true; Sleep(10); }
        return predicate();
    };
    const auto nested = (fixture / L"nested").wstring(), empty = (fixture / L"empty").wstring();
    {
        FolderSizes sizes;
        sizes.Sync({{nested}, {empty}}, {});
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Indexed; }) && sizes.Get(nested).bytes == 800,
            "real pipe client publishes indexed estimate before directory scan");
        check(sizes.Get(empty).state == FolderSizeState::Indexed && sizes.Get(empty).has_value && sizes.Get(empty).bytes == 0,
            "batched IPC distinguishes indexed empty folder from unknown");
        pipe.bytes = 1200;
        check(wait([&] { return sizes.Get(nested).bytes == 1200; }), "visible indexed sizes refresh from service updates");
        pipe.available = false;
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567,
            "journal gap or unavailable index falls back to actual directory scan");
        pipe.available = true;
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Indexed; }), "index recovery restores the fast path");
        sizes.Calculate(nested);
        check(wait([&] { return sizes.Get(nested).state == FolderSizeState::Ready; }) && sizes.Get(nested).bytes == 13567,
            "clicking estimate requests exact statistics instead of accepting index exclusions");
        const auto previous_requests = pipe.requests.load();
        check(wait([&] { return pipe.requests > previous_requests; }) && sizes.Get(nested).state == FolderSizeState::Ready,
            "later service polls cannot overwrite manually verified totals");
        sizes.Stop();
    }
    Sleep(20);
    {
        FolderSizes sizes;
        std::vector<FolderSizeRequest> visible;
        for (unsigned i = 0; i < 140; ++i) visible.push_back({(fixture / (L"batch-" + std::to_wstring(i))).wstring()});
        sizes.Sync(visible, {});
        check(wait([&] {
            for (const auto& row : visible) if (sizes.Get(row.path).state != FolderSizeState::Indexed) return false;
            return true;
        }), "visible folders beyond one IPC batch all receive indexed totals");
        pipe.bytes = 2400;
        check(wait([&] {
            for (const auto& row : visible) if (sizes.Get(row.path).bytes != 2400) return false;
            return true;
        }), "rotating batches keep all visible indexed totals fresh");
        sizes.Stop();
    }
    Sleep(20);
    pipe.stall = true;
    {
        const auto before = pipe.requests.load();
        FolderSizes sizes; sizes.Sync({{nested}}, {});
        check(wait([&] { return pipe.requests > before; }), "cancellation fixture reaches an in-flight service request");
        sizes.Sync({{empty}}, {});
        const auto start = GetTickCount64(); sizes.Stop();
        check(GetTickCount64() - start < 1000 && sizes.Get(empty).bytes != 2400,
            "navigation rejects stale responses and stop cancels stalled IPC promptly");
    }
    return ok;
}
}
#endif
