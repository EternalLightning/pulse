#include "../ui/shell_icons.h"
#include "../shell_host/shell_query_icons.h"
#include "../common/windows_compat.h"
#include <wincodec.h>
#include <commctrl.h>
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
#include <array>

namespace pulse::ui {
struct ShellIconCacheTestAccess {
    static bool SharedWindows() {
        bool ok = true;
        const auto check = [&](bool passed, const char* label) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
            ok &= passed;
        };
        HWND first = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
            WS_POPUP | WS_VISIBLE, -32000, -32000, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND second = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
            WS_POPUP | WS_VISIBLE, -32000, -32000, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShellIconCache cache;
        cache.AddNotifyWindow(first);
        cache.AddNotifyWindow(second);
        cache.AddNotifyWindow(first);
        check(first && second && cache.notify_->Count() == 2 && Wait(cache),
            "two windows subscribe to one icon worker without replacing each other");
        ValidateRect(first, nullptr);
        ValidateRect(second, nullptr);
        cache.GenericIndex(L"multiwindow.pulse-notify-test", false, FILE_ATTRIBUTE_NORMAL);
        check(Wait(cache) && GetUpdateRect(first, nullptr, FALSE) && GetUpdateRect(second, nullptr, FALSE),
            "an isolated Shell result invalidates both subscribed windows");

        ComPtr<ID3D11Device> d3d;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;
        ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> a, b;
        const bool graphics = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
            SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDevice(dxgi.get(), &device)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &a)) &&
            SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &b));
        check(graphics, "create independent contexts on the same Direct2D device");
        if (graphics) {
            cache.SetDeviceContext(a.get());
            auto* bitmap = cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32);
            const auto* creation_context = cache.dc_;
            cache.SetDeviceContext(b.get());
            check(bitmap && creation_context != a.get() && creation_context != b.get() &&
                cache.BitmapFor(L"", L"", true, FILE_ATTRIBUTE_DIRECTORY, 32) == bitmap,
                "painting a second window retains the icon bitmap and private creation context");
            cache.RemoveNotifyWindow(first);
            DestroyWindow(first);
            first = nullptr;
            a.reset();
            const uint32_t pixel = 0xffffffff;
            ComPtr<ID2D1Bitmap> created;
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            check(SUCCEEDED(cache.dc_->CreateBitmap(D2D1::SizeU(1, 1), &pixel, 4, &properties, &created)),
                "closing the first context leaves shared bitmap creation alive");
            ComPtr<ID2D1Bitmap1> target;
            const auto target_properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            HRESULT draw = b->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, &target_properties, &target);
            if (SUCCEEDED(draw) && bitmap) {
                b->SetTarget(target.get());
                b->BeginDraw();
                b->DrawBitmap(bitmap, D2D1::RectF(0, 0, 32, 32));
                draw = b->EndDraw();
            }
            check(bitmap && SUCCEEDED(draw), "remaining window draws the retained cached icon");
        }
        ValidateRect(second, nullptr);
        cache.notify_->InvalidateAll();
        check(cache.notify_->Count() == (first ? 2u : 1u) && GetUpdateRect(second, nullptr, FALSE),
            "notification subscription survives closing another window");
        cache.RemoveNotifyWindow(first);
        cache.RemoveNotifyWindow(second);
        if (first) DestroyWindow(first);
        if (second) DestroyWindow(second);
        return ok;
    }
    static bool CachePrune() {
        bool ok = true;
        const auto check = [&](bool passed, const char* name) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
            ok = passed && ok;
        };
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
        check(graphics, "cache prune creates Direct2D software device");
        if (!graphics) return false;
        ShellIconCache cache;
        cache.SetDeviceContext(context.get());
        // No Shell requests: exercise the same CPU removal and UI bitmap paths deterministically.
        cache.worker_ = std::make_shared<ShellIconCache::WorkerState>();
        auto& state = *cache.worker_;
        for (int index : {1, 2}) {
            std::vector<ipc::ShellIconPixels> icons;
            for (uint32_t list_id : {0u, 1u}) {
                ipc::ShellIconPixels icon;
                icon.list_id = list_id;
                icon.width = icon.height = 1;
                icon.stride = 4;
                icon.pixels = {255, 255, 255, 255};
                state.pixel_bytes += icon.pixels.size();
                icons.push_back(std::move(icon));
            }
            state.pixels[index] = std::make_shared<const std::vector<ipc::ShellIconPixels>>(std::move(icons));
        }
        auto* live = cache.BitmapForIndex(2, 0);
        check(live && cache.BitmapForIndex(1, 0) && cache.BitmapForIndex(1, 1),
              "cache prune seeds live and stale bitmap variants");
        check(!cache.PruneStaleBitmaps(), "no eviction skips bitmap scan");
        {
            std::lock_guard lock(state.mutex);
            state.RemovePixels(1);
        }
        check(state.pixel_bytes == 8 && state.eviction_generation.load() == 1,
              "CPU removal updates bytes and eviction generation");
        check(cache.PruneStaleBitmaps() && cache.bitmaps_.size() == 1 &&
              cache.bitmaps_.contains(2), "evicted index releases all cached bitmap variants");
        check(cache.BitmapForIndex(2, 0) == live,
              "valid bitmap retains identity and cache hit");
        check(!cache.PruneStaleBitmaps(), "unchanged generation skips repeat scan");
        {
            std::lock_guard lock(state.mutex);
            state.RemovePixels(1);
        }
        check(!cache.PruneStaleBitmaps(), "missing CPU index does not trigger a scan");
        // This synthetic state owns no worker-pool slot.
        cache.worker_.reset();
        return ok;
    }
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
        const std::array<BYTE, 8> alpha_values{0, 1, 8, 223, 238, 239, 254, 255};
        for (const bool premultiplied : {false, true}) {
            std::vector<BYTE> input(alpha_values.size() * 4);
            std::vector<BYTE> expected(input.size());
            for (size_t i = 0; i < alpha_values.size(); ++i) {
                const BYTE alpha = alpha_values[i];
                input[i * 4] = static_cast<BYTE>(premultiplied ? alpha / 2 : 153);
                input[i * 4 + 1] = static_cast<BYTE>(premultiplied ? alpha / 3 : 84);
                input[i * 4 + 2] = static_cast<BYTE>(premultiplied ? alpha / 5 : 17);
                input[i * 4 + 3] = alpha;
                for (size_t channel = 0; channel < 3; ++channel) {
                    const UINT color = input[i * 4 + channel];
                    expected[i * 4 + channel] = premultiplied ? static_cast<BYTE>(color) :
                        static_cast<BYTE>((color * alpha + 127) / 255);
                }
                expected[i * 4 + 3] = alpha;
            }
            const auto& format = premultiplied ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGRA;
            ComPtr<IWICBitmap> source;
            ipc::ShellIconPixels pixels;
            const bool converted = cache.EnsureWic() &&
                SUCCEEDED(cache.wic_->CreateBitmapFromMemory(static_cast<UINT>(alpha_values.size()), 1,
                    format, static_cast<UINT>(input.size()), static_cast<UINT>(input.size()),
                    input.data(), &source)) && shell::ConvertShellBitmapPixels(source.get(), 1, pixels) &&
                pixels.width == alpha_values.size() && pixels.height == 1 && pixels.stride == input.size() &&
                pixels.pixels.size() == expected.size();
            for (size_t i = 0; i < alpha_values.size(); ++i) {
                const bool exact = converted && std::equal(expected.begin() + i * 4,
                    expected.begin() + i * 4 + 4, pixels.pixels.begin() + i * 4);
                std::printf("[%s] WIC %s alpha=%u: %s\n", exact ? "PASS" : "FAIL",
                    premultiplied ? "PBGRA" : "BGRA", static_cast<UINT>(alpha_values[i]),
                    premultiplied ? "preserves exact premultiplied bytes" : "matches exact integer premultiplication");
                ok &= exact;
            }
        }
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
                IImageList* list = nullptr;
                if (queried) SHGetImageList(SHIL_JUMBO, IID_IImageList, reinterpret_cast<void**>(&list));
                HICON icon = nullptr;
                ComPtr<IWICBitmap> original;
                ComPtr<IWICFormatConverter> converted;
                const bool source = list && SUCCEEDED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) &&
                    icon && SUCCEEDED(cache.wic_->CreateBitmapFromHICON(icon, &original)) &&
                    SUCCEEDED(cache.wic_->CreateFormatConverter(&converted)) &&
                    SUCCEEDED(converted->Initialize(original.get(), GUID_WICPixelFormat32bppPBGRA,
                        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut));
                cache.GenericIndex(name, false, FILE_ATTRIBUTE_NORMAL);
                const bool loaded = Wait(cache);
                auto* bitmap = loaded ? cache.BitmapFor(L"", name, false, FILE_ATTRIBUTE_NORMAL, 256) : nullptr;
                if (icon) DestroyIcon(icon);
                if (list) list->Release();
                const auto size = bitmap ? bitmap->GetPixelSize() : D2D1::SizeU();
                ComPtr<ID2D1Bitmap1> staging;
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                bool exported = bitmap && SUCCEEDED(context->CreateBitmap(size, nullptr, 0, &props, &staging)) &&
                    SUCCEEDED(staging->CopyFromBitmap(nullptr, bitmap, nullptr));
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
    static bool NativePixelsMatch(ShellIconCache& cache, const std::wstring& path, int native_index) {
        std::shared_ptr<const std::vector<ipc::ShellIconPixels>> remote;
        {
            std::lock_guard lock(cache.worker_->mutex);
            const auto found = cache.worker_->exact_index.find(path);
            if (found == cache.worker_->exact_index.end()) return false;
            const auto pixels = cache.worker_->pixels.find(found->second);
            if (pixels == cache.worker_->pixels.end()) return false;
            remote = pixels->second;
        }
        bool ok = remote->size() == 4;
        if (remote->size() != 4)
            std::printf("[INFO] pixel comparison path=%ls native_index=%d remote_sizes=%zu\n",
                        path.c_str(), native_index, remote->size());
        for (const auto& received : *remote) {
            IImageList* list = nullptr;
            HICON icon = nullptr;
            ipc::ShellIconPixels expected;
            const bool converted = SUCCEEDED(SHGetImageList(received.list_id, IID_IImageList,
                reinterpret_cast<void**>(&list))) && list &&
                SUCCEEDED(list->GetIcon(native_index, ILD_TRANSPARENT, &icon)) && icon &&
                shell::ConvertShellIconPixels(icon, received.list_id, expected);
            if (icon) DestroyIcon(icon);
            if (list) list->Release();
            const bool matches = converted && ipc::ValidShellIconPixels(received) &&
                received.width == expected.width && received.height == expected.height &&
                received.stride == expected.stride && received.pixels == expected.pixels;
            if (!matches) {
                std::printf("[INFO] pixel comparison path=%ls native_index=%d\n", path.c_str(), native_index);
                size_t differences = 0;
                size_t first = SIZE_MAX;
                const size_t count = std::min(received.pixels.size(), expected.pixels.size());
                for (size_t i = 0; i < count; ++i) {
                    if (received.pixels[i] != expected.pixels[i]) {
                        ++differences;
                        if (first == SIZE_MAX) first = i;
                    }
                }
                std::printf("[INFO] list=%u remote=%ux%u stride=%u bytes=%zu native=%ux%u stride=%u bytes=%zu converted=%d differing_bytes=%zu first=%zu\n",
                    received.list_id, received.width, received.height, received.stride, received.pixels.size(),
                    expected.width, expected.height, expected.stride, expected.pixels.size(), converted,
                    differences, first);
                if (cache.EnsureWic()) {
                    CreateDirectoryW(L"bench_data\\shell_icon_fixture", nullptr);
                    const std::wstring base = L"bench_data\\shell_icon_fixture\\windows-list-" +
                        std::to_wstring(received.list_id);
                    const auto save = [&](const ipc::ShellIconPixels& pixels, const wchar_t* suffix) {
                        if (!ipc::ValidShellIconPixels(pixels)) return;
                        ComPtr<IWICBitmap> image;
                        if (SUCCEEDED(cache.wic_->CreateBitmapFromMemory(pixels.width, pixels.height,
                                GUID_WICPixelFormat32bppPBGRA, pixels.stride,
                                static_cast<UINT>(pixels.pixels.size()),
                                const_cast<BYTE*>(pixels.pixels.data()), &image)))
                            SavePng(cache, image.get(), base + suffix);
                    };
                    save(received, L"-remote.png");
                    save(expected, L"-native.png");
                    std::printf("[INFO] pixel diagnostics saved: %ls-{remote,native}.png\n", base.c_str());
                }
            }
            ok &= matches;
        }
        return ok;
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
                suppressed &= state->exact_index.empty() && state->pixels.empty() && !state->notify;
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
                    fresh.worker_->pixels.contains(fresh.worker_->exact_index.at(L"fresh"));
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
        if (info.hIcon) DestroyIcon(info.hIcon);
        check(resolved && NativePixelsMatch(cache, windows, info.iIcon),
              "cross-process pixels match native Shell icons in all four sizes");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(cache.worker_->exact_index.contains(windows) &&
                  cache.worker_->pixels.contains(cache.worker_->exact_index.at(windows)),
                  "opaque cache id owns CPU icon pixels");
            check(cache.worker_->exact_index.size() == 4096 && !cache.worker_->exact_index.contains(L"seed-0") &&
                  cache.worker_->exact_index.contains(L"seed-1"), "evicts only oldest entry");
        }
        // Fill the independent CPU budget with valid blobs, then make a real
        // isolated query prove that byte pressure evicts paths and their pixels.
        {
            std::lock_guard lock(cache.worker_->mutex);
            cache.worker_->exact_index.clear();
            cache.worker_->last_used.clear();
            cache.worker_->generic_index.clear();
            cache.worker_->pixels.clear();
            cache.worker_->pixel_bytes = 0;
            for (int i = 0; i < 32; ++i) {
                const int id = cache.worker_->next_index++;
                ipc::ShellIconPixels icon{4, 512, 512, 2048, std::vector<uint8_t>(1024 * 1024, 0xff)};
                const auto key = L"pixel-budget-" + std::to_wstring(i);
                cache.worker_->pixels[id] = std::make_shared<const std::vector<ipc::ShellIconPixels>>(
                    std::vector<ipc::ShellIconPixels>{std::move(icon)});
                cache.worker_->pixel_bytes += 1024 * 1024;
                cache.worker_->exact_index[key] = id;
                cache.worker_->last_used[key] = ++cache.worker_->access_clock;
            }
        }
        cache.RequestExact(windows);
        check(Wait(cache), "query completes under CPU pixel budget pressure");
        {
            std::lock_guard lock(cache.worker_->mutex);
            check(cache.worker_->pixel_bytes <= 32 * 1024 * 1024 &&
                  !cache.worker_->exact_index.contains(L"pixel-budget-0") &&
                  cache.worker_->exact_index.contains(windows) &&
                  cache.worker_->pixels.size() == cache.worker_->exact_index.size(),
                  "CPU byte limit evicts oldest path and associated pixels together");
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
                  "device recreation preserves CPU pixels and rebuilds bitmap");
            cache.Reset();
        }
        return ok;
    }
};
}

int main(int argc, char** argv) {
    // The production UI and query host both request per-monitor awareness;
    // native reference image lists must use the same DPI context.
    pulse::compat::EnableDpiAwareness();
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    bool ok = false;
    if (argc == 2 && std::strcmp(argv[1], "--shared-windows") == 0) {
        ok = pulse::ui::ShellIconCacheTestAccess::SharedWindows();
    } else
    if (argc == 2 && std::strcmp(argv[1], "--cache-prune") == 0) {
        ok = pulse::ui::ShellIconCacheTestAccess::CachePrune();
    } else
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
