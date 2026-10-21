#include "../ipc/preview_protocol.h"
#include "../common/current_user_security.h"
#include "../common/path_utils.h"
#include "../common/preview_extensions.h"
#include "../common/crash_reporter.h"
#include "metafile_raster.h"
#include "svg_raster.h"
#include "video_codec.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shellapi.h>
#include <thumbcache.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

using namespace pulse;
using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kRecallOnOpen = 0x00040000; // FILE_ATTRIBUTE_RECALL_ON_OPEN
constexpr DWORD kRecallOnData = 0x00400000; // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
constexpr DWORD kPinned = 0x00080000; // FILE_ATTRIBUTE_PINNED

bool IsOfflinePlaceholder(DWORD attrs) {
    return (attrs & (kRecallOnOpen | kRecallOnData)) && !(attrs & kPinned);
}

#ifndef PULSE_GIF_DECODER_TESTING
std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return extension;
}

// Shell APIs (SHCreateItemFromParsingName, SHGetPropertyStoreFromParsingName)
// reject \\?\ extended paths; the fs layer hands them out for long-path support.
std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

bool IsOneOf(std::wstring_view extension,
             std::initializer_list<std::wstring_view> values) {
    return std::find(values.begin(), values.end(), extension) != values.end();
}

bool IsKnownText(std::wstring_view extension) {
    return pulse::preview::IsTextExtension(extension);
}

bool IsDirectImage(std::wstring_view extension) {
    return pulse::preview::IsImageExtension(extension);
}

bool IsKnownShellPreview(std::wstring_view extension) {
    return IsOneOf(extension, {
        L".pdf", L".doc", L".docx", L".xls",
        L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp", L".mp4",
        L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v", L".dwg",
        L".dxf", L".step", L".stp", L".iges", L".igs"
    });
}

bool ReadPrefix(const std::wstring& path, DWORD limit, std::vector<uint8_t>& bytes,
                uint64_t& file_size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool sized = GetFileSizeEx(file, &size) != 0;
    file_size = sized ? static_cast<uint64_t>(size.QuadPart) : 0;
    bytes.resize(static_cast<size_t>((std::min)(file_size, static_cast<uint64_t>(limit))));
    DWORD read = 0;
    const bool ok = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != 0);
    CloseHandle(file);
    if (!ok) { bytes.clear(); return false; }
    bytes.resize(read);
    return true;
}

bool LooksBinary(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) ||
                             (bytes[0] == 0xFE && bytes[1] == 0xFF))) return false;
    size_t controls = 0;
    for (uint8_t c : bytes) {
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++controls;
    }
    return !bytes.empty() && controls * 20 > bytes.size();
}

bool DecodeText(const std::vector<uint8_t>& bytes, std::wstring& text, bool& truncated) {
    if (bytes.empty()) { text.clear(); return true; }
    size_t offset = 0;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        offset = 2;
        truncated |= (bytes.size() - offset) % 2 != 0;
        text.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
    } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        offset = 2;
        truncated |= (bytes.size() - offset) % 2 != 0;
        text.reserve((bytes.size() - offset) / 2);
        for (size_t i = offset; i + 1 < bytes.size(); i += 2)
            text.push_back(static_cast<wchar_t>((bytes[i] << 8) | bytes[i + 1]));
    } else {
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
            offset = 3;
        const char* raw = reinterpret_cast<const char*>(bytes.data() + offset);
        int raw_size = static_cast<int>(bytes.size() - offset);
        if (raw_size == 0) { text.clear(); return true; }
        int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, raw_size,
                                        nullptr, 0);
        if (chars <= 0 && truncated && raw_size > 0) {
            int start = raw_size - 1;
            while (start > 0 && raw_size - start < 4 &&
                   (static_cast<uint8_t>(raw[start]) & 0xC0) == 0x80) --start;
            const uint8_t lead = static_cast<uint8_t>(raw[start]);
            const int expected = lead >= 0xC2 && lead <= 0xDF ? 2 :
                lead >= 0xE0 && lead <= 0xEF ? 3 : lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
            bool tail = expected > raw_size - start;
            for (int i = start + 1; tail && i < raw_size; ++i)
                tail = (static_cast<uint8_t>(raw[i]) & 0xC0) == 0x80;
            if (tail && raw_size - start > 1) {
                const uint8_t second = static_cast<uint8_t>(raw[start + 1]);
                tail = (lead != 0xE0 || second >= 0xA0) &&
                    (lead != 0xED || second <= 0x9F) &&
                    (lead != 0xF0 || second >= 0x90) && (lead != 0xF4 || second <= 0x8F);
            }
            // Validate the complete prefix before discarding an incomplete UTF-8 tail.
            if (tail && start > 0) {
                const int prefix_chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    raw, start, nullptr, 0);
                if (prefix_chars > 0) { raw_size = start; chars = prefix_chars; }
            } else if (tail && start == 0) { text.clear(); return true; }
        }
        UINT code_page = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (chars <= 0) {
            code_page = CP_ACP;
            flags = 0;
            chars = MultiByteToWideChar(code_page, flags, raw, raw_size, nullptr, 0);
        }
        if (chars <= 0) return false;
        text.resize(chars);
        MultiByteToWideChar(code_page, flags, raw, raw_size, text.data(), chars);
    }
    for (wchar_t& c : text) {
        if (c < 0x20 && c != L'\r' && c != L'\n' && c != L'\t') c = L'\xFFFD';
    }
    constexpr size_t kMaxChars = 12000;
    if (text.size() > kMaxChars) {
        ipc::TruncatePreviewString(text, kMaxChars);
        truncated = true;
    }
    if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) {
        text.pop_back();
        truncated = true;
    }
    return true;
}

std::wstring MakeHex(const std::vector<uint8_t>& bytes) {
    std::wstring out;
    wchar_t line[128]{};
    for (size_t base = 0; base < bytes.size(); base += 16) {
        int pos = swprintf_s(line, L"%08llX  ", static_cast<unsigned long long>(base));
        for (size_t i = 0; i < 16; ++i) {
            if (base + i < bytes.size())
                pos += swprintf_s(line + pos, std::size(line) - pos, L"%02X ", bytes[base + i]);
            else
                pos += swprintf_s(line + pos, std::size(line) - pos, L"   ");
        }
        pos += swprintf_s(line + pos, std::size(line) - pos, L" ");
        for (size_t i = 0; i < 16 && base + i < bytes.size(); ++i) {
            const uint8_t c = bytes[base + i];
            line[pos++] = c >= 0x20 && c < 0x7F ? static_cast<wchar_t>(c) : L'.';
        }
        line[pos++] = L'\n';
        line[pos] = 0;
        out += line;
    }
    return out;
}

bool MakeTextOrHex(const std::wstring& path, DWORD attrs, ipc::PreviewContentKind& kind,
                   std::wstring& text, uint32_t& bytes_read, bool& truncated) {
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) || IsOfflinePlaceholder(attrs)) return false;
    const bool known_text = IsKnownText(ExtensionOf(path));
    std::vector<uint8_t> bytes;
    uint64_t file_size = 0;
    const DWORD initial = known_text ? 32u * 1024u : 256u;
    if (!ReadPrefix(path, initial, bytes, file_size)) return false;
    if (!known_text && LooksBinary(bytes)) {
        kind = ipc::PreviewContentKind::Hex;
        text = MakeHex(bytes);
        bytes_read = static_cast<uint32_t>(bytes.size());
        truncated = file_size > bytes.size();
        return true;
    }
    if (!known_text && file_size > bytes.size()) {
        if (!ReadPrefix(path, 32u * 1024u, bytes, file_size)) return false;
    }
    truncated = file_size > bytes.size();
    if (LooksBinary(bytes) || !DecodeText(bytes, text, truncated)) return false;
    kind = ipc::PreviewContentKind::Text;
    bytes_read = static_cast<uint32_t>(bytes.size());
    return true;
}

struct PreviewPropertyValue { std::wstring label, value; };

void AddProperty(IPropertyStore* store, REFPROPERTYKEY key, const wchar_t* label,
                 std::vector<PreviewPropertyValue>& out) {
    if (!store || out.size() >= ipc::kPreviewMaxPropertyCount) return;
    PROPVARIANT value{};
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt != VT_EMPTY && value.vt != VT_NULL) {
        PWSTR formatted = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(key, value, PDFF_DEFAULT, &formatted)) &&
            formatted && *formatted) {
            std::wstring display = IsEqualPropertyKey(key, PKEY_Video_Compression)
                ? preview::VideoCodecDisplayName(formatted) : std::wstring(formatted);
            if (!display.empty()) out.push_back({ label, display });
        }
        CoTaskMemFree(formatted);
    }
    PropVariantClear(&value);
}

bool ReadUintProperty(IPropertyStore* store, REFPROPERTYKEY key, uint32_t& value) {
    if (!store) return false;
    PROPVARIANT pv{};
    PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_UI4) { value = pv.uintVal; ok = true; }
        else if (pv.vt == VT_I4 && pv.lVal > 0) { value = static_cast<uint32_t>(pv.lVal); ok = true; }
    }
    PropVariantClear(&pv);
    return ok;
}

std::vector<PreviewPropertyValue> ReadProperties(const std::wstring& path) {
    std::vector<PreviewPropertyValue> out;
    const std::wstring shell_path = ShellPath(path);
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(shell_path.c_str(), nullptr, GPS_BESTEFFORT,
                                                 IID_PPV_ARGS(&store)))) return out;
    const std::wstring extension = ExtensionOf(path);
    if (IsOneOf(extension, {L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff", L".webp", L".heic"})) {
        AddProperty(store.Get(), PKEY_Image_Dimensions, L"尺寸", out);
        AddProperty(store.Get(), PKEY_Photo_DateTaken, L"拍摄时间", out);
        AddProperty(store.Get(), PKEY_Photo_CameraModel, L"相机", out);
    } else if (IsOneOf(extension, {L".mp4", L".mkv", L".mov", L".avi", L".webm", L".wmv", L".m4v"})) {
        AddProperty(store.Get(), PKEY_Media_Duration, L"时长", out);
        uint32_t width = 0, height = 0;
        if (out.size() < ipc::kPreviewMaxPropertyCount && ReadUintProperty(store.Get(), PKEY_Video_FrameWidth, width) &&
            ReadUintProperty(store.Get(), PKEY_Video_FrameHeight, height) &&
            width > 0 && height > 0) {
            wchar_t dims[64];
            swprintf_s(dims, L"%u x %u", width, height);
            out.push_back({ L"分辨率", dims });
        }
        AddProperty(store.Get(), PKEY_Video_FrameRate, L"帧率", out);
        AddProperty(store.Get(), PKEY_Video_Compression, L"编码格式", out);
    } else if (IsOneOf(extension, {L".mp3", L".wav", L".flac", L".m4a", L".aac"})) {
        AddProperty(store.Get(), PKEY_Media_Duration, L"时长", out);
        AddProperty(store.Get(), PKEY_Music_Artist, L"艺术家", out);
        AddProperty(store.Get(), PKEY_Music_AlbumTitle, L"专辑", out);
    } else if (IsOneOf(extension, {L".pdf", L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".odt", L".ods", L".odp"})) {
        AddProperty(store.Get(), PKEY_Title, L"标题", out);
        AddProperty(store.Get(), PKEY_Author, L"作者", out);
        AddProperty(store.Get(), PKEY_Document_PageCount, L"页数", out);
    }
    return out;
}

bool WriteString(HANDLE pipe, const std::wstring& value) {
    const uint32_t chars = static_cast<uint32_t>(value.size());
    return ipc::WriteAll(pipe, &chars, sizeof(chars)) &&
        (chars == 0 || ipc::WriteAll(pipe, value.data(), chars * sizeof(wchar_t)));
}
#endif

} // namespace

#ifndef PULSE_GIF_DECODER_TESTING
static bool DecodeImage(const std::wstring& path, DWORD attrs, UINT pixels,
                        std::vector<uint8_t>& out, UINT& width, UINT& height, UINT& stride,
                        UINT& source_width, UINT& source_height) {
    if (IsOfflinePlaceholder(attrs)) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT sourceWidth = 0, sourceHeight = 0;
    if (FAILED(frame->GetSize(&sourceWidth, &sourceHeight)) ||
        sourceWidth == 0 || sourceHeight == 0) return false;
    source_width = sourceWidth;
    source_height = sourceHeight;

    IWICBitmapSource* source = frame.Get();
    const UINT longest = (std::max)(sourceWidth, sourceHeight);
    if (longest > pixels) {
        const double ratio = static_cast<double>(pixels) / longest;
        const UINT scaledWidth = (std::max)(1u, static_cast<UINT>(sourceWidth * ratio + 0.5));
        const UINT scaledHeight = (std::max)(1u, static_cast<UINT>(sourceHeight * ratio + 0.5));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), scaledWidth, scaledHeight,
                                      WICBitmapInterpolationModeFant))) return false;
        source = scaler.Get();
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return false;
    stride = width * 4;
    out.resize(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()),
                                     out.data()))) {
        out.clear();
        return false;
    }
    return true;
}
#endif

static bool MetadataUInt(IWICMetadataQueryReader* reader, const wchar_t* name,
                         uint32_t& value) {
    if (!reader) return false;
    PROPVARIANT pv{}; PropVariantInit(&pv);
    const HRESULT hr = reader->GetMetadataByName(name, &pv);
    bool ok = SUCCEEDED(hr);
    if (ok) {
        if (pv.vt == VT_UI1) value = pv.bVal;
        else if (pv.vt == VT_UI2) value = pv.uiVal;
        else if (pv.vt == VT_UI4) value = pv.ulVal;
        else if (pv.vt == VT_I4 && pv.lVal >= 0) value = static_cast<uint32_t>(pv.lVal);
        else ok = false;
    }
    PropVariantClear(&pv);
    return ok;
}

static void GifFrameMetadata(IWICBitmapFrameDecode* frame, uint32_t& left,
                             uint32_t& top, uint32_t& width, uint32_t& height,
                             uint32_t& disposal, uint32_t& delay_ms) {
    left = top = 0; width = height = 0; disposal = 0; delay_ms = 100;
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader)) || !reader) {
        frame->GetSize(&width, &height); return;
    }
    MetadataUInt(reader.Get(), L"/imgdesc/Left", left);
    MetadataUInt(reader.Get(), L"/imgdesc/Top", top);
    MetadataUInt(reader.Get(), L"/imgdesc/Width", width);
    MetadataUInt(reader.Get(), L"/imgdesc/Height", height);
    MetadataUInt(reader.Get(), L"/grctlext/Disposal", disposal);
    uint32_t delay = 0;
    if (MetadataUInt(reader.Get(), L"/grctlext/Delay", delay) && delay > 0)
        delay_ms = std::clamp(delay * 10u, 20u, 2000u);
    if (!width || !height) frame->GetSize(&width, &height);
}

static void AlphaBlendPbgra(uint8_t* dst, const uint8_t* src) {
    const uint32_t sa = src[3];
    if (sa == 255) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; return; }
    if (sa == 0) return;
    const uint32_t inv = 255u - sa;
    dst[0] = static_cast<uint8_t>(src[0] + (dst[0] * inv + 127u) / 255u);
    dst[1] = static_cast<uint8_t>(src[1] + (dst[1] * inv + 127u) / 255u);
    dst[2] = static_cast<uint8_t>(src[2] + (dst[2] * inv + 127u) / 255u);
    dst[3] = static_cast<uint8_t>(sa + (dst[3] * inv + 127u) / 255u);
}

struct GifDecodeState {
    static constexpr size_t max_canvas_bytes = 32u * 1024u * 1024u;
    static constexpr size_t max_encoded_bytes = 32u * 1024u * 1024u;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICStream> stream;
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION version{};
    std::wstring path;
    UINT cap = 0, canvas_w = 0, canvas_h = 0, count = 0;
    uint32_t loop_count = 0, delay_ms = 0, next_frame = 0;
    uint32_t prev_left = 0, prev_top = 0, prev_w = 0, prev_h = 0, prev_disposal = 0;
    std::vector<uint8_t> encoded, canvas, saved;
#ifdef PULSE_GIF_DECODER_TESTING
    uint64_t composed_frames = 0, opened_decoders = 0;
#endif
    ~GifDecodeState() { Clear(); }
    void Clear() {
        decoder.Reset(); stream.Reset(); factory.Reset();
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        path.clear(); cap = canvas_w = canvas_h = count = next_frame = 0;
        loop_count = delay_ms = prev_disposal = 0;
        std::vector<uint8_t>().swap(encoded);
        std::vector<uint8_t>().swap(canvas);
        std::vector<uint8_t>().swap(saved);
    }
    bool Matches(const std::wstring& requested_path, UINT requested_cap,
                 const BY_HANDLE_FILE_INFORMATION& actual) const {
        return decoder && path == requested_path && cap == requested_cap &&
            version.dwVolumeSerialNumber == actual.dwVolumeSerialNumber &&
            version.nFileIndexHigh == actual.nFileIndexHigh && version.nFileIndexLow == actual.nFileIndexLow &&
            version.nFileSizeHigh == actual.nFileSizeHigh && version.nFileSizeLow == actual.nFileSizeLow &&
            CompareFileTime(&version.ftLastWriteTime, &actual.ftLastWriteTime) == 0 &&
            CompareFileTime(&version.ftCreationTime, &actual.ftCreationTime) == 0;
    }
};

static bool OpenGifState(GifDecodeState& state, const std::wstring& path, UINT pixels) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) { state.Clear(); return false; }
    BY_HANDLE_FILE_INFORMATION version{};
    if (!GetFileInformationByHandle(file, &version)) { CloseHandle(file); state.Clear(); return false; }
    if (state.Matches(path, pixels, version)) { CloseHandle(file); return true; }
    state.Clear();
    state.file = file; state.version = version; state.path = path; state.cap = pixels;
    if (version.nFileSizeHigh || !version.nFileSizeLow ||
        version.nFileSizeLow > GifDecodeState::max_encoded_bytes) return false;
    state.encoded.resize(version.nFileSizeLow);
    DWORD read = 0;
    BY_HANDLE_FILE_INFORMATION after{};
    if (!ReadFile(file, state.encoded.data(), version.nFileSizeLow, &read, nullptr) ||
        read != version.nFileSizeLow || !GetFileInformationByHandle(file, &after) ||
        after.nFileSizeHigh != version.nFileSizeHigh || after.nFileSizeLow != version.nFileSizeLow ||
        CompareFileTime(&after.ftLastWriteTime, &version.ftLastWriteTime) != 0) return false;
    // WIC can retain a file mapping that prevents replacement. Keep only the
    // bounded encoded snapshot so an idle cached decoder never locks the file.
    CloseHandle(state.file); state.file = INVALID_HANDLE_VALUE;
    auto& factory = state.factory;
    auto& decoder = state.decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&state.stream)) ||
        FAILED(state.stream->InitializeFromMemory(state.encoded.data(),
                                                  static_cast<DWORD>(state.encoded.size()))) ||
        FAILED(factory->CreateDecoderFromStream(state.stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder))) return false;
    if (FAILED(decoder->GetFrameCount(&state.count)) || !state.count) return false;
#ifdef PULSE_GIF_DECODER_TESTING
    ++state.opened_decoders;
#endif
    ComPtr<IWICMetadataQueryReader> decoder_reader;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&decoder_reader)) && decoder_reader) {
        PROPVARIANT pv{}; PropVariantInit(&pv);
        if (SUCCEEDED(decoder_reader->GetMetadataByName(L"/appext/Data", &pv)) &&
            ((pv.vt & VT_VECTOR) != 0) && ((pv.vt & VT_TYPEMASK) == VT_UI1) &&
            pv.caub.cElems >= 16) {
            const auto* b = pv.caub.pElems;
            for (ULONG i = 0; i + 4 < pv.caub.cElems; ++i)
                if (b[i] == 'N' && b[i + 1] == 'E' && b[i + 2] == 'T' &&
                    b[i + 3] == 'S' && b[i + 4] == 'C') {
                    for (ULONG j = i; j + 15 < pv.caub.cElems; ++j)
                        if (b[j] == 0x03 && b[j + 1] == 0x01) {
                            state.loop_count = b[j + 2] | (static_cast<uint32_t>(b[j + 3]) << 8); break;
                        }
                    break;
                }
        }
        PropVariantClear(&pv);
    }
    ComPtr<IWICBitmapFrameDecode> first; if (FAILED(decoder->GetFrame(0, &first))) return false;
    UINT canvas_w = 0, canvas_h = 0; first->GetSize(&canvas_w, &canvas_h);
    ComPtr<IWICMetadataQueryReader> first_reader;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&first_reader)) && first_reader) {
        uint32_t v = 0;
        if (MetadataUInt(first_reader.Get(), L"/logscrdesc/Width", v) && v) canvas_w = v;
        if (MetadataUInt(first_reader.Get(), L"/logscrdesc/Height", v) && v) canvas_h = v;
    }
    if (!canvas_w || !canvas_h || canvas_w > 16384 || canvas_h > 16384 ||
        static_cast<uint64_t>(canvas_w) * canvas_h * 4 > GifDecodeState::max_canvas_bytes) return false;
    state.canvas_w = canvas_w; state.canvas_h = canvas_h;
    state.canvas.assign(static_cast<size_t>(canvas_w) * canvas_h * 4, 0);
    return true;
}

static bool DecodeGifFrame(GifDecodeState& state, const std::wstring& path, DWORD attrs, UINT pixels,
                           uint32_t frame_index, std::vector<uint8_t>& out,
                           UINT& width, UINT& height, UINT& stride,
                           uint32_t& frame_count, uint32_t& delay_ms,
                           uint32_t& loop_count, UINT& source_width, UINT& source_height) {
    if (IsOfflinePlaceholder(attrs)) return false;
    if (!OpenGifState(state, path, pixels)) { state.Clear(); return false; }
    const auto decode = [&]() -> bool {
        auto& factory = state.factory; auto& decoder = state.decoder;
        const UINT canvas_w = state.canvas_w, canvas_h = state.canvas_h;
        auto& canvas = state.canvas; auto& saved = state.saved;
        auto& prev_left = state.prev_left; auto& prev_top = state.prev_top;
        auto& prev_w = state.prev_w; auto& prev_h = state.prev_h;
        auto& prev_disposal = state.prev_disposal;
        frame_count = state.count; frame_index = (std::min)(frame_index, state.count - 1);
        loop_count = state.loop_count;
        source_width = canvas_w; source_height = canvas_h;
        // A backward seek (including a playback loop) starts from the transparent
        // canvas. Forward skips still apply every intervening disposal operation.
        if (state.next_frame && frame_index + 1 < state.next_frame) {
            std::fill(canvas.begin(), canvas.end(), uint8_t{0});
            saved.clear(); state.next_frame = 0; prev_disposal = 0;
        }
        for (uint32_t i = state.next_frame; i <= frame_index; ++i) {
            if (i > 0) {
                if (prev_disposal == 2) {
                    const uint32_t x0 = (std::min)(prev_left, canvas_w);
                    const uint32_t x1 = (std::min)(canvas_w, prev_left + prev_w);
                    for (uint32_t y = (std::min)(prev_top, canvas_h);
                         y < (std::min)(canvas_h, prev_top + prev_h); ++y)
                        std::fill(canvas.begin() + (static_cast<size_t>(y) * canvas_w + x0) * 4,
                                  canvas.begin() + (static_cast<size_t>(y) * canvas_w + x1) * 4, uint8_t{0});
                } else if (prev_disposal == 3 && saved.size() == canvas.size()) canvas = saved;
            }
            ComPtr<IWICBitmapFrameDecode> frame; if (FAILED(decoder->GetFrame(i, &frame))) return false;
            uint32_t left, top, fw, fh, disposal, current_delay;
            GifFrameMetadata(frame.Get(), left, top, fw, fh, disposal, current_delay);
            state.delay_ms = current_delay;
            if (disposal == 3) saved = canvas;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                             WICBitmapDitherTypeNone, nullptr, 0.0,
                                             WICBitmapPaletteTypeCustom))) return false;
            UINT rw = 0, rh = 0;
            if (FAILED(converter->GetSize(&rw, &rh)) || !rw || !rh ||
                static_cast<uint64_t>(rw) * rh * 4 > GifDecodeState::max_canvas_bytes) return false;
            const UINT copy_w = (std::min)(fw, rw), copy_h = (std::min)(fh, rh);
            std::vector<uint8_t> pixels_data(static_cast<size_t>(rw) * rh * 4);
            if (FAILED(converter->CopyPixels(nullptr, rw * 4, static_cast<UINT>(pixels_data.size()), pixels_data.data()))) return false;
            for (UINT y = 0; y < copy_h && top + y < canvas_h; ++y)
                for (UINT x = 0; x < copy_w && left + x < canvas_w; ++x)
                    AlphaBlendPbgra(&canvas[(static_cast<size_t>(top + y) * canvas_w + left + x) * 4],
                                    &pixels_data[(static_cast<size_t>(y) * rw + x) * 4]);
            prev_left = left; prev_top = top; prev_w = fw; prev_h = fh; prev_disposal = disposal;
            state.next_frame = i + 1;
#ifdef PULSE_GIF_DECODER_TESTING
            ++state.composed_frames;
#endif
        }
        delay_ms = state.delay_ms;
        width = canvas_w; height = canvas_h; stride = canvas_w * 4;
        const UINT longest = (std::max)(canvas_w, canvas_h);
        if (longest > pixels) {
            const double ratio = static_cast<double>(pixels) / longest;
            width = (std::max)(1u, static_cast<UINT>(canvas_w * ratio + 0.5));
            height = (std::max)(1u, static_cast<UINT>(canvas_h * ratio + 0.5));
            ComPtr<IWICBitmap> bitmap;
            if (FAILED(factory->CreateBitmapFromMemory(canvas_w, canvas_h, GUID_WICPixelFormat32bppPBGRA,
                                                       canvas_w * 4, static_cast<UINT>(canvas.size()), canvas.data(), &bitmap))) return false;
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeFant))) return false;
            out.resize(static_cast<size_t>(width) * height * 4); stride = width * 4;
            return SUCCEEDED(scaler->CopyPixels(nullptr, stride, static_cast<UINT>(out.size()), out.data()));
        }
        out = canvas; return true;
    };
    if (decode()) return true;
    state.Clear(); out.clear(); return false;
}

#ifndef PULSE_GIF_DECODER_TESTING
static bool HbitmapToBgra(HBITMAP bitmap, bool own, std::vector<uint8_t>& out,
                          UINT& width, UINT& height, UINT& stride) {
    ComPtr<IWICImagingFactory> wicFactory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wicFactory)))) {
        if (own) DeleteObject(bitmap);
        return false;
    }
    const WICBitmapAlphaChannelOption options[] = {
        WICBitmapUsePremultipliedAlpha, WICBitmapIgnoreAlpha
    };
    bool ok = false;
    for (const auto option : options) {
        ComPtr<IWICBitmap> source;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory->CreateBitmapFromHBITMAP(bitmap, nullptr, option, &source)))
            continue;
        if (FAILED(wicFactory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
            continue;
        stride = width * 4;
        out.resize(static_cast<size_t>(stride) * height);
        if (SUCCEEDED(converter->CopyPixels(nullptr, stride,
                static_cast<UINT>(out.size()), out.data()))) {
            ok = true;
            break;
        }
        out.clear();
    }
    if (own) DeleteObject(bitmap);
    if (!ok) out.clear();
    return ok;
}

static bool FromThumbnailProvider(IShellItem* item, UINT pixels, HBITMAP& bitmap) {
    ComPtr<IThumbnailProvider> provider;
    if (FAILED(item->BindToHandler(nullptr, BHID_ThumbnailHandler, IID_PPV_ARGS(&provider))))
        return false;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    return SUCCEEDED(provider->GetThumbnail(pixels, &bitmap, &alpha)) && bitmap;
}

static bool FromThumbnailCache(IShellItem* item, UINT pixels, bool cache_only, HBITMAP& bitmap) {
    ComPtr<IThumbnailCache> cache;
    if (FAILED(CoCreateInstance(CLSID_LocalThumbnailCache, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&cache))))
        return false;
    const WTS_FLAGS flags = cache_only
        ? static_cast<WTS_FLAGS>(WTS_INCACHEONLY | WTS_SCALETOREQUESTEDSIZE)
        : static_cast<WTS_FLAGS>(WTS_EXTRACT | WTS_SCALETOREQUESTEDSIZE);
    ComPtr<ISharedBitmap> shared;
    WTS_CACHEFLAGS cacheFlags{};
    if (FAILED(cache->GetThumbnail(item, pixels, flags, &shared, &cacheFlags, nullptr)) || !shared)
        return false;
    HBITMAP shared_bitmap = nullptr;
    if (FAILED(shared->GetSharedBitmap(&shared_bitmap)) || !shared_bitmap) return false;
    bitmap = static_cast<HBITMAP>(CopyImage(shared_bitmap, IMAGE_BITMAP, 0, 0, 0));
    return bitmap != nullptr;
}

static bool MakeShellThumbnail(const std::wstring& path, DWORD attrs, UINT pixels,
                               std::vector<uint8_t>& out, UINT& width, UINT& height,
                               UINT& stride) {
    if (IsOfflinePlaceholder(attrs)) return false;
    const std::wstring shell_path = ShellPath(path);
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(shell_path.c_str(), nullptr, IID_PPV_ARGS(&item))))
        return false;
    const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    HBITMAP bitmap = nullptr;

    if (isDirectory) {
        ComPtr<IShellItemImageFactory> factory;
        if (FAILED(item.As(&factory))) return false;
        const SIZE size{static_cast<LONG>(pixels), static_cast<LONG>(pixels)};
        if (FAILED(factory->GetImage(size,
                SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmap)) || !bitmap)
            return false;
        return HbitmapToBgra(bitmap, true, out, width, height, stride);
    }

    if (!FromThumbnailCache(item.Get(), pixels, true, bitmap) || !bitmap)
        bitmap = nullptr;
    if (!bitmap && !FromThumbnailProvider(item.Get(), pixels, bitmap))
        bitmap = nullptr;
    if (!bitmap && (!FromThumbnailCache(item.Get(), pixels, false, bitmap) || !bitmap))
        bitmap = nullptr;
    if (!bitmap) {
        ComPtr<IShellItemImageFactory> factory;
        if (SUCCEEDED(item.As(&factory))) {
            const SIZE size{static_cast<LONG>(pixels), static_cast<LONG>(pixels)};
            const SIIGBF flags = static_cast<SIIGBF>(
                SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY);
            if (FAILED(factory->GetImage(size, flags, &bitmap))) bitmap = nullptr;
        }
    }
    if (!bitmap) return false;
    return HbitmapToBgra(bitmap, true, out, width, height, stride);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    crash::Initialize({crash::ProcessRole::Preview, false, {}});
    if (__argc < 2) return 2;
    const DWORD owner = wcstoul(__wargv[1], nullptr, 10);
    const std::wstring pipeName = ipc::PreviewPipeName(owner);
    CurrentUserSecurityAttributes pipeSecurity;
    if (!pipeSecurity) return 3;
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64*1024, 64*1024, 0, pipeSecurity.get());
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError()!=ERROR_PIPE_CONNECTED) { CloseHandle(pipe); return 4; }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    GifDecodeState gif_state;
    for (;;) {
        ipc::PreviewRequest req;
        if (!ipc::ReadAll(pipe, &req, sizeof(req)) || req.magic!=ipc::kPreviewMagic ||
            req.path_chars==0 || req.path_chars>32768) break;
        std::wstring path(req.path_chars, L'\0');
        if (!ipc::ReadAll(pipe, path.data(), req.path_chars*sizeof(wchar_t))) break;
        std::vector<uint8_t> pixels;
        std::wstring previewText;
        std::wstring errorText;
        std::vector<PreviewPropertyValue> properties;
        UINT w=0,h=0,stride=0;
        UINT source_w=0, source_h=0;
        ipc::PreviewResponse response{};
        response.request_id=req.request_id;
        response.generation=req.generation;

        bool made = false;
        if (req.kind == ipc::PreviewRequestKind::Properties) {
            if (!IsOfflinePlaceholder(req.attrs)) properties = ReadProperties(path);
            if (properties.size() > ipc::kPreviewMaxPropertyCount)
                properties.resize(ipc::kPreviewMaxPropertyCount);
            for (auto& property : properties) {
                ipc::TruncatePreviewString(property.label, ipc::kPreviewMaxPropertyLabelChars, true);
                ipc::TruncatePreviewString(property.value, ipc::kPreviewMaxPropertyValueChars, true);
            }
            response.property_count = static_cast<uint32_t>(properties.size());
            response.status = 0;
            made = true;
        } else {
            const std::wstring extension = ExtensionOf(path);
            bool truncated = false;
            uint32_t bytesRead = 0;
            const bool isDirectory = (req.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (isDirectory) {
                made = MakeShellThumbnail(path, req.attrs,
                                          ipc::ClampPreviewPixelSize(req.pixel_size, false),
                                          pixels, w, h, stride);
                if (made) response.kind = ipc::PreviewContentKind::Bitmap;
                else errorText = L"provider-failed";
            } else if (IsDirectImage(extension)) {
                uint32_t frame_count = 1, frame_delay = 0, loop_count = 0;
                const bool gif = extension == L".gif";
                const UINT cap = ipc::ClampPreviewPixelSize(req.pixel_size, gif);
                if (gif) {
                    made = DecodeGifFrame(gif_state, path, req.attrs, cap, req.frame_index,
                        pixels, w, h, stride, frame_count, frame_delay, loop_count,
                        source_w, source_h);
                } else {
                    made = DecodeImage(path, req.attrs, cap, pixels, w, h, stride,
                                       source_w, source_h);
                }
                response.frame_count = frame_count;
                response.frame_delay_ms = frame_delay;
                response.loop_count = loop_count;
                if (made) {
                    response.kind = ipc::PreviewContentKind::Bitmap;
                } else if (!IsOfflinePlaceholder(req.attrs)) {
                    // WIC has no decoder for every format in the list on every
                    // machine: WMF/EMF only decode when the metafile codec is
                    // present, and AVIF/HEIC need the store extension. Fall back
                    // to the shell thumbnail - the path Explorer itself uses -
                    // before reporting the preview as unavailable.
                    pixels.clear();
                    w = h = stride = 0;
                    if (MakeShellThumbnail(path, req.attrs,
                            ipc::ClampPreviewPixelSize(req.pixel_size, false),
                            pixels, w, h, stride)) {
                        made = true;
                        response.kind = ipc::PreviewContentKind::Bitmap;
                    } else {
                        errorText = L"image-decode-failed";
                    }
                } else {
                    errorText = L"image-decode-failed";
                }
            } else if (preview::IsVectorExtension(extension)) {
                // WIC has no SVG decoder, so these render through Direct2D into
                // the same premultiplied BGRA pixels the raster path returns.
                if (!IsOfflinePlaceholder(req.attrs)) {
                    made = preview::RasterizeSvgFile(path,
                        ipc::ClampPreviewPixelSize(req.pixel_size, false),
                        pixels, w, h, stride, source_w, source_h, &errorText);
                }
                if (made) {
                    response.kind = ipc::PreviewContentKind::Bitmap;
                } else if (!IsOfflinePlaceholder(req.attrs)) {
                    // No Direct2D SVG support (Windows before 1703), a document
                    // too large to parse, or damaged markup: keep the previous
                    // behaviour and show the file as text instead of an empty
                    // placeholder.
                    errorText.clear();
                    made = MakeTextOrHex(path, req.attrs, response.kind, previewText,
                                         bytesRead, truncated);
                    if (!made) errorText = L"svg-render-failed";
                }
            } else if (preview::IsMetaFileExtension(extension)) {
                // WMF/EMF: WIC decodes them only when its codec is installed,
                // and the shell has no thumbnail provider, so GDI renders them.
                if (!IsOfflinePlaceholder(req.attrs)) {
                    made = preview::RasterizeMetaFile(path,
                        ipc::ClampPreviewPixelSize(req.pixel_size, false),
                        pixels, w, h, stride, source_w, source_h, &errorText);
                }
                if (made) response.kind = ipc::PreviewContentKind::Bitmap;
            } else if (IsKnownShellPreview(extension)) {
                made = MakeShellThumbnail(path, req.attrs,
                                          ipc::ClampPreviewPixelSize(req.pixel_size, false),
                                          pixels, w, h, stride);
                if (made) response.kind = ipc::PreviewContentKind::Bitmap;
                else errorText = L"provider-failed";
            } else {
                made = MakeTextOrHex(path, req.attrs, response.kind, previewText,
                                     bytesRead, truncated);
                if (!made) errorText = L"content-read-failed";
            }
            response.status = made ? 0 : 1;
            if (!made) {
                response.kind = ipc::PreviewContentKind::Unsupported;
                if (IsOfflinePlaceholder(req.attrs)) errorText = L"offline-placeholder";
                else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                    errorText = L"path-unavailable";
            }
            response.text_chars = static_cast<uint32_t>(previewText.size());
            response.error_chars = static_cast<uint32_t>(errorText.size());
            response.bytes_read = bytesRead;
            if (truncated) response.flags |= ipc::kPreviewFlagTruncated;
        }
        response.width=w; response.height=h; response.stride=stride;
        response.source_width = source_w;
        response.source_height = source_h;
        HANDLE mapping=nullptr; void* view=nullptr; std::wstring mappingName;
        if (made && response.kind == ipc::PreviewContentKind::Bitmap) {
            mappingName=L"Local\\PulsePreviewMap-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                        std::to_wstring(req.request_id)+L"-"+std::to_wstring(GetTickCount64());
            mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
                static_cast<DWORD>(pixels.size()),mappingName.c_str());
            if (mapping) view=MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,pixels.size());
            if (!view) { response.status=2; mappingName.clear(); }
            else memcpy(view,pixels.data(),pixels.size());
        }
        response.mapping_chars=static_cast<uint32_t>(mappingName.size());
        bool ok=ipc::WriteAll(pipe,&response,sizeof(response));
        if (ok && !mappingName.empty()) ok=ipc::WriteAll(pipe,mappingName.data(),
            response.mapping_chars*sizeof(wchar_t));
        if (ok && !previewText.empty())
            ok=ipc::WriteAll(pipe,previewText.data(),response.text_chars*sizeof(wchar_t));
        if (ok && !errorText.empty())
            ok=ipc::WriteAll(pipe,errorText.data(),response.error_chars*sizeof(wchar_t));
        for (const auto& property : properties) {
            if (!ok) break;
            ok = WriteString(pipe, property.label) && WriteString(pipe, property.value);
        }
        if (ok && view) { unsigned char ack=0; ok=ipc::ReadAll(pipe,&ack,1); }
        if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping);
        if (!ok) break;
    }
    gif_state.Clear();
    CoUninitialize(); DisconnectNamedPipe(pipe); CloseHandle(pipe); return 0;
}
#endif
