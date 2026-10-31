// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "typography.h"
#include "ui_compositor.h"

#include "../common/localization.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pulse::ui::typography {
namespace {

std::mutex g_fallback_mutex;
IDWriteFactory2* g_fallback_factory = nullptr;
IDWriteFontFallback* g_fallback = nullptr;
std::atomic_uint64_t g_generation{1};
std::atomic_int g_text_render_mode{0};

template <typename T>
void Release(T*& value) noexcept {
    if (value) value->Release();
    value = nullptr;
}

bool FamilyExists(IDWriteFactory2* factory, const wchar_t* family) {
    if (!factory || !family) return false;
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE)) || !collection) {
        return false;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    const HRESULT result = collection->FindFamilyName(family, &index, &exists);
    collection->Release();
    return SUCCEEDED(result) && exists;
}

const wchar_t* ResolveFamily(IDWriteFactory2* factory, FontRole role) {
    const wchar_t* const text_fonts[] = {
        L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI Variable Text",
        L"Segoe UI", nullptr};
    const wchar_t* const display_fonts[] = {
        L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI Variable Display",
        L"Segoe UI Variable Text", L"Segoe UI", nullptr};
    const wchar_t* const icons[] = {
        L"Segoe Fluent Icons", L"Segoe MDL2 Assets", L"Segoe UI", nullptr};
    const wchar_t* const mono[] = {
        L"Cascadia Mono", L"Consolas", L"Microsoft YaHei UI", L"Segoe UI", nullptr};

    const wchar_t* const* candidates = nullptr;
    switch (role) {
    case FontRole::Display: candidates = display_fonts; break;
    case FontRole::Icon: candidates = icons; break;
    case FontRole::Monospace: candidates = mono; break;
    default: candidates = text_fonts; break;
    }
    for (size_t i = 0; candidates[i]; ++i) {
        if (FamilyExists(factory, candidates[i])) return candidates[i];
    }
    return L"Segoe UI";
}

bool BuildFallback(IDWriteFactory2* factory, IDWriteFontFallback** fallback) {
    if (!factory || !fallback) return false;
    *fallback = nullptr;
    IDWriteFontFallbackBuilder* builder = nullptr;
    if (FAILED(factory->CreateFontFallbackBuilder(&builder)) || !builder) return false;

    const DWRITE_UNICODE_RANGE cjk[] = {
        {0x2E80, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
        {0xF900, 0xFAFF}, {0xFF00, 0xFFEF}, {0x20000, 0x2FA1F},
    };
    const wchar_t* families[] = {L"Microsoft YaHei UI", L"Microsoft YaHei"};
    // Locale must be empty: a zh-CN mapping is skipped when the UI language
    // (and therefore the text format locale) is en-US, so Chinese in an
    // English shell would miss YaHei.
    builder->AddMapping(cjk, ARRAYSIZE(cjk), families, ARRAYSIZE(families),
                        nullptr, nullptr);

    IDWriteFontFallback* system = nullptr;
    if (SUCCEEDED(factory->GetSystemFontFallback(&system)) && system) {
        builder->AddMappings(system);
        system->Release();
    }
    const HRESULT result = builder->CreateFontFallback(fallback);
    builder->Release();
    return SUCCEEDED(result) && *fallback;
}

void ApplyFallbackInternal(IDWriteFactory2* factory, IDWriteTextFormat* format) {
    if (!factory || !format) return;
    std::lock_guard lock(g_fallback_mutex);
    if (g_fallback_factory != factory || !g_fallback) {
        Release(g_fallback);
        g_fallback_factory = factory;
        BuildFallback(factory, &g_fallback);
    }
    if (!g_fallback) return;
    IDWriteTextFormat1* format1 = nullptr;
    if (SUCCEEDED(format->QueryInterface(IID_PPV_ARGS(&format1))) && format1) {
        format1->SetFontFallback(g_fallback);
        format1->Release();
    }
}

} // namespace

const wchar_t* LocaleName() noexcept {
    return pulse::l10n::LocaleName();
}

bool HasIconFont(IDWriteFactory2* factory) {
    static const bool available = FamilyExists(factory, L"Segoe Fluent Icons") ||
                                   FamilyExists(factory, L"Segoe MDL2 Assets");
    return available;
}

const wchar_t* PreferredTextFamily() noexcept {
    return L"Microsoft YaHei UI";
}

HRESULT CreateTextFormat(IDWriteFactory2* factory, const TextFormatSpec& spec,
                         IDWriteTextFormat** format) {
    if (!format) return E_POINTER;
    *format = nullptr;
    if (!factory || spec.size <= 0.0f) return E_INVALIDARG;
    const wchar_t* family = ResolveFamily(factory, spec.role);
    const wchar_t* locale = spec.role == FontRole::Icon ? L"en-US" : LocaleName();
    const HRESULT result = factory->CreateTextFormat(
        family, nullptr, spec.weight, spec.style, spec.stretch, spec.size, locale, format);
    if (SUCCEEDED(result) && *format && spec.role != FontRole::Icon) {
        ApplyFallbackInternal(factory, *format);
    }
    return result;
}

HRESULT CreateRenderingParams(IDWriteFactory2* factory, HMONITOR monitor,
                              IDWriteRenderingParams2** params) {
    if (!params) return E_POINTER;
    *params = nullptr;
    if (!factory) return E_INVALIDARG;

    IDWriteRenderingParams* base = nullptr;
    HRESULT result = monitor
        ? factory->CreateMonitorRenderingParams(monitor, &base)
        : factory->CreateRenderingParams(&base);
    if (FAILED(result) || !base) return result;

    float grayscale_contrast = base->GetEnhancedContrast();
    IDWriteRenderingParams2* base3 = nullptr;
    if (SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&base3))) && base3) {
        grayscale_contrast = base3->GetGrayscaleEnhancedContrast();
        base3->Release();
    }

    const bool sharp = CurrentTextRenderMode() == TextRenderMode::Sharp;
    const bool grid_fit = CurrentTextRenderMode() != TextRenderMode::Smooth;
    IDWriteFactory3* modern = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&modern)))) {
        IDWriteRenderingParams3* modern_params = nullptr;
        result = modern->CreateCustomRenderingParams(
            base->GetGamma(), base->GetEnhancedContrast(), grayscale_contrast,
            base->GetClearTypeLevel(), DWRITE_PIXEL_GEOMETRY_FLAT,
            sharp ? DWRITE_RENDERING_MODE1_GDI_CLASSIC : DWRITE_RENDERING_MODE1_NATURAL_SYMMETRIC,
            grid_fit ? DWRITE_GRID_FIT_MODE_ENABLED : DWRITE_GRID_FIT_MODE_DISABLED,
            &modern_params);
        modern->Release();
        if (SUCCEEDED(result) && modern_params) {
            *params = modern_params;
            base->Release();
            return result;
        }
    }
    result = factory->CreateCustomRenderingParams(
        base->GetGamma(), base->GetEnhancedContrast(), grayscale_contrast,
        base->GetClearTypeLevel(), DWRITE_PIXEL_GEOMETRY_FLAT,
        sharp ? DWRITE_RENDERING_MODE_GDI_CLASSIC : DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        grid_fit ? DWRITE_GRID_FIT_MODE_ENABLED : DWRITE_GRID_FIT_MODE_DISABLED, params);
    base->Release();
    return result;
}

void SetTextRenderMode(TextRenderMode mode) noexcept {
    const int value = std::clamp(static_cast<int>(mode), 0, 2);
    if (g_text_render_mode.exchange(value, std::memory_order_relaxed) == value) return;
    g_generation.fetch_add(1, std::memory_order_relaxed);
    EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM) -> BOOL {
        SendMessageW(window, TextBackendChangedMessage(), 0, 0);
        EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
            SendMessageW(child, TextBackendChangedMessage(), 0, 0);
            return TRUE;
        }, 0);
        InvalidateRect(window, nullptr, FALSE);
        return TRUE;
    }, 0);
}

TextRenderMode CurrentTextRenderMode() noexcept {
    return static_cast<TextRenderMode>(g_text_render_mode.load(std::memory_order_relaxed));
}

UINT TextBackendChangedMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(L"Pulse.TextBackendChanged");
    return message;
}

DWRITE_MEASURING_MODE MeasuringMode() noexcept {
    return CurrentTextRenderMode() == TextRenderMode::Sharp
        ? DWRITE_MEASURING_MODE_GDI_CLASSIC : DWRITE_MEASURING_MODE_NATURAL;
}

HRESULT CreateTextLayout(IDWriteFactory* factory, const wchar_t* text, UINT32 length,
                         IDWriteTextFormat* format, float width, float height,
                         IDWriteTextLayout** layout, float pixels_per_dip) {
    if (!layout) return E_POINTER;
    *layout = nullptr;
    if (!factory) return E_INVALIDARG;
    if (MeasuringMode() == DWRITE_MEASURING_MODE_GDI_CLASSIC) {
        return factory->CreateGdiCompatibleTextLayout(text, length, format, width, height,
            pixels_per_dip, nullptr, FALSE, layout);
    }
    return factory->CreateTextLayout(text, length, format, width, height, layout);
}

int EditFontPixels(float scale, float dip) noexcept {
    return std::max(1, static_cast<int>(std::lround(dip * std::max(0.25f, scale))));
}

HFONT CreateEditFont(float scale, float dip) {
    const int height = -EditFontPixels(scale, dip);
    HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, PreferredTextFamily());
    if (!font) font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    return font;
}

void InvalidateCaches() {
    {
        std::lock_guard lock(g_fallback_mutex);
        Release(g_fallback);
        g_fallback_factory = nullptr;
    }
    g_generation.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Generation() noexcept {
    return g_generation.load(std::memory_order_relaxed);
}

D2D1_RECT_F SnapVerticalBounds(const D2D1_RECT_F& bounds) noexcept {
    D2D1_RECT_F snapped = bounds;
    snapped.top = std::round(bounds.top);
    snapped.bottom = std::max(snapped.top + 1.0f, std::round(bounds.bottom));
    return snapped;
}

bool UsesLumaText(const Compositor* compositor, IDWriteTextFormat* format) noexcept {
    return compositor && format && compositor->LumaTextEnabled();
}

float InkPad(IDWriteTextFormat* format) noexcept {
    const float size = format ? format->GetFontSize() : 14.0f;
    return std::max(4.0f, size * 0.25f);
}

namespace {

float MeasureAdvanceUncached(IDWriteFactory2* factory, IDWriteTextFormat* format,
                             std::wstring_view text, bool* measured = nullptr) {
    if (measured) *measured = false;
    if (!factory || !format || text.empty()) return 0.0f;
    const float fallback = format->GetFontSize() * static_cast<float>(text.size());
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(CreateTextLayout(factory, text.data(), static_cast<UINT32>(text.size()),
                                         format, 10000.0f, 100.0f, &layout)) || !layout) {
        return fallback;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    DWRITE_OVERHANG_METRICS overhang{};
    const HRESULT metrics_hr = layout->GetMetrics(&metrics);
    layout->GetOverhangMetrics(&overhang);
    layout->Release();
    if (FAILED(metrics_hr)) return fallback;
    if (measured) *measured = true;
    return metrics.widthIncludingTrailingWhitespace + std::max(0.0f, overhang.right) + 1.0f;
}

float MeasureLineUncached(Compositor* compositor, IDWriteTextFormat* format,
                          std::wstring_view text) {
    const float dwrite_w = MeasureAdvance(
        compositor ? compositor->DwriteFactory() : nullptr, format, text);
    float luma = 0.0f;
    if (compositor && compositor->MeasureLumaText(text, format, luma) && luma > 0.0f) {
        return std::max(dwrite_w, luma) + InkPad(format);
    }
    return dwrite_w;
}

struct MeasureKey {
    const void* factory = nullptr;
    const void* compositor = nullptr;
    const void* format = nullptr;
    float size = 0.0f;
    std::uint32_t style = 0; // weight | style << 16 | stretch << 20
    float tab_stop = 0.0f;
    DWRITE_READING_DIRECTION reading = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    DWRITE_FLOW_DIRECTION flow = DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM;
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    DWRITE_LINE_SPACING_METHOD spacing_method = DWRITE_LINE_SPACING_METHOD_DEFAULT;
    float spacing = 0.0f;
    float baseline = 0.0f;
    std::wstring text;
    bool operator==(const MeasureKey&) const = default;
};

struct MeasureKeyHash {
    size_t operator()(const MeasureKey& key) const noexcept {
        size_t h = std::hash<std::wstring>{}(key.text);
        const auto mix = [&h](size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<const void*>{}(key.factory));
        mix(std::hash<const void*>{}(key.compositor));
        mix(std::hash<const void*>{}(key.format));
        mix(std::hash<float>{}(key.size));
        mix(key.style);
        mix(std::hash<float>{}(key.tab_stop));
        mix(static_cast<size_t>(key.reading));
        mix(static_cast<size_t>(key.flow));
        mix(static_cast<size_t>(key.alignment));
        mix(static_cast<size_t>(key.paragraph));
        mix(static_cast<size_t>(key.spacing_method));
        mix(std::hash<float>{}(key.spacing));
        mix(std::hash<float>{}(key.baseline));
        return h;
    }
};

struct MeasureCache {
    std::uint64_t generation = 0;
    std::unordered_map<MeasureKey, float, MeasureKeyHash> widths;
};

constexpr size_t kMeasureCacheLimit = 16384;

MeasureCache& CurrentMeasureCache() {
    thread_local MeasureCache cache;
    const std::uint64_t generation = Generation();
    if (cache.generation != generation) {
        cache.widths.clear();
        cache.generation = generation;
    }
    return cache;
}

MeasureKey MakeMeasureKey(IDWriteFactory2* factory, Compositor* compositor,
                          IDWriteTextFormat* format, std::wstring_view text) {
    MeasureKey key;
    key.factory = factory;
    key.compositor = compositor;
    key.format = format;
    key.size = format->GetFontSize();
    key.style = static_cast<std::uint32_t>(format->GetFontWeight()) & 0xFFFFu;
    key.style |= (static_cast<std::uint32_t>(format->GetFontStyle()) & 0xFu) << 16;
    key.style |= (static_cast<std::uint32_t>(format->GetFontStretch()) & 0xFu) << 20;
    key.tab_stop = format->GetIncrementalTabStop();
    key.reading = format->GetReadingDirection();
    key.flow = format->GetFlowDirection();
    key.alignment = format->GetTextAlignment();
    key.paragraph = format->GetParagraphAlignment();
    format->GetLineSpacing(&key.spacing_method, &key.spacing, &key.baseline);
    key.text.assign(text.data(), text.size());
    return key;
}

void RememberWidth(MeasureCache& cache, MeasureKey key, float width) {
    if (cache.widths.size() >= kMeasureCacheLimit) cache.widths.clear();
    cache.widths.emplace(std::move(key), width);
}

} // namespace

void ClearMeasurementCache() noexcept {
    CurrentMeasureCache().widths.clear();
}

float MeasureAdvance(IDWriteFactory2* factory, IDWriteTextFormat* format,
                      std::wstring_view text) {
    if (!factory || !format || text.empty() || text.size() > 1024)
        return MeasureAdvanceUncached(factory, format, text);
    auto& cache = CurrentMeasureCache();
    auto key = MakeMeasureKey(factory, nullptr, format, text);
    if (const auto found = cache.widths.find(key); found != cache.widths.end())
        return found->second;
    bool measured = false;
    const float width = MeasureAdvanceUncached(factory, format, text, &measured);
    // Failed DWrite measurements must remain retryable, as in the renderer cache.
    if (measured) RememberWidth(cache, std::move(key), width);
    return width;
}

float MeasureLine(Compositor* compositor, IDWriteTextFormat* format,
                   std::wstring_view text) {
    IDWriteFactory2* factory = compositor ? compositor->DwriteFactory() : nullptr;
    if (!UsesLumaText(compositor, format))
        return MeasureAdvance(factory, format, text);
    if (text.empty() || text.size() > 1024)
        return MeasureLineUncached(compositor, format, text);
    auto& cache = CurrentMeasureCache();
    auto key = MakeMeasureKey(factory, compositor, format, text);
    if (const auto found = cache.widths.find(key); found != cache.widths.end())
        return found->second;
    const float width = MeasureLineUncached(compositor, format, text);
    RememberWidth(cache, std::move(key), width);
    return width;
}

float MeasureWrapped(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text, float width) {
    if (!factory || !format || text.empty() || width <= 0.0f) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(CreateTextLayout(factory, text.data(), static_cast<UINT32>(text.size()),
                                         format, width, 4096.0f, &layout)) || !layout) {
        return format->GetFontSize() * 1.4f;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS metrics{};
    const HRESULT hr = layout->GetMetrics(&metrics);
    layout->Release();
    if (FAILED(hr) || metrics.height <= 0.0f) return format->GetFontSize() * 1.4f;
    return std::ceil(metrics.height);
}

} // namespace pulse::ui::typography
