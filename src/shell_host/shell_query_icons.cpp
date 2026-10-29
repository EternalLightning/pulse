#include "shell_query_icons.h"
#include "../fs/cloud_state.h"
#include "../common/path_utils.h"
#include "../common/shell_id.h"
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>

namespace pulse::shell {
namespace {
using Microsoft::WRL::ComPtr;

struct Icon {
    HICON value = nullptr;
    ~Icon() { if (value) DestroyIcon(value); }
};
struct Pidl {
    PIDLIST_ABSOLUTE value = nullptr;
    ~Pidl() { CoTaskMemFree(value); }
};

void NormalizeJumbo(ipc::ShellIconPixels& icon) {
    if (icon.width <= 64 || icon.height <= 64) return;
    uint32_t left = icon.width, top = icon.height, right = 0, bottom = 0;
    for (uint32_t y = 0; y < icon.height; ++y) {
        for (uint32_t x = 0; x < icon.width; ++x) {
            if (icon.pixels[static_cast<size_t>(y) * icon.stride + x * 4 + 3] <= 8) continue;
            left = std::min(left, x); top = std::min(top, y);
            right = std::max(right, x + 1); bottom = std::max(bottom, y + 1);
        }
    }
    if (right <= left || bottom <= top || right - left > icon.width / 2 ||
        bottom - top > icon.height / 2) return;
    const uint32_t extent = std::max(right - left, bottom - top);
    uint32_t frame = extent;
    for (const uint32_t size : {16u, 32u, 48u, 64u, 96u, 128u, 256u, 512u}) {
        if (size >= extent) { frame = size; break; }
    }
    frame = std::min(frame, std::min(icon.width, icon.height));
    const int x = std::clamp((static_cast<int>(left + right) - static_cast<int>(frame)) / 2,
        0, static_cast<int>(icon.width - frame));
    const int y = std::clamp((static_cast<int>(top + bottom) - static_cast<int>(frame)) / 2,
        0, static_cast<int>(icon.height - frame));
    std::vector<uint8_t> cropped(static_cast<size_t>(frame) * frame * 4);
    for (uint32_t row = 0; row < frame; ++row) {
        memcpy(cropped.data() + static_cast<size_t>(row) * frame * 4,
            icon.pixels.data() + (static_cast<size_t>(y) + row) * icon.stride + x * 4, frame * 4);
    }
    icon.width = icon.height = frame;
    icon.stride = frame * 4;
    icon.pixels = std::move(cropped);
}
}

bool ConvertShellBitmapPixels(IWICBitmapSource* source, uint32_t list_id, ipc::ShellIconPixels& pixels) {
    if (!source) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICFormatConverter> converter;
    ipc::ShellIconPixels converted;
    converted.list_id = list_id;
    WICPixelFormatGUID format{};
    if (FAILED(source->GetPixelFormat(&format))) return false;
    const bool premultiplied = format == GUID_WICPixelFormat32bppPBGRA;
    IWICBitmapSource* reader = source;
    if (!premultiplied && format != GUID_WICPixelFormat32bppBGRA) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))) || FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) return false;
        reader = converter.Get();
    }
    if (FAILED(reader->GetSize(&converted.width, &converted.height)) ||
        !converted.width || !converted.height || converted.width > ipc::kShellQueryPixelLimit ||
        converted.height > ipc::kShellQueryPixelLimit) return false;
    converted.stride = converted.width * 4;
    converted.pixels.resize(static_cast<size_t>(converted.stride) * converted.height);
    if (FAILED(reader->CopyPixels(nullptr, converted.stride,
            static_cast<UINT>(converted.pixels.size()), converted.pixels.data()))) return false;
    // Make alpha rounding explicit so all producers use identical byte values.
    if (!premultiplied) {
        for (size_t offset = 0; offset < converted.pixels.size(); offset += 4) {
            const uint32_t alpha = converted.pixels[offset + 3];
            for (size_t channel = 0; channel < 3; ++channel)
                converted.pixels[offset + channel] = static_cast<uint8_t>(
                    (converted.pixels[offset + channel] * alpha + 127u) / 255u);
        }
    }
    NormalizeJumbo(converted);
    if (!ipc::ValidShellIconPixels(converted)) return false;
    pixels = std::move(converted);
    return true;
}

bool ConvertShellIconPixels(HICON icon, uint32_t list_id, ipc::ShellIconPixels& pixels) {
    if (!icon) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateBitmapFromHICON(icon, &bitmap)) &&
        ConvertShellBitmapPixels(bitmap.Get(), list_id, pixels);
}

void QueryShellIcons(const ipc::ShellQueryRequest& request, ipc::ShellQueryResult& result) {
    auto path = pulse::path::StripExtendedPathPrefix(request.path);
    DWORD attributes = request.attributes;
    bool use_attributes = request.use_attributes;
    if (!use_attributes && !path.starts_with(L"::") && !path.starts_with(L"shell:")) {
        const DWORD current = GetFileAttributesW(request.path.c_str());
        if (current != INVALID_FILE_ATTRIBUTES && fs::IsCloudPlaceholder(current)) {
            // Icon handlers can read content (notably .lnk and .exe). Never
            // invoke them on placeholders, even if the queued attrs were local.
            attributes = current & FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            const size_t dot = path.find_last_of(L'.');
            const size_t slash = path.find_last_of(L"\\/");
            path = L"dummy" + (dot != std::wstring::npos &&
                (slash == std::wstring::npos || dot > slash) ? path.substr(dot) : L"");
            use_attributes = true;
        }
    }
    SHFILEINFOW info{};
    UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
    Pidl pidl;
    LPCWSTR query = path.c_str();
    if (use_attributes) flags |= SHGFI_USEFILEATTRIBUTES;
    else if (path.starts_with(L"::") || path.starts_with(L"shell:")) {
        result.error = shell_id::ParseId(path, &pidl.value);
        if (FAILED(result.error)) return;
        query = reinterpret_cast<LPCWSTR>(pidl.value);
        flags |= SHGFI_PIDL;
    }
    if (!SHGetFileInfoW(query, attributes, &info, sizeof(info), flags)) {
        result.error = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    for (const uint32_t id : {1u, 0u, 2u, 4u}) {
        ComPtr<IImageList> list;
        if (FAILED(SHGetImageList(static_cast<int>(id), IID_PPV_ARGS(&list)))) continue;
        Icon icon;
        if (FAILED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon.value))) continue;
        ipc::ShellIconPixels pixels;
        if (ConvertShellIconPixels(icon.value, id, pixels)) result.icons.push_back(std::move(pixels));
    }
    result.error = result.icons.empty() ? E_FAIL : S_OK;
}

} // namespace pulse::shell
