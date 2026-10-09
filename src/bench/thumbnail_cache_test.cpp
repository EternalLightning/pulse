#include "../ui/thumbnail_cache.h"
#include <cstdio>

namespace pulse::ui {
struct ThumbnailCacheTestAccess {
    static bool RunFailures() {
        ThumbnailCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        auto request = [&] {
            ThumbnailCache::Request req;
            req.key = L"temporary"; req.identity = req.key;
            req.id = cache.next_id_++; req.epoch = cache.epoch_.load();
            ThumbnailCache::Pending pending;
            pending.id = req.id; pending.identity = req.identity;
            pending.interests[0] = {true, false, 0, cache.consumers_[0].epoch};
            pending.interests[1] = {true, false, 0, cache.consumers_[1].epoch};
            cache.pending_[req.key] = pending;
            cache.running_ = true;
            return req;
        };
        auto failure = [] {
            ThumbnailCache::Item result;
            result.failed = true; result.retryable = true;
            result.animation_identity = L"temporary";
            return result;
        };
        auto req = request();
        const ULONGLONG before_retry = GetTickCount64();
        check(cache.StoreResult(req, failure()) && cache.queue_.size() == 1 &&
              cache.queue_.front().retry_due >= before_retry + 2000 &&
              cache.queue_.front().failure_attempts == 1,
              "temporary failure schedules one delayed retry");
        const auto retry_id = cache.queue_.front().id;
        check(!cache.StoreResult(req, failure()) && cache.pending_.at(req.key).id == retry_id &&
              cache.queue_.size() == 1, "stale failure cannot duplicate or erase delayed retry");
        req = cache.queue_.front(); cache.queue_.clear();
        ThumbnailCache::Item success;
        success.kind = ipc::PreviewContentKind::Text; success.text = L"recovered";
        check(cache.StoreResult(req, std::move(success)) &&
              !cache.items_.at(req.key).failed && !cache.pending_.contains(req.key) &&
              cache.queue_.empty(), "retry recovery replaces negative cache with shared success");
        cache.Reset();
        req = request();
        for (uint32_t attempt = 1; attempt <= 3; ++attempt) {
            check(cache.StoreResult(req, failure()) &&
                  cache.items_.at(req.key).failure_attempts == attempt,
                  "temporary failures preserve attempt budget");
            if (attempt < 3) { req = cache.queue_.front(); cache.queue_.clear(); }
        }
        check(cache.queue_.empty() && cache.pending_.empty(),
              "three temporary attempts stop automatic retries");
        cache.SelectDetails(ThumbnailCache::Consumer::Main, L"temporary", 1);
        check(!cache.items_.contains(L"temporary"), "reselection reopens temporary failure budget");
        cache.Reset();
        req = request();
        auto stable = failure(); stable.retryable = false;
        check(cache.StoreResult(req, std::move(stable)) && cache.queue_.empty() &&
              cache.pending_.empty(), "stable unsupported result does not retry");
        cache.Reset();
        req = request(); cache.StoreResult(req, failure());
        cache.Evict(ThumbnailCache::Consumer::Main);
        check(cache.queue_.size() == 1 && cache.pending_.contains(req.key),
              "other consumer retains delayed retry interest");
        cache.Evict(ThumbnailCache::Consumer::QuickPreview);
        check(cache.queue_.empty() && cache.pending_.empty(),
              "last consumer cancellation removes delayed retry");
        return ok;
    }
    static bool Run() {
        ThumbnailCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        auto request = [&](const std::wstring& key) {
            ThumbnailCache::Request req;
            req.key = key;
            req.id = cache.next_id_++;
            req.epoch = cache.epoch_.load();
            ThumbnailCache::Pending pending;
            pending.id = req.id;
            pending.interests[0] = {true, false, 0, cache.consumers_[0].epoch};
            cache.pending_[key] = std::move(pending);
            return req;
        };
        auto insert = [&](const std::wstring& key, size_t cost = 1) {
            ThumbnailCache::Item item;
            item.cost = cost;
            cache.StoreResult(request(key), std::move(item));
        };
        for (int i = 0; i < 128; ++i) insert(std::to_wstring(i));
        cache.Touch(cache.items_.at(L"0"));
        insert(L"128");
        check(cache.items_.contains(L"0") && !cache.items_.contains(L"1"),
              "recently used preview survives capacity eviction");
        insert(L"0", 3);
        check(cache.items_.size() == 128 && cache.lru_.size() == 128 &&
              cache.cache_bytes_ == 130, "replacement has one LRU entry and exact accounting");

        auto old = request(L"old");
        cache.Evict();
        cache.Reset();
        const auto replacement = request(old.key);
        check(!cache.StoreResult(old, {}) && cache.pending_.at(old.key).id == replacement.id &&
              cache.items_.empty(), "old service epoch cannot erase replacement pending request");

        insert(L"current", 7);
        auto stale = request(L"current");
        stale.details = true;
        stale.identity = L"previous selection";
        cache.pending_.at(stale.key).identity = stale.identity;
        cache.pending_.at(stale.key).interests[0].details = true;
        cache.consumers_[0].details_identity = L"new selection";
        check(!cache.StoreResult(stale, {}) && cache.cache_bytes_ == 7 &&
              cache.items_.at(L"current").cost == 7,
              "rapid selection change preserves existing cache accounting");

        cache.Reset();
        insert(L"cold", 8 * 1024 * 1024);
        insert(L"hot", 8 * 1024 * 1024);
        cache.Touch(cache.items_.at(L"cold"));
        insert(L"new", 1);
        check(cache.items_.contains(L"cold") && !cache.items_.contains(L"hot") &&
              cache.cache_bytes_ == 8 * 1024 * 1024 + 1,
              "byte budget evicts least recently used preview");
        cache.Reset();
        for (int i = 0; i < 5; ++i) {
            auto req = request(std::to_wstring(i));
            req.identity = L"animation";
            ThumbnailCache::Item item;
            item.frame_count = 10;
            item.frame_index = static_cast<uint32_t>(i);
            item.animation_identity = req.identity;
            item.cost = 1;
            if (i == 4) cache.Touch(cache.items_.at(L"0"));
            cache.StoreResult(req, std::move(item));
        }
        check(cache.items_.size() == 4 && cache.items_.contains(L"0") &&
              !cache.items_.contains(L"1") && cache.cache_bytes_ == 4,
              "animation keeps four most recently used frames");
        cache.Reset();
        check(cache.items_.empty() && cache.lru_.empty() && cache.cache_bytes_ == 0,
              "reset clears all cache state");

        constexpr auto main = ThumbnailCache::Consumer::Main;
        constexpr auto quick = ThumbnailCache::Consumer::QuickPreview;
        auto enqueue = [&](const std::wstring& key, ThumbnailCache::Consumer consumer,
                           bool details = true, uint64_t generation = 1) {
            ThumbnailCache::Request req;
            req.key = key; req.identity = key; req.generation = generation;
            req.details = details;
            if (details) cache.SelectDetails(consumer, key, generation);
            // Suppress the real worker: deterministic completions exercise production queue/store logic.
            cache.running_ = true;
            cache.QueueRequest(req, consumer);
            req.id = cache.pending_.at(key).id; req.epoch = cache.epoch_.load();
            return req;
        };
        HWND main_window = CreateWindowExW(0, L"STATIC", L"thumbnail-main-test", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND quick_window = CreateWindowExW(0, L"STATIC", L"thumbnail-quick-test", WS_POPUP,
            0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(main_window && quick_window, "create isolated cache notification consumers");
        auto probe_notification_fixture = [&](HWND window, const char* name) {
            if (!window) return;
            ValidateRect(window, nullptr);
            const BOOL hidden_invalidated = InvalidateRect(window, nullptr, FALSE);
            const BOOL hidden_update = GetUpdateRect(window, nullptr, FALSE);
            SetWindowPos(window, nullptr, -32000, -32000, 32, 32,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            ValidateRect(window, nullptr);
            const BOOL visible_invalidated = InvalidateRect(window, nullptr, FALSE);
            const BOOL visible_update = GetUpdateRect(window, nullptr, FALSE);
            std::printf("[NOTIFY FIXTURE] %s hidden invalidate=%d update=%d visible invalidate=%d update=%d\n",
                name, hidden_invalidated, hidden_update, visible_invalidated, visible_update);
            check(visible_invalidated && visible_update,
                  "offscreen visible consumer exposes real Win32 invalidation region");
            ValidateRect(window, nullptr);
        };
        probe_notification_fixture(main_window, "main");
        probe_notification_fixture(quick_window, "quick");
        auto clear_notifications = [&] {
            if (main_window) ValidateRect(main_window, nullptr);
            if (quick_window) ValidateRect(quick_window, nullptr);
        };
        cache.SetNotifyWindow(main_window, main);
        cache.SetNotifyWindow(quick_window, quick);
        clear_notifications();
        auto both = enqueue(L"shared", main);
        auto duplicate = enqueue(L"shared", quick);
        check(both.id == duplicate.id && cache.queue_.size() == 1,
              "same content key deduplicates decode across consumers");
        bool stored = cache.StoreResult(both, {});
        BOOL main_update = main_window ? GetUpdateRect(main_window, nullptr, FALSE) : FALSE;
        BOOL quick_update = quick_window ? GetUpdateRect(quick_window, nullptr, FALSE) : FALSE;
        std::printf("[NOTIFY RESULT] both stored=%d main=%d quick=%d\n", stored, main_update, quick_update);
        check(stored, "shared completion is stored with two live interests");
        check(main_window && quick_window && main_update && quick_update,
              "one completion notifies both live consumers");
        cache.Reset();
        cache.SetNotifyWindow(main_window, main);
        cache.SetNotifyWindow(quick_window, quick);
        clear_notifications();
        both = enqueue(L"close quick", main);
        enqueue(L"close quick", quick);
        cache.SetNotifyWindow(nullptr, quick);
        cache.Evict(quick);
        check(cache.queue_.size() == 1, "quick close retains main queued request");
        stored = cache.StoreResult(both, {});
        main_update = main_window ? GetUpdateRect(main_window, nullptr, FALSE) : FALSE;
        quick_update = quick_window ? GetUpdateRect(quick_window, nullptr, FALSE) : FALSE;
        std::printf("[NOTIFY RESULT] close quick stored=%d main=%d quick=%d\n", stored, main_update, quick_update);
        check(stored, "quick close retains main completion interest");
        check(main_window && quick_window && main_update && !quick_update,
              "closing quick preview preserves main request and notification");
        cache.Reset();
        cache.SetNotifyWindow(main_window, main);
        cache.SetNotifyWindow(quick_window, quick);
        clear_notifications();
        both = enqueue(L"close main", quick);
        enqueue(L"close main", main);
        cache.SetNotifyWindow(nullptr, main);
        cache.Evict(main);
        stored = cache.StoreResult(both, {});
        main_update = main_window ? GetUpdateRect(main_window, nullptr, FALSE) : FALSE;
        quick_update = quick_window ? GetUpdateRect(quick_window, nullptr, FALSE) : FALSE;
        std::printf("[NOTIFY RESULT] close main stored=%d main=%d quick=%d\n", stored, main_update, quick_update);
        check(stored, "main close retains quick completion interest");
        check(main_window && quick_window && !main_update && quick_update,
              "closing main consumer preserves quick preview completion");
        cache.Reset();
        auto quick_old = enqueue(L"quick old", quick);
        auto main_live = enqueue(L"main live", main);
        enqueue(L"quick new", quick);
        check(!cache.StoreResult(quick_old, {}) && cache.StoreResult(main_live, {}),
              "quick selection cancellation does not cancel main details");
        auto main_old = enqueue(L"main old", main);
        auto quick_live = enqueue(L"quick live", quick);
        enqueue(L"main new", main);
        check(!cache.StoreResult(main_old, {}) && cache.StoreResult(quick_live, {}),
              "main selection cancellation does not cancel quick details");
        cache.Reset();
        auto generation_old = enqueue(L"same identity", quick, true, 4);
        auto generation_new = enqueue(L"same identity", quick, true, 5);
        check(generation_old.id != generation_new.id && !cache.StoreResult(generation_old, {}) &&
              cache.pending_.at(generation_new.key).id == generation_new.id &&
              cache.StoreResult(generation_new, {}),
              "same-path generation switch rejects old completion without erasing replacement");
        cache.Reset();
        auto cancelled = enqueue(L"cancelled", main);
        cache.Evict(main);
        auto renewed = enqueue(L"cancelled", quick);
        check(!cache.StoreResult(cancelled, {}) && cache.pending_.at(renewed.key).id == renewed.id &&
              cache.StoreResult(renewed, {}),
              "last-interest cancellation protects replacement request id");
        cache.Reset();
        auto list_request = enqueue(L"list and details", main, false);
        enqueue(L"list and details", main, true);
        enqueue(L"new details", main, true);
        check(cache.StoreResult(list_request, {}),
              "same-window list interest survives details selection cancellation");
        cache.Reset();
        const std::wstring path = LR"(\\?\UNC\server\share\long-name.png)";
        check(cache.Key(path, 256, 1, 2) != cache.Key(path, 512, 1, 2) &&
              cache.Key(path, 256, 1, 2) != cache.Key(path, 256, 3, 2) &&
              cache.Key(path, 256, 1, 2) != cache.Key(path, 256, 1, 3) &&
              cache.Key(path, 256, 1, 2, 0) != cache.Key(path, 256, 1, 2, 1),
              "path size modified pixel bucket and animation frame remain content key inputs");
        if (main_window) DestroyWindow(main_window);
        if (quick_window) DestroyWindow(quick_window);

        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;
        const bool graphics = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        check(graphics, "create preview software rendering device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            ComPtr<ID2D1Bitmap1> target, readback;
            auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            bool ready = SUCCEEDED(context->CreateBitmap(D2D1::SizeU(100, 100),
                nullptr, 0, &props, &target));
            props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
            ready = ready && SUCCEEDED(context->CreateBitmap(D2D1::SizeU(100, 100),
                nullptr, 0, &props, &readback));
            check(ready, "create preview render and readback targets");
            if (ready) {
                context->SetTarget(target.get());
                for (bool portrait : {false, true}) {
                    cache.Reset();
                    ThumbnailCache::Item item;
                    item.w = portrait ? 100u : 200u;
                    item.h = portrait ? 200u : 100u;
                    item.stride = item.w * 4;
                    item.pixels.assign(static_cast<size_t>(item.stride) * item.h, 255);
                    item.cost = item.pixels.size();
                    item.kind = ipc::PreviewContentKind::Bitmap;
                    cache.StoreResult(request(cache.Key(L"image", 200, 0, 0)), std::move(item));
                    for (float offset : {0.0f, 50.0f, 100.0f, 200.0f}) {
                        float x = offset, y = offset;
                        context->BeginDraw();
                        context->Clear(D2D1::ColorF(0, 0.0f));
                        const auto result = cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"image", 0, 200, 0, 0, 0, 1.0f, nullptr, nullptr, nullptr,
                            false, nullptr, &x, &y);
                        bool filled = result == PreviewDrawResult::Bitmap && SUCCEEDED(context->EndDraw());
                        filled = filled && SUCCEEDED(readback->CopyFromBitmap(nullptr, target.get(), nullptr));
                        D2D1_MAPPED_RECT mapped{};
                        if (filled && SUCCEEDED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
                            for (UINT row = 0; row < 100; ++row)
                                for (UINT col = 0; col < 100; ++col)
                                    filled &= mapped.bits[row * mapped.pitch + col * 4 + 3] == 255;
                            readback->Unmap();
                        } else {
                            filled = false;
                        }
                        check(filled, "cover preview remains filled at pan boundaries");
                    }
                }
                cache.Reset();
                ThumbnailCache::Item shared_image;
                shared_image.w = shared_image.h = 100; shared_image.stride = 400;
                shared_image.pixels.assign(40000, 255); shared_image.cost = 40000;
                shared_image.kind = ipc::PreviewContentKind::Bitmap;
                const auto image_key = cache.Key(L"shared-image", 100, 0, 0);
                cache.StoreResult(request(image_key), std::move(shared_image));
                auto draw_shared = [&](ID2D1DeviceContext* dc, ThumbnailCache::Consumer consumer) {
                    dc->BeginDraw();
                    const auto result = cache.Draw(dc, D2D1::RectF(0, 0, 100, 100),
                        L"shared-image", 0, 100, 0, 0, 0, 1.0f, nullptr, nullptr, nullptr,
                        false, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, consumer);
                    return result == PreviewDrawResult::Bitmap && SUCCEEDED(dc->EndDraw());
                };
                check(draw_shared(context.get(), main) && cache.items_.at(image_key).pixels.size() == 40000 &&
                      cache.cache_bytes_ == 80000, "CPU pixels remain available and first GPU copy is budgeted");
                ComPtr<ID3D11Device> d3d_second;
                ComPtr<IDXGIDevice> dxgi_second;
                ComPtr<ID2D1Device> device_second;
                ComPtr<ID2D1DeviceContext> context_second;
                ComPtr<ID2D1Bitmap1> target_second;
                bool second_ready = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                    nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                    &d3d_second, nullptr, nullptr)) &&
                    SUCCEEDED(d3d_second->QueryInterface(IID_PPV_ARGS(&dxgi_second))) &&
                    SUCCEEDED(factory->CreateDevice(dxgi_second.get(), &device_second)) &&
                    SUCCEEDED(device_second->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_second));
                props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
                second_ready = second_ready && SUCCEEDED(context_second->CreateBitmap(D2D1::SizeU(100, 100),
                    nullptr, 0, &props, &target_second));
                check(second_ready, "create separate D2D device for quick preview consumer");
                if (second_ready) {
                    context_second->SetTarget(target_second.get());
                    check(draw_shared(context_second.get(), quick) && cache.cache_bytes_ == 120000 &&
                          cache.items_.at(image_key).bitmaps[0].get() != cache.items_.at(image_key).bitmaps[1].get(),
                          "two devices share CPU decode but never a device-bound bitmap");
                    cache.Evict(main);
                    cache.SetDeviceContext(nullptr, main);
                    check(cache.cache_bytes_ == 80000 && draw_shared(context_second.get(), quick),
                          "detaching main device preserves quick bitmap and shared CPU decode");
                    cache.SetDeviceContext(nullptr, quick);
                    check(cache.cache_bytes_ == 40000 && draw_shared(context_second.get(), quick),
                          "consumer device recovery rebuilds bitmap without another decode");
                    check(draw_shared(context.get(), quick) && cache.cache_bytes_ == 80000,
                          "Draw context change invalidates only that consumer GPU resource");
                    context_second->SetTarget(nullptr);
                }
                cache.Reset();
                ThumbnailCache::Item budget_image;
                budget_image.w = budget_image.h = 100; budget_image.stride = 400;
                budget_image.pixels.assign(40000, 255);
                budget_image.cost = 16ull * 1024ull * 1024ull - 20000;
                budget_image.kind = ipc::PreviewContentKind::Bitmap;
                cache.StoreResult(request(image_key), std::move(budget_image));
                check(draw_shared(context.get(), main) && cache.items_.empty() && cache.cache_bytes_ == 0,
                      "GPU allocation triggers the same total byte budget after drawing");
                cache.Reset();
                cache.running_ = true;
                constexpr int visible_count = 40;
                {
                    ThumbnailCache::FrameScope frame(cache);
                    for (int i = 0; i < visible_count; ++i) {
                        cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"video-" + std::to_wstring(i) + L".mp4", 0, 256, 1, 0, 0);
                    }
                }
                const auto decodes = std::move(cache.queue_);
                cache.queue_.clear();
                for (const auto& decode : decodes) {
                    ThumbnailCache::Item cover;
                    cover.w = cover.h = 256; cover.stride = 1024;
                    cover.pixels.assign(256 * 1024, 255);
                    cover.cost = cover.pixels.size();
                    cover.kind = ipc::PreviewContentKind::Bitmap;
                    cache.StoreResult(decode, std::move(cover));
                }
                bool stable = decodes.size() == visible_count;
                for (int redraw = 0; redraw < 80; ++redraw) {
                    ThumbnailCache::FrameScope frame(cache);
                    context->BeginDraw();
                    for (int i = 0; i < visible_count; ++i) {
                        stable &= cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                            L"video-" + std::to_wstring(i) + L".mp4", 0, 256, 1, 0, 0)
                            == PreviewDrawResult::Bitmap;
                    }
                    stable &= SUCCEEDED(context->EndDraw());
                }
                check(stable && cache.items_.size() == visible_count && cache.pending_.empty() &&
                      cache.queue_.empty() && cache.cache_bytes_ > 16ull * 1024ull * 1024ull,
                      "visible video covers above idle budget remain bitmaps for 80 redraws without redecoding");
                const auto cover_key = cache.Key(L"video-0.mp4", 256, 0, 0);
                {
                    ThumbnailCache::FrameScope frame(cache, quick);
                    context->BeginDraw();
                    const auto result = cache.Draw(context.get(), D2D1::RectF(0, 0, 100, 100),
                        L"video-0.mp4", 0, 256, 1, 0, 0, 1.0f, nullptr, nullptr, nullptr,
                        false, nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr,
                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, quick);
                    const auto draw_result = context->EndDraw();
                    check(result == PreviewDrawResult::Bitmap && SUCCEEDED(draw_result) &&
                          cache.items_.contains(cover_key) && cache.pending_.empty(),
                          "quick preview shares a visible cover without evicting the large-icon working set");
                }
                { ThumbnailCache::FrameScope frame(cache); }
                check(cache.items_.size() <= 32 && cache.cache_bytes_ <= 16ull * 1024ull * 1024ull,
                      "leaving the large-icon view returns unused covers to the idle budget");
                cache.Evict(main);
                check(cache.items_.contains(cover_key) && cache.items_.at(cover_key).bitmaps[1].get(),
                      "main view detachment retains the quick preview's visible cover");
                cache.Evict(quick);
                cache.running_ = false;
                context->SetTarget(nullptr);
            }
            cache.Reset();
        }
        return ok;
    }
};
}

bool RunThumbnailCacheTests() {
    return pulse::ui::ThumbnailCacheTestAccess::Run();
}

bool RunThumbnailFailureTests() {
    return pulse::ui::ThumbnailCacheTestAccess::RunFailures();
}
