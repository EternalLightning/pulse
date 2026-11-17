// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <wincodec.h>

namespace {
bool WriteSolidPng(const std::wstring& path, bool blue) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(256, 256))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    std::vector<BYTE> pixels(256 * 256 * 3);
    for (size_t i = 0; i < pixels.size(); i += 3) pixels[i + (blue ? 0 : 2)] = 255;
    return SUCCEEDED(frame->SetPixelFormat(&format)) &&
        SUCCEEDED(frame->WritePixels(256, 256 * 3, static_cast<UINT>(pixels.size()), pixels.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

int RunThumbnailReplacementTests() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t current[32768]{};
    if (FAILED(com) || !GetCurrentDirectoryW(ARRAYSIZE(current), current)) return 2;
    const std::wstring root = std::wstring(current) + L"\\bench_data\\thumbnail-replacement-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(root.c_str(), nullptr)) { CoUninitialize(); return 2; }
    const std::wstring original = root + L"\\a.png", replacement = root + L"\\b.png";
    Check(WriteSolidPng(original, false) && WriteSolidPng(replacement, true), L"create red a.png and blue b.png");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    Check(GetFileAttributesExW(original.c_str(), GetFileExInfoStandard, &data) != FALSE,
        L"capture original image metadata");
    {
        Microsoft::WRL::ComPtr<IShellItem> item;
        Microsoft::WRL::ComPtr<IThumbnailCache> cache;
        Microsoft::WRL::ComPtr<ISharedBitmap> bitmap;
        WTS_CACHEFLAGS flags{};
        Check(SUCCEEDED(SHCreateItemFromParsingName(original.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            SUCCEEDED(CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cache))) &&
            SUCCEEDED(cache->GetThumbnail(item.Get(), 256, WTS_EXTRACT, &bitmap, &flags, nullptr)),
            L"seed original image in Windows thumbnail cache at current icon size");
    }
    HANDLE handle = CreateFileW(replacement.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(handle != INVALID_HANDLE_VALUE && SetFileTime(handle, nullptr, nullptr, &data.ftLastWriteTime),
        L"replacement keeps original timestamp");
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    Check(DeleteFileW(original.c_str()) && MoveFileW(replacement.c_str(), original.c_str()),
        L"delete a.png and rename b.png to a.png");
    Host host;
    Check(host.Start(), L"start replacement thumbnail host");
    for (uint32_t cap : {256u, 128u, 512u, 256u}) {
        Result result;
        const bool ok = host.Request(original, result, MAXDWORD, cap, ipc::PreviewRequestKind::Thumbnail);
        const size_t center = result.pixels.empty() ? 0 :
            static_cast<size_t>(result.response.height / 2) * result.response.stride + result.response.width / 2 * 4;
        const bool blue = center + 3 < result.pixels.size() && result.pixels[center] > 240 &&
            result.pixels[center + 2] < 15 && result.pixels[center + 3] == 255;
        std::wprintf(L"[INFO] cap=%u status=%u kind=%u error=%ls center_bgr=%u,%u,%u\n", cap,
            result.response.status, static_cast<unsigned>(result.response.kind), result.error.c_str(),
            center + 2 < result.pixels.size() ? result.pixels[center] : 0,
            center + 2 < result.pixels.size() ? result.pixels[center + 1] : 0,
            center + 2 < result.pixels.size() ? result.pixels[center + 2] : 0);
        Check(ok && result.response.kind == ipc::PreviewContentKind::Bitmap && blue,
            L"current and alternate icon sizes show replacement pixels");
    }
    host.Stop();
    DeleteFileW(original.c_str()); DeleteFileW(replacement.c_str());
    Check(RemoveDirectoryW(root.c_str()) != FALSE, L"clean isolated replacement images");
    CoUninitialize();
    return failed ? 1 : 0;
}
}
