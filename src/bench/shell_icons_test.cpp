#include "../ui/shell_icons.h"
#include <shellapi.h>
#include <commoncontrols.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool SavePng(ShellIconCache& cache, IWICBitmapSource* source, const std::wstring& path) {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        UINT width = 0;
        UINT height = 0;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        return SUCCEEDED(source->GetSize(&width, &height)) &&
            SUCCEEDED(cache.wic_->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(cache.wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(width, height)) &&
            SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WriteSource(source, nullptr)) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    struct IconContent {
        UINT width = 0;
        UINT height = 0;
        size_t pixels = 0;
        UINT Extent() const { return std::max(width, height); }
    };
    static IconContent ContentBounds(IWICBitmapSource* source) {
        UINT width = 0;
        UINT height = 0;
        if (FAILED(source->GetSize(&width, &height))) return {};
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        if (FAILED(source->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
            return {};
        IconContent content;
        UINT left = width;
        UINT top = height;
        UINT right = 0;
        UINT bottom = 0;
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                if (pixels[(static_cast<size_t>(y) * width + x) * 4 + 3] <= 8) continue;
                ++content.pixels;
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x + 1);
                bottom = std::max(bottom, y + 1);
            }
        }
        if (right > left && bottom > top) {
            content.width = right - left;
            content.height = bottom - top;
        }
        return content;
    }
    static HICON MakeIcon(UINT canvas, const RECT& art, bool faint_shadow = false) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(canvas);
        info.bmiHeader.biHeight = -static_cast<LONG>(canvas);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!color || !bits) {
            if (color) DeleteObject(color);
            return nullptr;
        }
        auto* pixels = static_cast<uint32_t*>(bits);
        std::fill_n(pixels, static_cast<size_t>(canvas) * canvas, 0u);
        for (LONG y = art.top; y < art.bottom; ++y)
            for (LONG x = art.left; x < art.right; ++x)
                pixels[static_cast<size_t>(y) * canvas + x] = 0xff336699;
        if (faint_shadow) pixels[0] = 0x04000000;
        std::vector<BYTE> mask_bits(static_cast<size_t>((canvas + 15) / 16) * 2 * canvas, 0);
        HBITMAP mask = CreateBitmap(canvas, canvas, 1, 1, mask_bits.data());
        ICONINFO icon_info{};
        icon_info.fIcon = TRUE;
        icon_info.hbmColor = color;
        icon_info.hbmMask = mask;
        HICON icon = mask ? CreateIconIndirect(&icon_info) : nullptr;
        if (mask) DeleteObject(mask);
        DeleteObject(color);
        return icon;
    }
    static bool PaddedIcons(bool native_archives = false) {
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
        std::printf("[%s] create software device for padded Shell icons\n", graphics ? "PASS" : "FAIL");
        if (!graphics) return false;
        ShellIconCache cache;
        cache.SetDeviceContext(context.get());
        bool ok = true;
        auto check = [&](UINT canvas, const RECT& art, UINT expected, bool shadow, const char* name) {
            HICON icon = MakeIcon(canvas, art, shadow);
            auto bitmap = cache.BitmapFromIcon(icon);
            if (icon) DestroyIcon(icon);
            const auto size = bitmap.get() ? bitmap->GetPixelSize() : D2D1::SizeU();
            const bool result = size.width == expected && size.height == expected;
            std::printf("[%s] %s (source %ux%u)\n", result ? "PASS" : "FAIL", name, size.width, size.height);
            ok &= result;
        };
        check(256, RECT{0, 0, 32, 32}, 32, false,
              "32px legacy archive icon fills large destination rather than 256px padding");
        check(256, RECT{112, 112, 144, 144}, 32, true,
              "centered legacy icon ignores faint remote shadow");
        check(256, RECT{240, 240, 256, 256}, 16, false,
              "edge-aligned small icon retains all visible pixels");
        check(256, RECT{100, 100, 124, 140}, 48, false,
              "rectangular legacy icon preserves aspect ratio in square frame");
        check(256, RECT{32, 32, 224, 224}, 256, false,
              "native jumbo icon preserves its original padding");
        check(32, RECT{4, 4, 28, 28}, 32, false,
              "ordinary small Shell icon preserves its original padding");
        check(256, RECT{}, 256, false,
              "empty icon does not produce invalid crop");
        if (native_archives) {
            CreateDirectoryW(L"bench_data\\shell_icon_fixture", nullptr);
            for (const wchar_t* extension : {L"zip", L"7z"}) {
                const std::wstring name = std::wstring(L"dummy.") + extension;
                SHFILEINFOW info{};
                const bool queried = SHGetFileInfoW(name.c_str(), FILE_ATTRIBUTE_NORMAL, &info,
                    sizeof(info), SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES) != 0;
                IImageList* list = queried ? cache.EnsureImageList(SHIL_JUMBO) : nullptr;
                HICON icon = nullptr;
                ComPtr<IWICBitmap> original;
                ComPtr<IWICFormatConverter> converted;
                const bool source = list && SUCCEEDED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) &&
                    icon && SUCCEEDED(cache.wic_->CreateBitmapFromHICON(icon, &original)) &&
                    SUCCEEDED(cache.wic_->CreateFormatConverter(&converted)) &&
                    SUCCEEDED(converted->Initialize(original.get(), GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
                auto bitmap = source ? cache.BitmapFromIcon(icon) : ComPtr<ID2D1Bitmap>{};
                if (icon) DestroyIcon(icon);
                const auto size = bitmap.get() ? bitmap->GetPixelSize() : D2D1::SizeU();
                ComPtr<ID2D1Bitmap1> staging;
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                bool exported = bitmap.get() && SUCCEEDED(context->CreateBitmap(size, nullptr, 0, &props, &staging)) &&
                    SUCCEEDED(staging->CopyFromBitmap(nullptr, bitmap.get(), nullptr));
                D2D1_MAPPED_RECT mapped{};
                exported = exported && SUCCEEDED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped));
                IconContent after_content;
                if (exported) {
                    ComPtr<IWICBitmap> after;
                    exported = SUCCEEDED(cache.wic_->CreateBitmapFromMemory(size.width, size.height,
                        GUID_WICPixelFormat32bppPBGRA, mapped.pitch, mapped.pitch * size.height,
                        mapped.bits, &after));
                    if (exported) {
                        after_content = ContentBounds(after.get());
                        const std::wstring base = std::wstring(L"bench_data\\shell_icon_fixture\\") + extension;
                        exported = SavePng(cache, converted.get(), base + L"-before.png") &&
                            SavePng(cache, after.get(), base + L"-after.png");
                    }
                    staging->Unmap();
                }
                const IconContent before_content = source ? ContentBounds(converted.get()) : IconContent{};
                const bool result = exported && before_content.pixels > 0 &&
                    after_content.width == before_content.width && after_content.height == before_content.height &&
                    after_content.pixels == before_content.pixels;
                std::printf("[%s] native .%ls: content %ux%u (%zu pixels preserved); normalized canvas %ux%u; visible at 128px: %.1fpx; PNG pair exported\n",
                    result ? "PASS" : "FAIL", extension, before_content.width, before_content.height,
                    before_content.pixels, size.width, size.height,
                    size.width ? after_content.Extent() * 128.0 / size.width : 0.0);
                ok &= result;
            }
        }
        return ok;
    }
    static bool Wait(ShellIconCache& cache) {
        if (!cache.worker_) return false;
        const auto end = GetTickCount64() + 10000;
        while (GetTickCount64() < end) {
            {
                std::lock_guard lock(cache.worker_->mutex);
                if (cache.worker_->queued.empty()) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
#ifdef PULSE_SHELL_ICONS_TESTING
    struct Block {
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<uint32_t> entered{0};
        ~Block() { if (release) CloseHandle(release); }
    };
    static bool Lifecycle(bool permanent) {
        auto block = std::make_shared<Block>();
        ShellIconCache::SetQueryHookForTest([block](const std::wstring&) {
            block->entered.fetch_add(1);
            WaitForSingleObject(block->release, INFINITE);
            return 42;
        });
        const auto limit = ShellIconCache::Resources().limit;
        bool ok = true;
        std::vector<std::shared_ptr<ShellIconCache::WorkerState>> retired_states;
        double max_reset = 0.0;
        for (uint32_t i = 0; i < limit; ++i) {
            ShellIconCache cache;
            cache.RequestExact(L"blocked-" + std::to_wstring(i));
            const auto deadline = GetTickCount64() + 3000;
            while (block->entered.load() < i + 1 && GetTickCount64() < deadline) Sleep(1);
            ok &= block->entered.load() == i + 1;
            const auto old = cache.worker_;
            if (old) retired_states.push_back(old);
            const auto before = std::chrono::steady_clock::now();
            cache.Reset();
            max_reset = (std::max)(max_reset, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - before).count());
            if (old) {
                std::lock_guard lock(old->mutex);
                ok &= old->stop && !old->notify && old->exact_index.empty();
            }
        }
        for (int i = 0; i < 24; ++i) {
            ShellIconCache denied;
            denied.RequestExact(L"denied");
            ok &= !denied.worker_;
        }
        const auto full = ShellIconCache::Resources();
        const bool cap = full.live == limit && full.retired == limit && block->entered.load() == limit;
        std::printf("[%s] global Shell cap includes destroyed cache workers (live=%u retired=%u cap=%u)\n",
                    cap ? "PASS" : "FAIL", full.live, full.retired, full.limit);
        std::printf("[%s] Reset does not wait for Shell (max %.2f ms)\n",
                    max_reset < 100.0 ? "PASS" : "FAIL", max_reset);
        ok &= cap && max_reset < 100.0;
        if (permanent) {
            std::printf("[INFO] permanent injected calls retain %u workers until process exit; not zero leakage\n", full.live);
        } else {
            SetEvent(block->release);
            const auto deadline = GetTickCount64() + 5000;
            while (ShellIconCache::Resources().live && GetTickCount64() < deadline) Sleep(1);
            const bool reaped = ShellIconCache::Resources().live == 0;
            std::printf("[%s] late returning Shell calls release their slots\n", reaped ? "PASS" : "FAIL");
            ok &= reaped;
            bool suppressed = true;
            for (const auto& state : retired_states) {
                std::lock_guard lock(state->mutex);
                suppressed &= state->exact_index.empty() && !state->notify;
            }
            std::printf("[%s] late results cannot populate or notify retired cache state\n", suppressed ? "PASS" : "FAIL");
            ok &= suppressed;
            retired_states.clear();
            ShellIconCache::SetQueryHookForTest([](const std::wstring&) { return 7; });
            ShellIconCache fresh;
            fresh.RequestExact(L"fresh");
            const bool done = Wait(fresh);
            bool resolved = false;
            if (fresh.worker_) {
                std::lock_guard lock(fresh.worker_->mutex);
                resolved = fresh.worker_->exact_index.contains(L"fresh") &&
                    fresh.worker_->exact_index.at(L"fresh") == 7;
            }
            std::printf("[%s] fresh cache resolves after retirement completes\n", done && resolved ? "PASS" : "FAIL");
            ok &= done && resolved;
        }
        ShellIconCache::SetQueryHookForTest({});
        return ok;
    }
#endif
    static bool Run() {
        ShellIconCache cache;
        bool ok = true;
        auto check = [&](bool result, const char* name) {
            std::printf("[%s] %s\n", result ? "PASS" : "FAIL", name);
            ok &= result;
        };
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        cache.GenericIndex(L"test.txt", false, FILE_ATTRIBUTE_NORMAL);
        check(cache.worker_ != nullptr, "admit native icon worker");
        if (!cache.worker_) return false;
        check(Wait(cache), "native type queries complete");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(cache.worker_->generic_index.contains(L"<dir>") && cache.worker_->generic_index.contains(L".txt"),
                  "Windows folder and associated file icons resolve");
        }
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        check(cache.NeedsExactIcon(L"Windows", true, windows), "folders use actual Shell paths");
        // Seed a full cache to exercise eviction without thousands of Shell calls.
        {
            std::lock_guard lock(cache.worker_->mutex);
            for (int i = 0; i < 4096; ++i) {
                const auto key = L"seed-" + std::to_wstring(i);
                cache.worker_->exact_index[key] = 0;
                cache.worker_->last_used[key] = ++cache.worker_->access_clock;
            }
        }
        cache.RequestExact(windows);
        check(Wait(cache), "exact folder query completes");
        SHFILEINFOW info{};
        const bool resolved = SHGetFileInfoW(windows, 0, &info, sizeof(info),
                                            SHGFI_SYSICONINDEX | SHGFI_SMALLICON) != 0;
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(resolved && cache.worker_->exact_index.contains(windows) &&
                  cache.worker_->exact_index.at(windows) == info.iIcon, "index matches native Shell query");
            check(cache.worker_->exact_index.size() == 4096 && !cache.worker_->exact_index.contains(L"seed-0") &&
                  cache.worker_->exact_index.contains(L"seed-1"), "evicts only oldest entry");
        }
        const std::wstring missing = std::wstring(windows) + L"\\pulse-nonexistent-icon-test\\missing.exe";
        cache.RequestExact(missing);
        check(Wait(cache), "missing path query completes");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(!cache.worker_->exact_index.contains(missing) && cache.worker_->retry_after.contains(missing),
                  "failure is retryable rather than permanently cached");
            cache.worker_->retry_after[windows] = 0;
            cache.worker_->exact_index.erase(windows);
            cache.worker_->last_used.erase(windows);
        }
        cache.RequestExact(windows);
        check(Wait(cache), "expired failure retries");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(cache.worker_->exact_index.contains(windows) && !cache.worker_->retry_after.contains(windows),
                  "successful retry clears failure state");
        }
        cache.Reset();
        cache.GenericIndex(L"", true, FILE_ATTRIBUTE_DIRECTORY);
        check(Wait(cache), "worker restarts after reset");
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
        check(graphics, "create Direct2D software device");
        if (graphics) {
            cache.SetDeviceContext(context.get());
            for (const float size : {16.0f, 24.0f, 32.0f, 48.0f, 96.0f, 256.0f}) {
                auto* bitmap = cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, size);
                check(bitmap && bitmap->GetPixelSize().width >= 16,
                      "native folder converts to Direct2D bitmap at requested scale");
            }
            cache.SetDeviceContext(nullptr);
            cache.SetDeviceContext(context.get());
            check(cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) != nullptr,
                  "device recreation preserves native indices and rebuilds bitmap");
            cache.Reset();
        }
        return ok;
    }
};
}

int main(int argc, char** argv) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    if (argc == 2 && (std::strcmp(argv[1], "--padded-icons") == 0 ||
                     std::strcmp(argv[1], "--archive-icons") == 0)) {
        ok = pulse::ui::ShellIconCacheTestAccess::PaddedIcons(std::strcmp(argv[1], "--archive-icons") == 0);
    } else
#ifdef PULSE_SHELL_ICONS_TESTING
    if (argc == 2 && (std::strcmp(argv[1], "--lifecycle") == 0 ||
                      std::strcmp(argv[1], "--permanent") == 0)) {
        ok = pulse::ui::ShellIconCacheTestAccess::Lifecycle(std::strcmp(argv[1], "--permanent") == 0);
    } else
#else
    (void)argc;
    (void)argv;
#endif
    {
        ok = pulse::ui::ShellIconCacheTestAccess::Run();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? 0 : 1;
}
