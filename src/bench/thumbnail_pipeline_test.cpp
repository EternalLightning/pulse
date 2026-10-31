// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../ui/thumbnail_cache.h"
#include "../ui/thumbnail_disk_cache.h"
#include "../ipc/preview_response.h"
#include <algorithm>
#include <cstdio>
#include <set>

namespace pulse::ui {
struct ThumbnailPipelineTestAccess {
    static int Run(const std::wstring& fixture, const std::wstring& root) {
        unsigned passed = 0, failed = 0;
        auto check = [&](bool value, const char* name) {
            std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
            value ? ++passed : ++failed;
            std::fflush(stdout);
        };
        ipc::PreviewRequest wire;
        wire.kind = ipc::PreviewRequestKind::Thumbnail; wire.pixel_size = 128;
        ipc::PreviewResponse response;
        response.kind = ipc::PreviewContentKind::Bitmap; response.width = 2;
        response.height = 2; response.stride = 8; response.mapping_chars = 24;
        ipc::PreviewResponseLayout layout;
        check(ipc::ValidatePreviewResponse(wire, response, layout) && layout.pixel_bytes == 16,
            "Thumbnail protocol accepts a valid bounded bitmap");
        response.stride = 7;
        check(!ipc::ValidatePreviewResponse(wire, response, layout), "Thumbnail protocol rejects bad pixel metadata");
        response.stride = 8; wire.kind = static_cast<ipc::PreviewRequestKind>(99);
        check(!ipc::ValidatePreviewResponse(wire, response, layout), "protocol rejects an unknown request kind");
        {
            ThumbnailCache policy;
            ThumbnailCache::Request speculative;
            speculative.key = L"prefetch"; speculative.prefetch = true;
            policy.queue_.push_back(speculative);
            check(policy.NextRequest(0, GetTickCount64()) == policy.queue_.end(),
                "reserved foreground lane never starts speculative work");
            ThumbnailCache::Request visible;
            visible.key = L"visible";
            policy.queue_.push_back(visible);
            policy.consumers_[0].visible_keys.insert(visible.key);
            check(policy.NextRequest(1, GetTickCount64())->key == visible.key,
                "visible work takes priority over the earlier prefetch");
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(fixture.c_str(), GetFileExInfoStandard, &data)) return 2;
        const uint64_t modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
            data.ftLastWriteTime.dwLowDateTime;
        const uint64_t bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        ThumbnailCache cache;
        std::mutex observed_mutex;
        std::set<DWORD> threads;
        std::vector<HANDLE> processes;
        cache.before_response_body_ = [&] {
            {
                std::lock_guard cache_lock(cache.mutex_);
                std::lock_guard observed_lock(observed_mutex);
                if (threads.insert(GetCurrentThreadId()).second) {
                    HANDLE process = nullptr;
                    if (GetThreadId(cache.worker_.native_handle()) == GetCurrentThreadId()) process = cache.child_.hProcess;
                    for (auto& host : cache.hosts_)
                        if (host.worker.joinable() && GetThreadId(host.worker.native_handle()) == GetCurrentThreadId())
                            process = host.child.hProcess;
                    HANDLE copy = nullptr;
                    if (process && DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &copy,
                        SYNCHRONIZE, FALSE, 0)) processes.push_back(copy);
                }
            }
            Sleep(35); // hold headers briefly so every live lane receives real host work
        };
        const auto started = GetTickCount64();
        {
            std::lock_guard lock(cache.mutex_);
            for (unsigned i = 0; i < 24; ++i) {
                ThumbnailCache::Request req;
                req.path = fixture; req.attrs = data.dwFileAttributes;
                req.pixels = 128 + i * 4; req.modified = modified; req.size = bytes;
                req.generation = 1; req.kind = ipc::PreviewRequestKind::Thumbnail;
                req.key = cache.Key(fixture, req.pixels, modified, bytes) + L":thumbnail";
                req.identity = cache.Key(fixture, 0, modified, bytes);
                cache.QueueRequest(std::move(req), ThumbnailCache::Consumer::Main);
            }
        }
        bool complete = false, all_bitmaps = false;
        while (GetTickCount64() - started < 12000) {
            {
                std::lock_guard lock(cache.mutex_);
                if (cache.items_.size() == 24 && cache.pending_.empty()) {
                    complete = true;
                    all_bitmaps = std::all_of(cache.items_.begin(), cache.items_.end(), [](const auto& pair) {
                        return !pair.second.failed && pair.second.kind == ipc::PreviewContentKind::Bitmap &&
                            !pair.second.pixels.empty();
                    });
                    break;
                }
            }
            Sleep(10);
        }
        check(complete && all_bitmaps, "real Thumbnail hosts return all still-image bitmaps");
        {
            std::lock_guard lock(observed_mutex);
            check(threads.size() >= std::min<size_t>(2, cache.lane_count_), "real requests complete on independent host lanes");
            std::printf("[INFO] lanes=%zu completed_threads=%zu elapsed_ms=%llu\n", cache.lane_count_,
                threads.size(), static_cast<unsigned long long>(GetTickCount64() - started));
        }
        // Stop at the response body with a real helper still connected, switch
        // the consumer epoch, and ensure the old result cannot enter memory.
        cache.Reset();
        for (HANDLE process : processes) {
            check(WaitForSingleObject(process, 1000) == WAIT_OBJECT_0, "Reset retires an actual preview child");
            CloseHandle(process);
        }
        HANDLE body = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        cache.before_response_body_ = [&] {
            SetEvent(body);
            const HANDLE events[] = {cache.stop_event_, release};
            WaitForMultipleObjects(2, events, FALSE, 650);
        };
        std::wstring old_key;
        {
            std::lock_guard lock(cache.mutex_);
            ThumbnailCache::Request req;
            req.path = fixture; req.attrs = data.dwFileAttributes; req.pixels = 320;
            req.modified = modified; req.size = bytes; req.generation = 7;
            req.kind = ipc::PreviewRequestKind::Thumbnail;
            req.key = cache.Key(fixture, 320, modified, bytes) + L":thumbnail";
            old_key = req.key; req.identity = cache.Key(fixture, 0, modified, bytes);
            cache.QueueRequest(req, ThumbnailCache::Consumer::Main);
        }
        const bool entered = WaitForSingleObject(body, 5000) == WAIT_OBJECT_0;
        cache.Evict();
        SetEvent(release);
        Sleep(100);
        {
            std::lock_guard lock(cache.mutex_);
            check(entered && !cache.pending_.contains(old_key) && !cache.items_.contains(old_key),
                "directory switch rejects an in-flight real host result from the old epoch");
        }
        ResetEvent(body); ResetEvent(release);
        {
            std::lock_guard lock(cache.mutex_);
            ThumbnailCache::Request req;
            req.path = fixture; req.attrs = data.dwFileAttributes; req.pixels = 384;
            req.modified = modified; req.size = bytes; req.generation = 8;
            req.kind = ipc::PreviewRequestKind::Thumbnail;
            req.key = cache.Key(fixture, 384, modified, bytes) + L":thumbnail";
            req.identity = cache.Key(fixture, 0, modified, bytes);
            cache.QueueRequest(req, ThumbnailCache::Consumer::Main);
        }
        const bool entered_shutdown = WaitForSingleObject(body, 5000) == WAIT_OBJECT_0;
        // Simulate the response gate listening to cancellation just as the
        // pipe reader does, to measure the production shutdown path itself.
        std::vector<HANDLE> shutdown_threads;
        const auto observe_thread = [&](std::thread& thread) {
            HANDLE copy = nullptr;
            if (thread.joinable() && DuplicateHandle(GetCurrentProcess(), thread.native_handle(),
                    GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0)) shutdown_threads.push_back(copy);
        };
        observe_thread(cache.worker_);
        for (auto& host : cache.hosts_) observe_thread(host.worker);
        const auto reset_started = GetTickCount64();
        std::thread observer([&] {
            std::vector<bool> finished(shutdown_threads.size());
            size_t remaining = finished.size();
            while (remaining) {
                for (size_t i = 0; i < finished.size(); ++i)
                    if (!finished[i] && WaitForSingleObject(shutdown_threads[i], 0) == WAIT_OBJECT_0) {
                        finished[i] = true; --remaining;
                        std::printf("[INFO] shutdown lane=%zu worker_exit_ms=%llu\n", i,
                            static_cast<unsigned long long>(GetTickCount64() - reset_started));
                    }
                Sleep(1);
            }
        });
        cache.Reset();
        const auto reset_elapsed = GetTickCount64() - reset_started;
        observer.join();
        for (HANDLE thread : shutdown_threads) CloseHandle(thread);
        std::printf("[INFO] shutdown entered=%d elapsed_ms=%llu queued=%zu pending=%zu\n",
            entered_shutdown, static_cast<unsigned long long>(reset_elapsed), cache.queue_.size(), cache.pending_.size());
        check(entered_shutdown && reset_elapsed < 1000 && cache.queue_.empty() &&
            cache.pending_.empty(), "shutdown cancels real host traffic and clears every lane");
        CloseHandle(body); CloseHandle(release);

        auto& store = ThumbnailDiskCache::Shared();
        ThumbnailStoreKey key{fixture, data.dwFileAttributes, 64, modified, bytes};
        StoredThumbnail thumb;
        thumb.width = 2; thumb.height = 2; thumb.source_width = 1024; thumb.source_height = 1024;
        thumb.pixels = {0,0,128,128, 0,255,0,255, 255,0,0,255, 0,0,0,0};
        ThumbnailFileIdentity identity{};
        check(ReadThumbnailFileIdentity(key, identity), "source identity is captured without decoding");
        store.Save(key, 100, thumb, &identity);
        StoredThumbnail loaded;
        check(store.Load(key, loaded) && loaded.pixels == thumb.pixels && loaded.source_width == 1024,
            "persistent transparent PNG thumbnail round-trips");
        const std::wstring extended = L"\\\\?\\" + fixture;
        ThumbnailStoreKey extended_key{extended, data.dwFileAttributes, 64, modified, bytes};
        store.Save(extended_key, 100, thumb, &identity);
        check(store.Load(extended_key, loaded), "extended local paths use persistent thumbnails");
        ThumbnailStoreKey wrong_time{fixture, data.dwFileAttributes, 64, modified + 1, bytes};
        check(!store.Load(wrong_time, loaded), "changed modification time misses persistent cache");
        ThumbnailStoreKey cloud{fixture, data.dwFileAttributes | FILE_ATTRIBUTE_OFFLINE, 64, modified, bytes};
        check(!store.Load(cloud, loaded), "cloud placeholder never reads a persistent thumbnail");
        // Atomic replacement with identical timestamp and length still has a
        // different file ID and must not inherit the old decoded pixels.
        const std::wstring replacement = root + L"\\replacement.bmp";
        bool replaced = CopyFileW(fixture.c_str(), replacement.c_str(), FALSE) != FALSE;
        HANDLE replacement_handle = CreateFileW(replacement.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (replacement_handle != INVALID_HANDLE_VALUE) {
            replaced &= SetFileTime(replacement_handle, nullptr, nullptr, &data.ftLastWriteTime) != FALSE;
            CloseHandle(replacement_handle);
        } else replaced = false;
        replaced &= MoveFileExW(replacement.c_str(), fixture.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        check(replaced && !store.Load(key, loaded), "same-time same-size file replacement invalidates disk entry");
        const auto previous_bytes = store.BytesOnDisk();
        store.Save(key, 100, thumb, &identity);
        check(store.BytesOnDisk() == previous_bytes && !store.Load(key, loaded),
            "old decode identity cannot save pixels for a replacement file");
        store.Clear();
        check(store.BytesOnDisk() == 0, "isolated test cache cleanup completes");
        std::printf("THUMBNAIL PIPELINE passed=%u failed=%u\n", passed, failed);
        return failed ? 1 : 0;
    }
};
}

int wmain() {
    wchar_t current[32768]{};
    if (!GetCurrentDirectoryW(32768, current)) return 2;
    const std::wstring root = std::wstring(current) + L"\\bench_data\\thumbnail-pipeline-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW((std::wstring(current) + L"\\bench_data").c_str(), nullptr);
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 2;
    SetEnvironmentVariableW(L"PULSE_TEST_THUMB_CACHE", (root + L"\\cache").c_str());
    const std::wstring fixture = root + L"\\fixture.bmp";
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = 1024; info.biHeight = 1024;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = 1024 * 1024 * 4;
    file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::vector<uint8_t> pixels(info.biSizeImage, 180);
    HANDLE out = CreateFileW(fixture.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) return 2;
    DWORD wrote = 0;
    const bool written = WriteFile(out, &file, sizeof(file), &wrote, nullptr) &&
        WriteFile(out, &info, sizeof(info), &wrote, nullptr) &&
        WriteFile(out, pixels.data(), static_cast<DWORD>(pixels.size()), &wrote, nullptr);
    CloseHandle(out);
    if (!written) return 2;
    const int result = pulse::ui::ThumbnailPipelineTestAccess::Run(fixture, root);
    DeleteFileW(fixture.c_str());
    return result;
}
