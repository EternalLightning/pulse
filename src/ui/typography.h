// Shared DirectWrite typography policy for Pulse-owned UI surfaces.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

#include <cstdint>
#include <string_view>

namespace pulse::ui { class Compositor; }

namespace pulse::ui::typography {

enum class FontRole {
    Text,
    Display,
    Icon,
    Monospace,
};

struct TextFormatSpec {
    FontRole role = FontRole::Text;
    float size = 14.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
};

const wchar_t* LocaleName() noexcept;
bool HasIconFont(IDWriteFactory2* factory);
const wchar_t* PreferredTextFamily() noexcept;
HRESULT CreateTextFormat(IDWriteFactory2* factory, const TextFormatSpec& spec,
                         IDWriteTextFormat** format);
HRESULT CreateRenderingParams(IDWriteFactory2* factory, HMONITOR monitor,
                              IDWriteRenderingParams2** params);

enum class TextRenderMode : int { Auto = 0, Sharp = 1, Smooth = 2 };
void SetTextRenderMode(TextRenderMode mode) noexcept;
TextRenderMode CurrentTextRenderMode() noexcept;
UINT TextBackendChangedMessage() noexcept;
inline bool UseLumaTextForUi() noexcept { return CurrentTextRenderMode() == TextRenderMode::Auto; }

// Pulse targets use physical pixels at 96 DPI. Sharp layout and drawing must
// share GDI-compatible advances; mixing natural advances with classic hinting
// makes short Latin letter pairs appear unevenly spaced.
DWRITE_MEASURING_MODE MeasuringMode() noexcept;
HRESULT CreateTextLayout(IDWriteFactory* factory, const wchar_t* text, UINT32 length,
                         IDWriteTextFormat* format, float width, float height,
                         IDWriteTextLayout** layout, float pixels_per_dip = 1.0f);
int EditFontPixels(float scale, float dip = 14.0f) noexcept;
HFONT CreateEditFont(float scale, float dip = 14.0f);

// Invalidate language-dependent fallback and measurement caches.
void InvalidateCaches();
std::uint64_t Generation() noexcept;
// Drop only the calling thread's widths before its formats/device are replaced.
void ClearMeasurementCache() noexcept;

// Pulse lays out in physical pixels on a 96-DPI D2D target. Keep vertical
// text boxes stable while preserving the selected mode's horizontal metrics.
D2D1_RECT_F SnapVerticalBounds(const D2D1_RECT_F& bounds) noexcept;

// Dest rects clip ink, not advance. Leave room for glyph overhang beyond
// DWrite's cluster width.
float InkPad(IDWriteTextFormat* format) noexcept;

// DWrite advance plus right overhang. Empty text is 0. Shares the per-thread
// width cache with MeasureLine; replacing a format/fallback requires invalidation.
float MeasureAdvance(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text);

// Auto uses the enabled Luma renderer; explicit modes use DirectWrite.
bool UsesLumaText(const Compositor* compositor, IDWriteTextFormat* format) noexcept;

// max(DWrite, Luma) plus InkPad so a size-to-content box does not shave
// the last glyph. DirectWrite formats share the MeasureAdvance entry.
float MeasureLine(Compositor* compositor, IDWriteTextFormat* format,
                  std::wstring_view text);

// Wrapped block height at `width`. Newlines are paragraph breaks.
float MeasureWrapped(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text, float width);

} // namespace pulse::ui::typography
