#include "lumatext_renderer.h"
#include "optional_lumatext.h"
#include "typography.h"
#include "edit_word_range.h"
#include "edit_motion.h"
#include "animation_clock.h"
#include "FluentTokens.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <wrl/client.h>

#if defined(PULSE_HAS_LUMATEXT)
#include <lumatext/lumatext.hpp>
#endif

#pragma comment(lib, "imm32.lib")

namespace pulse::ui {
#ifdef PULSE_TEST_EDIT_VIEWPORT
namespace { std::unordered_map<HWND, float> observed_edit_scroll; }
float EditScrollForTest(HWND hwnd) {
    const auto found = observed_edit_scroll.find(hwnd);
    return found == observed_edit_scroll.end() ? -1.0f : found->second;
}
#endif
namespace {

#if defined(PULSE_HAS_LUMATEXT)
uint64_t EditPresentationTime(HWND hwnd) {
#ifdef PULSE_TEST_EDIT_VIEWPORT
    if (const auto* time = reinterpret_cast<const uint64_t*>(GetPropW(hwnd, L"Pulse.Test.EditTime")))
        return *time;
#else
    (void)hwnd;
#endif
    return AnimationTimeMs();
}

bool IsEditCaretActivity(UINT message) {
    switch (message) {
    case WM_SETFOCUS: case WM_SETTEXT: case EM_SETSEL: case EM_REPLACESEL:
    case WM_KEYDOWN: case WM_CHAR: case WM_CUT: case WM_PASTE: case WM_CLEAR:
    case WM_UNDO: case EM_UNDO: case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION:
        return true;
    default:
        return false;
    }
}
#endif

bool EnvironmentEnabled() noexcept {
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_LUMATEXT", value, ARRAYSIZE(value));
    if (length == 0 || length >= ARRAYSIZE(value)) return true;
    return _wcsicmp(value, L"0") != 0 && _wcsicmp(value, L"off") != 0 &&
           _wcsicmp(value, L"false") != 0;
}

std::wstring FontPath(const wchar_t* file_name) {
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
    if (length == 0 || length >= ARRAYSIZE(windows)) return {};
    return (std::filesystem::path(windows) / L"Fonts" / file_name).wstring();
}

// A font the user installed (per-user first, then for all users). Pulse
// never ships these faces; it only uses them when they are already present.
std::wstring InstalledFontPath(const wchar_t* file_name) {
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, ARRAYSIZE(local));
    if (length != 0 && length < ARRAYSIZE(local)) {
        std::error_code error;
        const auto user = std::filesystem::path(local) / L"Microsoft" / L"Windows" / L"Fonts" / file_name;
        if (std::filesystem::exists(user, error)) return user.wstring();
    }
    return FontPath(file_name);
}

std::wstring FontFamily(IDWriteTextFormat* format) {
    if (!format) return {};
    const UINT32 length = format->GetFontFamilyNameLength();
    if (length == 0 || length > 256) return {};
    std::wstring result(length + 1, L'\0');
    if (FAILED(format->GetFontFamilyName(result.data(), length + 1))) return {};
    result.resize(length);
    return result;
}

bool Contains(const std::wstring& value, const wchar_t* fragment) {
    return value.find(fragment) != std::wstring::npos;
}

} // namespace

struct LumaTextRenderer::Impl {
    LumaTextStats stats;

#if defined(PULSE_HAS_LUMATEXT)
    struct LayoutKey {
        std::wstring text;
        std::uint32_t family = 0;
        std::uint16_t weight = 400;
        std::int32_t size_64 = 0;
        std::int32_t width_64 = 0;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        lt_text_ellipsis ellipsis = LT_TEXT_ELLIPSIS_NONE;

        bool operator==(const LayoutKey&) const = default;
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& key) const noexcept {
            size_t value = std::hash<std::wstring>{}(key.text);
            const auto combine = [&value](size_t next) {
                value ^= next + 0x9e3779b9u + (value << 6) + (value >> 2);
            };
            combine(std::hash<std::uint32_t>{}(key.family));
            combine(std::hash<std::uint16_t>{}(key.weight));
            combine(std::hash<std::int32_t>{}(key.size_64));
            combine(std::hash<std::int32_t>{}(key.width_64));
            combine(std::hash<int>{}(static_cast<int>(key.alignment)));
            combine(std::hash<int>{}(static_cast<int>(key.ellipsis)));
            return value;
        }
    };

    struct SurfaceKey {
        const lt_text_layout* layout = nullptr;
        // Keep the text identity alongside the native layout pointer. Layout
        // objects are LRU-evicted and their addresses can be reused; a stale
        // command list must never become a hit for a different string.
        std::size_t text_hash = 0;
        // The clip width is part of the recorded command list. Without it,
        // resizing a column can reuse a surface recorded at the old width.
        std::int32_t width_64 = 0;
        std::int32_t height_64 = 0;
        // LumaText's glyph cache has 1/8-pixel phase precision. Keeping the
        // surface key at 1/64 creates a new command list for every smooth
        // scroll tick even though the rasterized glyphs are identical.
        std::uint8_t x_phase_8 = 0;
        std::uint32_t foreground = 0;
        std::uint32_t background = 0;
        bool dark = false;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;

        bool operator==(const SurfaceKey&) const = default;
    };

    struct SurfaceKeyHash {
        size_t operator()(const SurfaceKey& key) const noexcept {
            size_t value = std::hash<const void*>{}(key.layout);
            const auto combine = [&value](size_t next) {
                value ^= next + 0x9e3779b9u + (value << 6) + (value >> 2);
            };
            combine(std::hash<std::size_t>{}(key.text_hash));
            combine(std::hash<std::int32_t>{}(key.width_64));
            combine(std::hash<std::int32_t>{}(key.height_64));
            combine(std::hash<std::uint32_t>{}(
                static_cast<std::uint32_t>(key.x_phase_8) << 24 |
                (key.dark ? 1u : 0u)));
            combine(std::hash<std::uint32_t>{}(key.foreground));
            combine(std::hash<std::uint32_t>{}(key.background));
            combine(std::hash<int>{}(static_cast<int>(key.alignment)));
            combine(std::hash<int>{}(static_cast<int>(key.paragraph)));
            return value;
        }
    };

    struct SurfaceEntry {
        Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
        std::uint64_t estimated_bytes = 0;
        std::list<SurfaceKey>::iterator lru;
    };

    struct LayoutSlot {
        LumaText::TextLayout layout;
        std::list<LayoutKey>::iterator lru;
    };

    LumaText::Context context;
    LumaText::Renderer renderer;
    LumaText::Renderer layout_renderer;
    LumaText::Renderer edit_renderer;
    LumaText::FontFace yahei_regular;
    LumaText::FontFace yahei_bold;
    LumaText::FontFace segoe_regular;
    LumaText::FontFace segoe_semibold;
    LumaText::FontFace segoe_bold;
    LumaText::FontFace pingfang_regular;
    LumaText::FontFace pingfang_semibold;
    // Use YaHei for both Latin and CJK UI text; other faces cover missing glyphs.
    LumaText::FontCascade text_cascade;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> target_dc;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> recording_dc;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> blit_dc;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> blit_target;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> blit_staging;
    int blit_w = 0;
    int blit_h = 0;
    HDC present_dc = nullptr;
    HBITMAP present_dib = nullptr;
    HGDIOBJ present_old = nullptr;
    void* present_bits = nullptr;
    int present_w = 0;
    int present_h = 0;
    bool busy = false;
    HWND mouse_hwnd = nullptr;
    HWND mouse_scroll_hwnd = nullptr;
    bool setting_mouse_selection = false;
    int mouse_anchor = 0;
    int mouse_caret = 0;
    bool applying_mouse_selection = false;
    // A drag keeps its selection here until release. Sending EM_SETSEL for
    // every WM_MOUSEMOVE runs the native redraw guard, which briefly hides
    // the redirected edit surface and made dragged selections flicker.
    bool mouse_selection_deferred = false;
    // After a double-click the held button extends by whole words, as in
    // native editors; a pointer that shifts slightly keeps the whole word.
    bool mouse_word_drag = false;
    int mouse_word_start = 0;
    int mouse_word_end = 0;
    std::unordered_map<HWND, std::pair<int, int>> edit_selections;
    // Horizontal scroll persists like native EDIT's offset: the text moves
    // only when the caret would leave the field. Paint and mouse hit-testing
    // share it so the pointer maps to the characters actually on screen.
    std::unordered_map<HWND, float> edit_scrolls;
    static constexpr float kEditPad = 2.0f;
    // Edits carrying this property wrap onto as many lines as the text needs
    // (the icon-grid rename field). The EDIT itself stays single-line, so
    // Enter still commits and no line break can be typed or pasted; only the
    // presentation, hit-testing and Up/Down are line-aware.
    static bool EditWraps(HWND hwnd) {
        return hwnd && GetPropW(hwnd, L"Pulse.EditWrap") != nullptr;
    }
    // Last painted wrapped layout and its origin, shared by mouse hit-testing
    // and Up/Down so both act on the lines actually on screen.
    struct EditPainted {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        float x = 0.0f;
        float y = 0.0f;
    };
    std::unordered_map<HWND, EditPainted> edit_painted;
    struct EditMotionState {
        EditCaretMotion caret;
        EditTextMotion change;
        std::wstring text;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        IDWriteTextFormat* format = nullptr;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> change_layout;
        float change_scroll = 0;
        float scroll = 0, suffix_dx = 0;
        int width = 0, height = 0;
    };
    std::unordered_map<HWND, EditMotionState> edit_motion;
    std::unordered_map<HWND, EditCaretBlink> edit_blinks;
    // Up/Down keep the column they started from across short lines.
    HWND line_goal_hwnd = nullptr;
    int line_goal_caret = -1;
    float line_goal_x = 0.0f;
    // Inputs use DirectWrite in every text mode, sharing the list rendering
    // parameters while EDIT continues to own text, undo and IME.
    Microsoft::WRL::ComPtr<IDWriteFactory2> dw_factory;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams2> dw_params;
    std::uint64_t dw_params_generation = 0;
    HMONITOR dw_params_monitor = nullptr;
    struct EditLayoutSlot {
        std::wstring text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        std::uint64_t generation = 0;
        float height = 0;
        float font_size = 0;
        DWRITE_READING_DIRECTION reading = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
        DWRITE_FLOW_DIRECTION flow = DWRITE_FLOW_DIRECTION_TOP_TO_BOTTOM;
        DWRITE_LINE_SPACING_METHOD spacing_method = DWRITE_LINE_SPACING_METHOD_DEFAULT;
        float line_spacing = 0;
        float baseline = 0;
        float tab_stop = 0;
        float wrap = 0;
    };
    std::array<EditLayoutSlot, 4> edit_layouts;
    size_t next_edit_layout = 0;
    std::unordered_map<LayoutKey, LayoutSlot, LayoutKeyHash> layouts;
    std::list<LayoutKey> layout_lru;
    std::unordered_map<SurfaceKey, SurfaceEntry, SurfaceKeyHash> surfaces;
    std::list<SurfaceKey> surface_lru;
    std::uint64_t surface_bytes = 0;

    static constexpr std::uint32_t kYaHei = 1;
    static constexpr std::uint32_t kSegoe = 2;
    // Each Compositor owns a renderer. Keep per-window caches deliberately
    // small so opening Quick Look or an operation dialog cannot multiply a
    // large global-looking font cache into the process working set.
    // Keep enough layouts for a full multi-pane frame. Evicting a layout also
    // evicts every command-list surface that references it, so a tiny layout
    // limit turns an otherwise warm frame into repeated rasterization.
    static constexpr size_t kLayoutCacheLimit = 512;
    static constexpr size_t kSurfaceCountLimit = 512;
    static constexpr std::uint64_t kSurfaceCacheLimit = 8ull * 1024ull * 1024ull;
    static constexpr std::uint64_t kGlyphCacheLimit = 512ull * 1024ull;
    static constexpr std::uint64_t kMaxCachedSurfaceBytes = 64ull * 1024ull;
    static constexpr std::int32_t kUnboundedWidth64 = 10000 * 64;

    static constexpr std::uint32_t Tag(char a, char b, char c, char d) noexcept {
        return (static_cast<std::uint32_t>(a) << 24) |
               (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(c) << 8) |
               static_cast<std::uint32_t>(d);
    }

    bool MakeFace(const std::wstring& path, LumaText::FontFace& face,
                  float variable_weight = 0.0f) {
        std::error_code error;
        if (path.empty() || !std::filesystem::exists(path,error)) return false;
        lt_font_axis axis{Tag('w', 'g', 'h', 't'), variable_weight};
        auto desc = LumaText::Descriptor<lt_font_source_desc>();
        desc.source_type = LT_FONT_SOURCE_FILE;
        desc.file_path = path.c_str();
        if (variable_weight > 0.0f) {
            desc.axes = &axis;
            desc.axis_count = 1;
        }
        return lt_font_face_create(context.get(), &desc, face.put()) == LT_OK;
    }

    bool MakeCascade(const std::vector<lt_font_cascade_entry>& entries,
                     LumaText::FontCascade& cascade) {
        auto desc = LumaText::Descriptor<lt_font_cascade_desc>();
        desc.entries = entries.data();
        desc.entry_count = static_cast<std::uint32_t>(entries.size());
        desc.allow_system_fallback = false;
        return lt_font_cascade_create(context.get(), &desc, cascade.put()) == LT_OK;
    }

    static void PushFace(std::vector<lt_font_cascade_entry>& entries,
                         const LumaText::FontFace& face, std::uint16_t weight) {
        if (face) entries.push_back({face.get(), weight, 0});
    }

    void ClearSurfaces() {
        surfaces.clear();
        surface_lru.clear();
        surface_bytes = 0;
    }

    void DropSurfacesFor(const lt_text_layout* layout) {
        if (!layout) return;
        for (auto it = surfaces.begin(); it != surfaces.end(); ) {
            if (it->first.layout != layout) {
                ++it;
                continue;
            }
            surface_bytes -= it->second.estimated_bytes;
            surface_lru.erase(it->second.lru);
            stats.surface_cache_evictions++;
            it = surfaces.erase(it);
        }
    }

    void TrimSurfaces() {
        while (!surfaces.empty() &&
               (surfaces.size() > kSurfaceCountLimit ||
                surface_bytes > kSurfaceCacheLimit)) {
            const SurfaceKey oldest = surface_lru.back();
            if (const auto found = surfaces.find(oldest); found != surfaces.end()) {
                surface_bytes -= found->second.estimated_bytes;
                surfaces.erase(found);
                stats.surface_cache_evictions++;
            }
            surface_lru.pop_back();
        }
    }

    void TouchSurface(std::unordered_map<SurfaceKey, SurfaceEntry, SurfaceKeyHash>::iterator it) {
        surface_lru.splice(surface_lru.begin(), surface_lru, it->second.lru);
        it->second.lru = surface_lru.begin();
    }

    void StoreSurface(const SurfaceKey& key,
                      Microsoft::WRL::ComPtr<ID2D1CommandList> commands,
                      std::uint64_t estimated_bytes) {
        if (estimated_bytes == 0 || estimated_bytes > kMaxCachedSurfaceBytes) return;
        if (auto existing = surfaces.find(key); existing != surfaces.end()) {
            surface_bytes -= existing->second.estimated_bytes;
            surface_lru.erase(existing->second.lru);
            surfaces.erase(existing);
        }
        surface_lru.push_front(key);
        surfaces.emplace(key, SurfaceEntry{std::move(commands), estimated_bytes,
                                           surface_lru.begin()});
        surface_bytes += estimated_bytes;
        TrimSurfaces();
    }

    void EvictOldestLayout() {
        if (layout_lru.empty()) return;
        const LayoutKey oldest = layout_lru.back();
        if (const auto found = layouts.find(oldest); found != layouts.end()) {
            DropSurfacesFor(found->second.layout.get());
            layouts.erase(found);
        }
        layout_lru.pop_back();
    }

    void ClearLayouts() {
        for (auto& entry : edit_layouts) entry = {};
        next_edit_layout = 0;
        ClearSurfaces();
        layouts.clear();
        layout_lru.clear();
    }

    // CoreText-like UI text: exact FreeType coverage with linear (gamma 1.0)
    // edges, light autohinting in y only (PingFang's CFF carries no useful
    // hints; x positions and HarfBuzz widths stay unhinted), and symmetric
    // outline dilation that grows with the text colour's luminance, the way
    // CoreGraphics font smoothing thickens light-on-dark text more than
    // dark-on-light. Constants come from fitting macOS 15 CoreText renders
    // (strength 0.0145 em dark text .. 0.030 em light text, capped) and are
    // scaled by 0.92 to offset the extra contrast of hinted rows.
    static void ApplySharpConfig(lt_render_config& config,
                                 const D2D1_COLOR_F& foreground) noexcept {
        config.coverage_gamma = 1.00f;
        config.coverage_contrast = 1.00f;
        config.raster_filter = LT_RASTER_FILTER_DIRECT;
        config.flags |= LT_RENDER_CONFIG_DISABLE_STEM_COMPENSATION |
                        LT_RENDER_CONFIG_AUTOHINT_LIGHT |
                        LT_RENDER_CONFIG_DISABLE_OPTICAL_GAMMA;
        const float luminance = std::clamp(0.2126f * foreground.r + 0.7152f * foreground.g +
                                           0.0722f * foreground.b, 0.0f, 1.0f);
        const float level = std::round(luminance * 4.0f) / 4.0f;
        constexpr float kFit = 0.92f;
        config.dilation_per_em = static_cast<std::uint8_t>(
            std::lround((0.0145f + 0.0155f * level) * kFit * 4096.0f));
        config.dilation_max = static_cast<std::uint8_t>(
            std::lround((0.45f + 0.20f * level) * kFit * 64.0f));
    }

    void FillLayoutDesc(lt_text_layout_desc& desc, const LayoutKey& key,
                        lt_text_style& style, float font_size,
                        LumaText::FontCascade& cascade) const {
        style = LumaText::Descriptor<lt_text_style>();
        style.cascade = cascade.get();
        style.font_size = font_size;
        style.weight = key.weight;
        desc = LumaText::Descriptor<lt_text_layout_desc>();
        desc.text = key.text.data();
        desc.text_length = static_cast<std::uint32_t>(key.text.size());
        desc.base_style = style;
        desc.locale = key.family == kYaHei ? "zh-CN" : "en-US";
        desc.direction = LT_TEXT_DIRECTION_AUTO;
        desc.alignment = key.alignment == DWRITE_TEXT_ALIGNMENT_CENTER
            ? LT_TEXT_ALIGNMENT_CENTER
            : key.alignment == DWRITE_TEXT_ALIGNMENT_TRAILING
                ? LT_TEXT_ALIGNMENT_END : LT_TEXT_ALIGNMENT_START;
        desc.ellipsis = key.ellipsis;
        desc.max_width = key.width_64 / 64.0f;
    }

    bool Init(IDWriteFactory* dwrite, ID2D1RenderTarget* target) {
        auto context_desc = LumaText::Descriptor<lt_context_desc>();
        context_desc.dwrite_factory = dwrite;
        context_desc.cpu_cache_limit_bytes = kGlyphCacheLimit;
        if (lt_context_create(&context_desc, context.put()) != LT_OK) return false;
        // Editing uses DirectWrite for identical painting and hit-test geometry.
        dwrite->QueryInterface(IID_PPV_ARGS(&dw_factory));

        if (FAILED(target->QueryInterface(IID_PPV_ARGS(&target_dc))) || !target_dc) {
            return false;
        }
        Microsoft::WRL::ComPtr<ID2D1Device> device;
        target_dc->GetDevice(&device);
        if (!device || FAILED(device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recording_dc)) || !recording_dc) {
            return false;
        }
        recording_dc->SetDpi(96.0f, 96.0f);
        recording_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &blit_dc)) || !blit_dc) {
            return false;
        }
        blit_dc->SetDpi(96.0f, 96.0f);
        blit_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        auto renderer_desc = LumaText::Descriptor<lt_d2d_desc>();
        renderer_desc.render_target = recording_dc.Get();
        renderer_desc.manage_begin_end_draw = false;
        if (lt_d2d_renderer_create(context.get(), &renderer_desc, renderer.put()) != LT_OK) {
            return false;
        }

        renderer_desc.render_target=target_dc.Get();
        if(lt_d2d_renderer_create(context.get(),&renderer_desc,layout_renderer.put())!=LT_OK)
            layout_renderer.reset();
        renderer_desc.render_target=blit_dc.Get();
        if(lt_d2d_renderer_create(context.get(),&renderer_desc,edit_renderer.put())!=LT_OK)
            edit_renderer.reset();

        if (!MakeFace(FontPath(L"msyh.ttc"), yahei_regular) ||
            !MakeFace(FontPath(L"msyhbd.ttc"), yahei_bold)) {
            return false;
        }

        // Win11's segoeui.ttf is a ~200 KB stub; Segoe UI Variable holds the
        // real outlines. Two faces share one mapped file in LumaText.
        const std::wstring variable = FontPath(L"SegUIVar.ttf");
        if (MakeFace(variable, segoe_regular, 400.0f) &&
            MakeFace(variable, segoe_bold, 700.0f)) {
            if (!MakeFace(variable, segoe_semibold, 600.0f)) segoe_semibold.reset();
        } else {
            segoe_regular.reset();
            segoe_bold.reset();
            if (!MakeFace(FontPath(L"segoeui.ttf"), segoe_regular) ||
                !MakeFace(FontPath(L"segoeuib.ttf"), segoe_bold)) {
                return false;
            }
        }

        // Optional fallback for glyphs missing from the primary UI font.
        if (!MakeFace(InstalledFontPath(L"PingFangSC-Regular.otf"), pingfang_regular) ||
            !MakeFace(InstalledFontPath(L"PingFangSC-Semibold.otf"), pingfang_semibold)) {
            pingfang_regular.reset();
            pingfang_semibold.reset();
        }

        // Faces are picked per grapheme: nearest weight first, then cascade
        // order. Keep YaHei first at every weight for consistent UI typography.
        std::vector<lt_font_cascade_entry> entries;
        const auto push_weight = [&](std::uint16_t weight, const LumaText::FontFace& segoe,
                                     const LumaText::FontFace& pingfang,
                                     const LumaText::FontFace& yahei) {
            PushFace(entries, yahei, weight);
            PushFace(entries, segoe, weight);
            PushFace(entries, pingfang, weight);
        };
        push_weight(400, segoe_regular, pingfang_regular, yahei_regular);
        push_weight(600, segoe_semibold ? segoe_semibold : segoe_bold, pingfang_semibold, yahei_bold);
        push_weight(700, segoe_bold, pingfang_semibold, yahei_bold);
        if (entries.empty() || !MakeCascade(entries, text_cascade)) return false;

#if defined(PULSE_TEST_LUMATEXT_INIT_FAILURE)
        wchar_t fail_init[2]{};
        if (GetEnvironmentVariableW(L"PULSE_TEST_LUMATEXT_INIT_FAILURE", fail_init, ARRAYSIZE(fail_init)))
            return false;
#endif
        return true;
    }

    void ReleasePresent() noexcept {
        if (present_dc && present_old) SelectObject(present_dc, present_old);
        present_old = nullptr;
        if (present_dib) DeleteObject(present_dib);
        present_dib = nullptr;
        present_bits = nullptr;
        present_w = 0;
        present_h = 0;
        if (present_dc) DeleteDC(present_dc);
        present_dc = nullptr;
    }

    bool EnsurePresent(int w, int h) {
        if (present_dib && present_dc && present_bits && present_w == w && present_h == h) {
            return true;
        }
        if (present_dc && present_old) {
            SelectObject(present_dc, present_old);
            present_old = nullptr;
        }
        if (present_dib) {
            DeleteObject(present_dib);
            present_dib = nullptr;
            present_bits = nullptr;
        }
        if (!present_dc) present_dc = CreateCompatibleDC(nullptr);
        if (!present_dc || w <= 0 || h <= 0) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        present_dib = CreateDIBSection(present_dc, &info, DIB_RGB_COLORS, &present_bits,
                                       nullptr, 0);
        if (!present_dib || !present_bits) {
            if (present_dib) DeleteObject(present_dib);
            present_dib = nullptr;
            present_bits = nullptr;
            return false;
        }
        present_old = SelectObject(present_dc, present_dib);
        present_w = w;
        present_h = h;
        return true;
    }

    bool PresentEditBits(HWND hwnd, HDC paint_dc, const void* bits, int w, int h, bool preserve_alpha) {
        if (!hwnd || !bits || w <= 0 || h <= 0) return false;
        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        const bool layered =
            (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
        if (layered && !paint_dc) {
            // Own the redirected bitmap. SetLayeredWindowAttributes + WM_PAINT
            // lets EDIT's GetDC/ClearType selection paint win on mouse-down.
            if (!EnsurePresent(w, h)) return false;
            auto* dst = static_cast<std::uint8_t*>(present_bits);
            std::memcpy(dst, bits, bytes);
            if (!preserve_alpha)
                for (int i = 0; i < w * h; ++i) dst[static_cast<size_t>(i) * 4u + 3u] = 255;
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            POINT dst_pt{wr.left, wr.top};
            SIZE sz{w, h};
            POINT src{0, 0};
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            // Layout owns the position. Child coordinates must never be replaced
            // with the screen-space rectangle during a text repaint.
            const bool child = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0;
            const BOOL ok = UpdateLayeredWindow(hwnd, nullptr, child ? nullptr : &dst_pt, &sz, present_dc,
                                                &src, 0, &blend, ULW_ALPHA);
            return ok != FALSE;
        }
        if (!paint_dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        return SetDIBitsToDevice(paint_dc, 0, 0, static_cast<DWORD>(w),
                                 static_cast<DWORD>(h), 0, 0, 0, static_cast<UINT>(h),
                                 bits, &info, DIB_RGB_COLORS) > 0;
    }

    void Shutdown() noexcept {
        ClearLayouts();
        ReleasePresent();
        dw_params.Reset();
        dw_factory.Reset();
        dw_params_monitor = nullptr;
        blit_target.Reset();
        blit_staging.Reset();
        blit_dc.Reset();
        blit_w = 0;
        blit_h = 0;
        busy = false;
        text_cascade.reset();
        pingfang_semibold.reset();
        pingfang_regular.reset();
        segoe_bold.reset();
        segoe_semibold.reset();
        segoe_regular.reset();
        yahei_bold.reset();
        yahei_regular.reset();
        edit_renderer.reset();
        layout_renderer.reset();
        renderer.reset();
        recording_dc.Reset();
        target_dc.Reset();
        context.reset();
    }

    LumaText::TextLayout* GetLayout(const LayoutKey& key, float font_size,
                                    LumaText::FontCascade& cascade) {
        if (auto found = layouts.find(key); found != layouts.end()) {
            layout_lru.splice(layout_lru.begin(), layout_lru, found->second.lru);
            found->second.lru = layout_lru.begin();
            return &found->second.layout;
        }
        lt_text_style style{};
        lt_text_layout_desc desc{};
        FillLayoutDesc(desc, key, style, font_size, cascade);

        LumaText::TextLayout layout;
        if (lt_text_layout_create(context.get(), &desc, layout.put()) != LT_OK) return nullptr;
        while (layouts.size() >= kLayoutCacheLimit) EvictOldestLayout();
        layout_lru.push_front(key);
        const auto inserted = layouts.emplace(key, LayoutSlot{std::move(layout),
                                                              layout_lru.begin()});
        return &inserted.first->second.layout;
    }

    LumaText::TextLayout* FitEndByClusters(LayoutKey key, float font_size,
                                          LumaText::FontCascade& cascade, float width,
                                          float ink, lt_text_metrics& metrics) {
        key.width_64 = kUnboundedWidth64;
        key.ellipsis = LT_TEXT_ELLIPSIS_NONE;
        auto* source = GetLayout(key, font_size, cascade);
        if (!source || !*source || key.text.empty()) return nullptr;
        const std::wstring text = key.text;
        key.text = L"\u2026";
        auto* fitted = GetLayout(key, font_size, cascade);
        if (!fitted || !*fitted || lt_text_layout_get_metrics(fitted->get(), &metrics) != LT_OK ||
            metrics.line_count != 1 || metrics.width + ink > width + 0.5f) return nullptr;

        LayoutKey best = key;
        std::uint32_t prefix = 0;
        size_t low = 0, high = text.size() - 1;
        while (low < high) {
            const size_t middle = low + (high - low + 1) / 2;
            float x = 0.0f, y = 0.0f;
            auto hit = LumaText::Descriptor<lt_hit_test_metrics>();
            if (lt_text_layout_hit_test_position(source->get(), static_cast<std::uint32_t>(middle),
                    false, &x, &y, &hit) != LT_OK || hit.text_position > middle) return nullptr;
            key.text = text.substr(0, hit.text_position) + L"\u2026";
            auto* candidate = GetLayout(key, font_size, cascade);
            if (!candidate || !*candidate || lt_text_layout_get_metrics(candidate->get(), &metrics) != LT_OK)
                return nullptr;
            if (metrics.line_count == 1 && metrics.width + ink <= width + 0.5f) {
                low = middle;
                best = key;
                prefix = hit.text_position;
            } else high = middle - 1;
        }
        fitted = GetLayout(best, font_size, cascade);
        if (!fitted || !*fitted || lt_text_layout_get_metrics(fitted->get(), &metrics) != LT_OK) return nullptr;
        stats.end_ellipsis_repairs++;
        stats.last_ellipsis_prefix_length = prefix;
        return fitted;
    }

    bool DrawOn(ID2D1DeviceContext* dc, std::wstring_view text, IDWriteTextFormat* format,
                 const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                 const D2D1_COLOR_F& background, DWRITE_TEXT_ALIGNMENT alignment) {
        if (!dc || busy || !renderer || !format || text.empty() ||
            format->GetFontStyle() != DWRITE_FONT_STYLE_NORMAL ||
            format->GetFontStretch() != DWRITE_FONT_STRETCH_NORMAL ||
            text.size() > UINT32_MAX) {
            return false;
        }
        struct BusyGuard {
            bool& flag;
            explicit BusyGuard(bool& value) : flag(value) { flag = true; }
            ~BusyGuard() { flag = false; }
        } busy_guard(busy);
        const float width = bounds.right - bounds.left;
        const float height = bounds.bottom - bounds.top;
        const float font_size = format->GetFontSize();
        if (!(width > 0.0f) || !(height > 0.0f) || !(font_size > 0.0f)) return false;

        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return false;

        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return false;

        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
        // Dest is an ink clip. Advance-only overflow left the last glyph's
        // glyph overhang shaved off ("File" → "Fil").
        const float ink = pulse::ui::typography::InkPad(format);
        if (metrics.width + ink > width + 0.5f) {
            key.width_64 = static_cast<std::int32_t>(
                std::lround(std::max(1.0f, width - ink) * 64.0f));
            key.ellipsis = LT_TEXT_ELLIPSIS_END;
            layout = GetLayout(key, font_size, *cascade);
            if (!layout || !*layout) return false;
            if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
            // Mixed runs can retain the full advance despite native ellipsis.
            // Fit an explicit sign at a Luma cluster boundary so shaping and
            // painting stay on the same backend without cutting glyphs.
            if (metrics.line_count != 1 || metrics.width + ink > width + 0.5f) {
                layout = FitEndByClusters(key, font_size, *cascade, width, ink, metrics);
                if (!layout) return false;
            }
        }
        const float floor_x = std::floor(bounds.left);
        const float snapped_y = std::round(bounds.top);
        const float phase_x = bounds.left - floor_x;
        const auto phase_bucket = [](float phase, int buckets) {
            const long rounded = std::lround(phase * static_cast<float>(buckets));
            return static_cast<std::uint8_t>(std::clamp<long>(
                rounded, 0l, static_cast<long>(buckets - 1)));
        };
        const std::uint8_t x_phase_8 = phase_bucket(phase_x, 8);
        const float cached_phase_x = static_cast<float>(x_phase_8) / 8.0f;
        const auto color_byte = [](float value) {
            return static_cast<std::uint32_t>(std::clamp(
                std::lround(value * 255.0f), 0l, 255l));
        };
        const std::uint32_t packed_foreground =
            color_byte(foreground.r) |
            (color_byte(foreground.g) << 8) |
            (color_byte(foreground.b) << 16) |
            (color_byte(foreground.a) << 24);
        const std::uint32_t packed_background =
            color_byte(background.r) |
            (color_byte(background.g) << 8) |
            (color_byte(background.b) << 16) |
            (color_byte(background.a) << 24);
        const bool dark = 0.2126f * background.r + 0.7152f * background.g +
                          0.0722f * background.b < 0.5f;
        const SurfaceKey surface_key{
            layout->get(),
            std::hash<std::wstring_view>{}(text),
            static_cast<std::int32_t>(std::lround(width * 64.0f)),
            static_cast<std::int32_t>(std::lround(height * 64.0f)),
            x_phase_8,
            packed_foreground,
            packed_background,
            dark,
            alignment,
            format->GetParagraphAlignment(),
        };
        if (auto cached = surfaces.find(surface_key); cached != surfaces.end()) {
                TouchSurface(cached);
                const auto offset = D2D1::Point2F(
                    floor_x + phase_x - cached_phase_x,
                    snapped_y);
                // The command list already contains the canonical text clip;
                // avoid repeating target clip state changes for every row.
                dc->DrawImage(cached->second.commands.Get(), &offset, nullptr,
                              D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                              D2D1_COMPOSITE_MODE_SOURCE_OVER);
                stats.surface_cache_hits++;
                stats.draw_calls++;
                return true;
        }
        stats.surface_cache_misses++;

        Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
        if (FAILED(recording_dc->CreateCommandList(&commands)) || !commands) return false;
        recording_dc->SetTarget(commands.Get());
        recording_dc->BeginDraw();
        auto frame_desc = LumaText::Descriptor<lt_frame_desc>();
        frame_desc.dpi_x = 96.0f;
        frame_desc.dpi_y = 96.0f;
        LumaText::Frame frame;
        if (lt_frame_begin(renderer.get(), &frame_desc, frame.put()) != LT_OK) {
            recording_dc->EndDraw();
            recording_dc->SetTarget(nullptr);
            return false;
        }

        auto draw = LumaText::Descriptor<lt_draw_text_desc>();
        draw.origin_x = cached_phase_x;
        if (alignment == DWRITE_TEXT_ALIGNMENT_CENTER) {
            draw.origin_x += std::max(0.0f, (width - metrics.width) * 0.5f);
        } else if (alignment == DWRITE_TEXT_ALIGNMENT_TRAILING) {
            draw.origin_x += std::max(0.0f, width - metrics.width);
        }
        // The target uses physical pixels: keep stems on full scan lines and
        // avoid smoothing small UI text a second time with a raster filter.
        const float paragraph_offset = format->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_NEAR
            ? 0.0f : format->GetParagraphAlignment() == DWRITE_PARAGRAPH_ALIGNMENT_FAR
                ? height - metrics.height : (height - metrics.height) * 0.5f;
        draw.origin_y = std::round(paragraph_offset + metrics.ascent) - metrics.ascent;
        draw.clip = {cached_phase_x, 0.0f, cached_phase_x + width, height};
        draw.clip_enabled = true;
        draw.foreground = {foreground.r, foreground.g, foreground.b, foreground.a};
        draw.background = {background.r, background.g, background.b, background.a};
        draw.background_type = LT_BACKGROUND_TRANSPARENT;
        draw.render_config = LumaText::Descriptor<lt_render_config>();
        ApplySharpConfig(draw.render_config, foreground);
        // No render profile: it would pick a config by background and drop
        // the foreground-dependent dilation set above.

        const lt_result result = lt_frame_draw_text_layout(frame.get(), layout->get(), &draw);
        auto frame_stats = LumaText::Descriptor<lt_frame_stats>();
        if (result == LT_OK && lt_frame_get_stats(frame.get(), &frame_stats) == LT_OK) {
            stats.freetype_glyphs += frame_stats.freetype_glyphs;
            stats.cache_hits += frame_stats.glyph_cache_hits;
        }
        const lt_result end_result = lt_frame_end(frame.get());
        const HRESULT recording_result = recording_dc->EndDraw();
        recording_dc->SetTarget(nullptr);
        if (result != LT_OK || end_result != LT_OK || FAILED(recording_result) ||
            FAILED(commands->Close())) {
            return false;
        }

        const float ink_width = std::min(width, std::max(1.0f, metrics.width + 2.0f));
        const std::uint64_t estimated_bytes = static_cast<std::uint64_t>(
            std::max(1l, std::lround(std::ceil(ink_width)))) *
            static_cast<std::uint64_t>(std::max(1l, std::lround(std::ceil(height)))) * 4ull;
        StoreSurface(surface_key, commands, estimated_bytes);

        const auto offset = D2D1::Point2F(
            floor_x + phase_x - cached_phase_x,
            snapped_y);
        dc->DrawImage(commands.Get(), &offset, nullptr,
                      D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                      D2D1_COMPOSITE_MODE_SOURCE_OVER);
        stats.draw_calls++;
        return true;
    }

    bool Draw(std::wstring_view text, IDWriteTextFormat* format,
              const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
              const D2D1_COLOR_F& background, DWRITE_TEXT_ALIGNMENT alignment) {
        return DrawOn(target_dc.Get(), text, format, bounds, foreground, background,
                      alignment);
    }

    // A DirectWrite layout keeps its own shaping, fallback fonts, wrapping and
    // trimming; LumaText only rasterizes the glyph runs. Ranges coloured with
    // SetDrawingEffect(ID2D1SolidColorBrush) keep their colour.
    bool DrawLayoutOn(LumaText::Renderer& destination, ID2D1DeviceContext* dc,
                      D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                    const D2D1_COLOR_F& foreground, bool clip) {
        if (!destination || !dc || !layout) return false;
        float dpi_x = 96.0f, dpi_y = 96.0f;
        dc->GetDpi(&dpi_x, &dpi_y);
        auto frame_desc = LumaText::Descriptor<lt_frame_desc>();
        frame_desc.dpi_x = dpi_x;
        frame_desc.dpi_y = dpi_y;
        LumaText::Frame frame;
        if (lt_frame_begin(destination.get(), &frame_desc, frame.put()) != LT_OK) return false;
        auto draw = LumaText::Descriptor<lt_draw_text_desc>();
        draw.origin_x = origin.x;
        draw.origin_y = origin.y;
        draw.clip_enabled = clip;
        if (clip) {
            draw.clip = {origin.x, origin.y, origin.x + layout->GetMaxWidth(),
                         origin.y + layout->GetMaxHeight()};
        }
        draw.foreground = {foreground.r, foreground.g, foreground.b, foreground.a};
        draw.background_type = LT_BACKGROUND_TRANSPARENT;
        draw.render_config = LumaText::Descriptor<lt_render_config>();
        ApplySharpConfig(draw.render_config, foreground);
        const lt_result result = lt_frame_draw_layout(frame.get(), layout, &draw);
        auto frame_stats = LumaText::Descriptor<lt_frame_stats>();
        if (result == LT_OK && lt_frame_get_stats(frame.get(), &frame_stats) == LT_OK) {
            stats.freetype_glyphs += frame_stats.freetype_glyphs;
            stats.cache_hits += frame_stats.glyph_cache_hits;
        }
        const lt_result end_result = lt_frame_end(frame.get());
        if(result!=LT_OK || end_result!=LT_OK) return false;
        ++stats.draw_calls;
        return true;
    }

    bool DrawLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                    const D2D1_COLOR_F& foreground, bool clip) {
        return DrawLayoutOn(layout_renderer,target_dc.Get(),origin,layout,foreground,clip);
    }

    // Two colours in one shaped line (a dimmed file extension): the same
    // layout is drawn twice, clipped at the caret x of `split` in that layout,
    // so glyph positions never depend on substring widths.
    bool DrawSplit(std::wstring_view text, IDWriteTextFormat* format,
                   const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                   std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                   const D2D1_COLOR_F& background) {
        ID2D1DeviceContext* dc = target_dc.Get();
        if (!dc || busy || !renderer || !format || split == 0 || split >= text.size()) return false;
        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        const float font_size = format->GetFontSize();
        if (!(font_size > 0.0f) || !ResolveCascade(format, text, family, cascade) || !cascade) {
            return false;
        }
        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return false;
        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
        // DrawOn would ellipsize this line, and the split would no longer
        // index the drawn glyphs.
        const float ink = pulse::ui::typography::InkPad(format);
        if (metrics.width + ink > bounds.right - bounds.left + 0.5f) return false;
        float split_x = 0.0f, split_y = 0.0f;
        auto hit = LumaText::Descriptor<lt_hit_test_metrics>();
        if (lt_text_layout_hit_test_position(layout->get(), split, false,
                                             &split_x, &split_y, &hit) != LT_OK) {
            return false;
        }
        const float cut = std::round(bounds.left + split_x);
        const float top = bounds.top - 8.0f, bottom = bounds.bottom + 8.0f;
        dc->PushAxisAlignedClip(D2D1::RectF(std::floor(bounds.left) - 8.0f, top, cut, bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        const bool head = DrawOn(dc, text, format, bounds, foreground, background,
                                 DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->PopAxisAlignedClip();
        if (!head) return false;
        dc->PushAxisAlignedClip(D2D1::RectF(cut, top, std::ceil(bounds.right) + 8.0f, bottom),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        // Same layout and geometry as the head draw; it cannot fail where
        // that one succeeded short of a device loss, which redraws anyway.
        DrawOn(dc, text, format, bounds, split_foreground, background,
               DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->PopAxisAlignedClip();
        return true;
    }

    static bool ContainsCjk(std::wstring_view text) noexcept {
        for (const wchar_t c : text) {
            if ((c >= 0x2E80 && c <= 0x9FFF) ||
                (c >= 0xF900 && c <= 0xFAFF) ||
                (c >= 0xFF00 && c <= 0xFFEF)) {
                return true;
            }
        }
        return false;
    }

    bool ResolveCascade(IDWriteTextFormat* format, std::wstring_view text,
                        std::uint32_t& family, LumaText::FontCascade*& cascade) {
        if (!format) return false;
        const std::wstring family_name = FontFamily(format);
        const bool yahei = Contains(family_name, L"Microsoft YaHei");
        const bool segoe_text = Contains(family_name, L"Segoe UI") &&
                   !Contains(family_name, L"Icons") &&
                   !Contains(family_name, L"Assets") &&
                   !Contains(family_name, L"Emoji");
        if (!yahei && !segoe_text) return false;
        // Chinese runs shape as zh-CN even when the shell language (and the
        // DirectWrite family) is English / Segoe; the faces are the same.
        family = (yahei || ContainsCjk(text)) ? kYaHei : kSegoe;
        cascade = &text_cascade;
        return text_cascade.get() != nullptr;
    }

    bool Measure(std::wstring_view text, IDWriteTextFormat* format,
                 float& width, float* height) {
        width = 0.0f;
        if (height) *height = 0.0f;
        if (!renderer || !format || text.empty()) return text.empty();
        std::uint32_t family = 0;
        LumaText::FontCascade* cascade = nullptr;
        if (!ResolveCascade(format, text, family, cascade) || !cascade) return false;
        const float font_size = format->GetFontSize();
        if (!(font_size > 0.0f)) return false;
        LayoutKey key;
        key.text.assign(text);
        key.family = family;
        key.weight = static_cast<std::uint16_t>(std::clamp<int>(
            static_cast<int>(format->GetFontWeight()), 1, 1000));
        key.size_64 = static_cast<std::int32_t>(std::lround(font_size * 64.0f));
        key.width_64 = kUnboundedWidth64;
        key.alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        LumaText::TextLayout* layout = GetLayout(key, font_size, *cascade);
        if (!layout || !*layout) return false;
        auto metrics = LumaText::Descriptor<lt_text_metrics>();
        if (lt_text_layout_get_metrics(layout->get(), &metrics) != LT_OK) return false;
        width = metrics.width;
        if (height) *height = metrics.height;
        return true;
    }

    static std::wstring ReadEditText(HWND hwnd) {
        const int len = GetWindowTextLengthW(hwnd);
        if (len <= 0) return {};
        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
        const int copied = GetWindowTextW(hwnd, text.data(), len + 1);
        text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        return text;
    }

    Microsoft::WRL::ComPtr<IDWriteTextLayout> DwEditLayout(std::wstring_view text,
                                                           IDWriteTextFormat* format,
                                                           float height, float wrap_width = 0.0f) {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (!dw_factory || !format || text.size() > UINT32_MAX) return layout;
        const float line_height = std::max(1.0f, height);
        const float font_size = format->GetFontSize();
        const auto reading = format->GetReadingDirection();
        const auto flow = format->GetFlowDirection();
        DWRITE_LINE_SPACING_METHOD spacing_method = DWRITE_LINE_SPACING_METHOD_DEFAULT;
        float line_spacing = 0, baseline = 0;
        format->GetLineSpacing(&spacing_method, &line_spacing, &baseline);
        const float tab_stop = format->GetIncrementalTabStop();
        const auto generation = pulse::ui::typography::Generation();
        // Caret blinking and drag selection reuse the same layout. Retaining
        // the format prevents its address being recycled into a false hit.
        const bool cacheable = text.size() <= 4096;
        if (cacheable) {
            for (const auto& entry : edit_layouts) {
                if (entry.layout && entry.format.Get() == format && entry.generation == generation &&
                    entry.height == line_height && entry.font_size == font_size && entry.reading == reading &&
                    entry.flow == flow && entry.spacing_method == spacing_method &&
                    entry.line_spacing == line_spacing && entry.baseline == baseline &&
                    entry.tab_stop == tab_stop && entry.wrap == wrap_width && entry.text == text) {
                    ++stats.edit_layout_cache_hits;
                    return entry.layout;
                }
            }
        }
        if (FAILED(pulse::ui::typography::CreateTextLayout(dw_factory.Get(), text.data(), static_cast<UINT32>(text.size()),
                format, wrap_width > 0.0f ? wrap_width : 1.0e6f, line_height, &layout)) || !layout) {
            return nullptr;
        }
        if (wrap_width > 0.0f) {
            // Wrapped fields center their lines like the icon names they edit.
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
        // One unwrapped, untrimmed line; the edit scrolls it horizontally.
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // Keep the unbounded line at the physical left origin even for RTL
        // paragraphs; LEADING would place those glyphs near x = 1e6.
        layout->SetTextAlignment(reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ?
            DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&none, nullptr);
        ++stats.edit_layout_builds;
        if (cacheable) {
            auto& entry = edit_layouts[next_edit_layout];
            next_edit_layout = (next_edit_layout + 1) % edit_layouts.size();
            entry = {std::wstring(text), format, layout, generation, line_height, font_size, reading, flow,
                spacing_method, line_spacing, baseline, tab_stop, wrap_width};
        }
        return layout;
    }

    // Up/Down in a wrapped field move to the neighbouring line at the same
    // column; past the first or last line they go to the start or end.
    bool MoveEditLine(HWND hwnd, bool down, bool extend) {
        if (!EditWraps(hwnd)) return false;
        const auto shown = edit_painted.find(hwnd);
        if (shown == edit_painted.end() || !shown->second.layout) return false;
        IDWriteTextLayout* layout = shown->second.layout.Get();
        const int length = GetWindowTextLengthW(hwnd);
        DWORD lo = 0, hi = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
        int anchor = static_cast<int>(lo), caret = static_cast<int>(hi);
        if (const auto saved = edit_selections.find(hwnd); saved != edit_selections.end() &&
            std::min(saved->second.first, saved->second.second) == static_cast<int>(lo) &&
            std::max(saved->second.first, saved->second.second) == static_cast<int>(hi)) {
            anchor = saved->second.first;
            caret = saved->second.second;
        }
        // Without Shift a selection collapses from the edge in that direction.
        if (!extend && lo != hi) caret = down ? static_cast<int>(hi) : static_cast<int>(lo);
        caret = std::clamp(caret, 0, length);
        float x = 0.0f, y = 0.0f, line_h = 0.0f;
        DwCaretPoint(layout, caret, length, x, y, line_h);
        if (line_goal_hwnd == hwnd && line_goal_caret == caret) x = line_goal_x;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        const float target_y = down ? y + std::max(1.0f, line_h) * 1.5f : y - std::max(1.0f, line_h) * 0.5f;
        int target = 0;
        if (target_y < 0.0f) target = 0;
        else if (target_y >= metrics.height) target = length;
        else target = DwHitIndex(layout, x, length, target_y);
        if (!extend) anchor = target;
        SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(anchor), static_cast<LPARAM>(target));
        line_goal_hwnd = hwnd;
        line_goal_caret = target;
        line_goal_x = x;
        return true;
    }

    static float DwCaretX(IDWriteTextLayout* layout, int position, int length) {
        if (!layout || length <= 0) return 0.0f;
        const bool trailing = position >= length;
        const UINT32 probe = static_cast<UINT32>(trailing ? length - 1 : position);
        float x = 0.0f;
        float y = 0.0f;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestTextPosition(probe, trailing ? TRUE : FALSE, &x, &y, &hit)))
            return 0.0f;
        return x;
    }

    // Caret top-left and line height; works on empty text too, which a
    // centered wrapped field places in the middle of its line.
    static void DwCaretPoint(IDWriteTextLayout* layout, int position, int length,
                             float& x, float& y, float& height) {
        x = 0.0f; y = 0.0f; height = 0.0f;
        if (!layout) return;
        const bool trailing = length > 0 && position >= length;
        const UINT32 probe = static_cast<UINT32>(trailing ? length - 1 : std::max(0, position));
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestTextPosition(probe, trailing ? TRUE : FALSE, &x, &y, &hit))) return;
        y = hit.top;
        height = hit.height;
    }

    static int DwHitIndex(IDWriteTextLayout* layout, float x, int length, float y = 1.0f) {
        if (!layout || length <= 0) return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestPoint(x, y, &trailing, &inside, &hit))) return length;
        const int index = static_cast<int>(hit.textPosition) +
            (trailing ? static_cast<int>(hit.length) : 0);
        return std::clamp(index, 0, length);
    }

    IDWriteRenderingParams* DwParams(HWND hwnd) {
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        const std::uint64_t generation = pulse::ui::typography::Generation();
        if (!dw_params || dw_params_generation != generation || dw_params_monitor != monitor) {
            dw_params.Reset();
            pulse::ui::typography::CreateRenderingParams(dw_factory.Get(), monitor, &dw_params);
            dw_params_generation = generation;
            dw_params_monitor = monitor;
        }
        return dw_params.Get();
    }

    static float EditScrollX(float caret_x, float width, float previous = 0.0f,
                             float text_width = -1.0f) {
        const float inner = std::max(1.0f, width - kEditPad);
        float scroll = std::max(0.0f, previous);
        if (caret_x - scroll > inner) scroll = caret_x - inner;
        if (caret_x - scroll < kEditPad) scroll = std::max(0.0f, caret_x - kEditPad);
        if (text_width >= 0.0f) scroll = std::min(scroll, std::max(0.0f, text_width - inner));
        return std::max(0.0f, scroll);
    }

    static float EditTextWidth(IDWriteTextLayout* layout) {
        DWRITE_TEXT_METRICS metrics{};
        if (!layout || FAILED(layout->GetMetrics(&metrics))) return -1.0f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    float EditScroll(HWND hwnd, IDWriteTextLayout* layout, float caret_x, float width) const {
        const auto saved = edit_scrolls.find(hwnd);
        return EditScrollX(caret_x, width, saved != edit_scrolls.end() ? saved->second : 0.0f,
                           EditTextWidth(layout));
    }

    // Character under the pointer (not the nearest caret stop), so a click on
    // the right half of a letter still belongs to that letter's word.
    static int DwCharIndex(IDWriteTextLayout* layout, float x, int length, float y = 1.0f) {
        if (!layout || length <= 0) return 0;
        BOOL trailing = FALSE;
        BOOL inside = FALSE;
        DWRITE_HIT_TEST_METRICS hit{};
        if (FAILED(layout->HitTestPoint(x, y, &trailing, &inside, &hit))) return length - 1;
        return std::clamp(static_cast<int>(hit.textPosition), 0, length - 1);
    }

    void ApplyMouseSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                             IDWriteTextFormat* format) {
        if (!hwnd || !format) return;
        if (msg != WM_LBUTTONDOWN && msg != WM_LBUTTONDBLCLK && msg != WM_MOUSEMOVE)
            return;
        if (msg == WM_MOUSEMOVE &&
            (GetCapture() != hwnd || (wParam & MK_LBUTTON) == 0)) {
            return;
        }

        mouse_scroll_hwnd = hwnd;
        const std::wstring text = ReadEditText(hwnd);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float width = static_cast<float>(rc.right - rc.left);
        DWORD sel0 = 0, sel1 = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel0),
                     reinterpret_cast<LPARAM>(&sel1));
        if (mouse_hwnd != hwnd) {
            const auto saved = edit_selections.find(hwnd);
            if (saved != edit_selections.end() &&
                std::min(saved->second.first, saved->second.second) == static_cast<int>(sel0) &&
                std::max(saved->second.first, saved->second.second) == static_cast<int>(sel1)) {
                mouse_hwnd = hwnd;
                mouse_anchor = saved->second.first;
                mouse_caret = saved->second.second;
            }
        }
        int caret_for_scroll = static_cast<int>(sel1);
        if (mouse_hwnd == hwnd && (mouse_caret == static_cast<int>(sel0) || mouse_caret == static_cast<int>(sel1)))
            caret_for_scroll = mouse_caret;
        caret_for_scroll = std::clamp(caret_for_scroll, 0, static_cast<int>(text.size()));

        const int length = static_cast<int>(text.size());
        const bool wraps = EditWraps(hwnd);
        const auto dw = DwEditLayout(text, format, static_cast<float>(rc.bottom - rc.top),
                                     wraps ? std::max(1.0f, width - 2.0f * kEditPad) : 0.0f);
        if (!dw) return;
        const float caret_x = DwCaretX(dw.Get(), caret_for_scroll, length);
        // Hit-test against the offset last painted, not one re-derived from
        // the caret: deriving it again slid the text under a held pointer.
        const auto painted = edit_scrolls.find(hwnd);
        float scroll = painted != edit_scrolls.end()
            ? painted->second : EditScroll(hwnd, dw.Get(), caret_x, width);
        const float pointer_x = static_cast<float>(GET_X_LPARAM(lParam));
        if (!wraps && msg == WM_MOUSEMOVE && (pointer_x < 0.0f || pointer_x >= width)) {
            const float outside = pointer_x < 0.0f ? pointer_x : pointer_x - width + 1.0f;
            const float maximum = std::max(0.0f, EditTextWidth(dw.Get()) - std::max(1.0f, width - kEditPad));
            scroll = std::clamp(scroll + outside, 0.0f, maximum);
            edit_scrolls[hwnd] = scroll;
        }
        float text_x = pointer_x - kEditPad + scroll;
        float text_y = 1.0f;
        if (wraps) {
            const auto shown = edit_painted.find(hwnd);
            const float origin_x = shown != edit_painted.end() ? shown->second.x : kEditPad;
            const float origin_y = shown != edit_painted.end() ? shown->second.y : 0.0f;
            text_x = static_cast<float>(GET_X_LPARAM(lParam)) - origin_x;
            text_y = static_cast<float>(GET_Y_LPARAM(lParam)) - origin_y;
        }
        const int index = DwHitIndex(dw.Get(), text_x, length, text_y);
        ++stats.edit_directwrite_hits;

        if (msg == WM_LBUTTONDBLCLK) {
            const auto [start, end] = EditWordRangeAt(text, DwCharIndex(dw.Get(), text_x, length, text_y));
            mouse_hwnd = hwnd;
            mouse_anchor = start;
            mouse_caret = end;
            mouse_word_drag = true;
            mouse_word_start = start;
            mouse_word_end = end;
            mouse_selection_deferred = false;
            SetMouseSelection(hwnd);
            return;
        }
        if (msg == WM_LBUTTONDOWN) {
            mouse_word_drag = false;
            if (wParam & MK_SHIFT) {
                if (mouse_hwnd != hwnd) mouse_anchor = static_cast<int>(sel0);
                mouse_hwnd = hwnd;
                mouse_caret = index;
            } else {
                mouse_hwnd = hwnd;
                mouse_anchor = index;
                mouse_caret = index;
            }
            mouse_selection_deferred = false;
            SetMouseSelection(hwnd);
            return;
        }
        if (mouse_word_drag && mouse_hwnd == hwnd) {
            const auto [start, end] = EditWordRangeAt(text, DwCharIndex(dw.Get(), text_x, length, text_y));
            if (start < mouse_word_start) {
                mouse_anchor = mouse_word_end;
                mouse_caret = start;
            } else {
                mouse_anchor = mouse_word_start;
                mouse_caret = std::max(end, mouse_word_end);
            }
            mouse_selection_deferred = true;
            return;
        }
        if (mouse_hwnd != hwnd) {
            mouse_hwnd = hwnd;
            mouse_anchor = static_cast<int>(sel0);
        }
        mouse_word_drag = false;
        mouse_caret = index;
        mouse_selection_deferred = true;
    }

    bool DeferredSelection(HWND hwnd) const {
        return mouse_selection_deferred && mouse_hwnd == hwnd;
    }

    void CommitMouseSelection(HWND hwnd) {
        if (!DeferredSelection(hwnd)) return;
        mouse_selection_deferred = false;
        if (IsWindow(hwnd)) SetMouseSelection(hwnd);
    }

    void SetMouseSelection(HWND hwnd) {
        setting_mouse_selection = true;
        SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(mouse_anchor), static_cast<LPARAM>(mouse_caret));
        setting_mouse_selection = false;
    }

    bool EnsureBlit(int w, int h, bool readback) {
        if (blit_target && (!readback || blit_staging) && blit_w == w && blit_h == h) return true;
        blit_target.Reset();
        blit_staging.Reset();
        blit_w = 0;
        blit_h = 0;
        if (!blit_dc || w <= 0 || h <= 0) return false;
        const D2D1_SIZE_U size{static_cast<UINT32>(w), static_cast<UINT32>(h)};
        const auto pixel = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                             D2D1_ALPHA_MODE_PREMULTIPLIED);
        const auto target_props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, pixel, 96.0f, 96.0f);
        const auto cpu_props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            pixel, 96.0f, 96.0f);
        if (FAILED(blit_dc->CreateBitmap(size, nullptr, 0, &target_props, &blit_target))) {
            return false;
        }
        if (readback && FAILED(blit_dc->CreateBitmap(size, nullptr, 0, &cpu_props, &blit_staging))) {
            blit_target.Reset();
            return false;
        }
        blit_w = w;
        blit_h = h;
        return true;
    }

    bool PaintEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                   const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background,
                   ID2D1Bitmap1** bitmap = nullptr) {
        if (!hwnd || !format || !blit_dc || !dw_factory) return false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;

        if (!EnsureBlit(w, h, bitmap == nullptr)) return false;

        const int len = GetWindowTextLengthW(hwnd);
        std::wstring text;
        if (len > 0) {
            text.resize(static_cast<size_t>(len) + 1);
            const int copied = GetWindowTextW(hwnd, text.data(), len + 1);
            text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        }

        DWORD sel0 = 0, sel1 = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&sel0),
                     reinterpret_cast<LPARAM>(&sel1));
        if (DeferredSelection(hwnd)) {
            const int length = static_cast<int>(text.size());
            sel0 = static_cast<DWORD>(std::clamp(mouse_anchor, 0, length));
            sel1 = static_cast<DWORD>(std::clamp(mouse_caret, 0, length));
        }
        const int sel_lo = static_cast<int>(std::min(sel0, sel1));
        const int sel_hi = static_cast<int>(std::max(sel0, sel1));
        int caret = static_cast<int>(sel1);
        if (mouse_hwnd == hwnd && (mouse_caret == sel_lo || mouse_caret == sel_hi))
            caret = mouse_caret;
        else if (const auto saved = edit_selections.find(hwnd); saved != edit_selections.end() &&
            std::min(saved->second.first, saved->second.second) == sel_lo &&
            std::max(saved->second.first, saved->second.second) == sel_hi)
            caret = saved->second.second;
        caret = std::clamp(caret, 0, static_cast<int>(text.size()));

        const bool wraps = EditWraps(hwnd);
        const float wrap_width = wraps ? std::max(1.0f, static_cast<float>(w) - 2.0f * kEditPad) : 0.0f;
        const auto dw_layout = DwEditLayout(text, format, static_cast<float>(h), wrap_width);
        IDWriteRenderingParams* params = DwParams(hwnd);
        if (!dw_layout || !params) return false;
        const auto caret_origin = [&](int position) {
            return DwCaretX(dw_layout.Get(), position, static_cast<int>(text.size()));
        };
        float caret_x = caret_origin(caret);
        const float pad = kEditPad;
        // Wrapped fields never scroll: every line is visible, and the caret
        // and selection carry the line's vertical position instead.
        float caret_top = 2.0f;
        float caret_bottom = static_cast<float>(h) - 2.0f;
        float wrap_y = 0.0f;
        if (wraps) {
            DWRITE_TEXT_METRICS wrapped{};
            if (SUCCEEDED(dw_layout->GetMetrics(&wrapped)))
                wrap_y = std::round((static_cast<float>(h) - wrapped.height) * 0.5f) - wrapped.top;
            float line_y = 0.0f, line_h = 0.0f;
            DwCaretPoint(dw_layout.Get(), caret, static_cast<int>(text.size()), caret_x, line_y, line_h);
            caret_top = wrap_y + line_y + 1.0f;
            caret_bottom = wrap_y + line_y + std::max(2.0f, line_h) - 1.0f;
            edit_painted[hwnd] = {dw_layout, pad, wrap_y};
            // The EDIT's own caret still tracks its one unwrapped line; keep
            // it (IME and accessibility follow it) on the painted caret.
            if (GetFocus() == hwnd)
                SetCaretPos(static_cast<int>(std::floor(pad + caret_x)), static_cast<int>(caret_top));
        }
        const auto painted_scroll = edit_scrolls.find(hwnd);
        // Keep the origin hit-tested by the last mouse event through release.
        // Only a later caret/text change may chase a rounded edge cluster.
        const float scroll = wraps ? 0.0f : mouse_scroll_hwnd == hwnd && painted_scroll != edit_scrolls.end()
            ? painted_scroll->second : EditScroll(hwnd, dw_layout.Get(), caret_x, static_cast<float>(w));
        edit_scrolls[hwnd] = scroll;
        const uint64_t now = EditPresentationTime(hwnd);
        BOOL system_motion = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &system_motion, 0);
#ifdef PULSE_TEST_EDIT_VIEWPORT
        if (const auto policy = reinterpret_cast<ULONG_PTR>(GetPropW(hwnd, L"Pulse.Test.EditMotion")))
            system_motion = policy == 1;
#endif
        const bool animate = system_motion && !IsHighContrast() && GetFocus() == hwnd &&
            sel_lo == sel_hi && !GetPropW(hwnd, L"Pulse.EditComposing");
        auto& motion = edit_motion[hwnd];
        if (motion.width != w || motion.height != h || motion.format != format) motion = {};
        auto& caret_blink = edit_blinks[hwnd];
        const std::array<float, 3> caret_target{std::floor(pad + caret_x - scroll), caret_top, caret_bottom};
        if (!motion.caret.initialized || motion.text != text || motion.caret.target != caret_target)
            caret_blink.Reset(now);
        motion.change.Sample(now);
        if (motion.text != text) {
            if (animate && motion.layout) {
                const float carried = motion.change.pending ? motion.suffix_dx * (1 - motion.change.progress) : 0;
                motion.change_layout = motion.layout;
                motion.change_scroll = motion.scroll;
                motion.change.Begin(motion.text, text, now);
                auto& change = motion.change;
                // Expand the changed span to shaped cluster boundaries (IME,
                // combining marks and ligatures are never split by clipping).
                const auto cluster = [](IDWriteTextLayout* layout, size_t index) {
                    float x = 0, y = 0;
                    DWRITE_HIT_TEST_METRICS hit{};
                    layout->HitTestTextPosition(static_cast<UINT32>(index), FALSE, &x, &y, &hit);
                    return hit;
                };
                change.prefix = std::min({change.prefix,
                    change.prefix < text.size() ? static_cast<size_t>(cluster(dw_layout.Get(), change.prefix).textPosition) : change.prefix,
                    change.prefix < motion.text.size() ? static_cast<size_t>(cluster(motion.layout.Get(), change.prefix).textPosition) : change.prefix});
                const auto old_hit = cluster(motion.layout.Get(), change.old_end);
                const auto new_hit = cluster(dw_layout.Get(), change.new_end);
                const size_t extra = std::max(
                    change.old_end < motion.text.size() && change.old_end > old_hit.textPosition ? old_hit.textPosition + old_hit.length - change.old_end : size_t{0},
                    change.new_end < text.size() && change.new_end > new_hit.textPosition ? new_hit.textPosition + new_hit.length - change.new_end : size_t{0});
                change.old_end = std::min(motion.text.size(), change.old_end + extra);
                change.new_end = std::min(text.size(), change.new_end + extra);
                motion.suffix_dx = DwCaretX(motion.layout.Get(), static_cast<int>(change.old_end),
                    static_cast<int>(motion.text.size())) - motion.scroll + carried -
                    (caret_origin(static_cast<int>(change.new_end)) - scroll);
                DWRITE_TEXT_METRICS metrics{};
                dw_layout->GetMetrics(&metrics);
                // Keep complex bidirectional runs in their shaped positions.
                if (wraps) motion.suffix_dx = 0;
                if (metrics.maxBidiReorderingDepth > 1) motion.change = {};
            } else motion.change = {};
        }
        if (!animate) motion.change = {};
        motion.caret.Tick(caret_target, now, animate);
        motion.text = text; motion.layout = dw_layout; motion.scroll = scroll;
        motion.format = format; motion.width = w; motion.height = h;
        const bool motion_pending = motion.caret.pending || motion.change.pending;
        caret_blink.Sample(now, motion_pending);
        if (motion_pending) SetTimer(hwnd, kEditMotionTimer, 16, nullptr);
        else KillTimer(hwnd, kEditMotionTimer);
        if (motion_pending) SetPropW(hwnd, L"Pulse.EditMotionPending", reinterpret_cast<HANDLE>(1));
        else RemovePropW(hwnd, L"Pulse.EditMotionPending");
#ifdef PULSE_TEST_EDIT_VIEWPORT
        observed_edit_scroll[hwnd] = scroll;
#endif

        blit_dc->SetTarget(blit_target.Get());
        blit_dc->BeginDraw();
        blit_dc->Clear(background);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        blit_dc->CreateSolidColorBrush(background, &brush);
        bool selection_drawn = sel_hi <= sel_lo || GetFocus() != hwnd;
        if (sel_hi > sel_lo && GetFocus() == hwnd && brush) {
            brush->SetColor(D2D1::ColorF(0.0f, 0.47f, 0.83f, 0.35f));
            UINT32 count = 0;
            dw_layout->HitTestTextRange(static_cast<UINT32>(sel_lo), static_cast<UINT32>(sel_hi - sel_lo),
                pad - scroll, wrap_y, nullptr, 0, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
            if (count && SUCCEEDED(dw_layout->HitTestTextRange(static_cast<UINT32>(sel_lo),
                static_cast<UINT32>(sel_hi - sel_lo), pad - scroll, wrap_y, ranges.data(), count, &count))) {
                for (const auto& range : ranges)
                    blit_dc->FillRectangle(wraps
                        ? D2D1::RectF(range.left, range.top, range.left + range.width, range.top + range.height)
                        : D2D1::RectF(range.left, 1.0f, range.left + range.width, static_cast<float>(h) - 1.0f),
                        brush.Get());
                selection_drawn = true;
            }
        }
        std::wstring visible = text;
        D2D1_COLOR_F visible_color = foreground;
        if (visible.empty()) {
            wchar_t cue[256]{};
            if (SendMessageW(hwnd, EM_GETCUEBANNER, reinterpret_cast<WPARAM>(cue),
                             ARRAYSIZE(cue)) && cue[0] != 0) {
                visible = cue;
                visible_color = D2D1::ColorF(foreground.r, foreground.g, foreground.b,
                                             foreground.a * 0.45f);
            }
        }
        bool text_drawn = visible.empty();
        if (!visible.empty()) {
            // Same layout, params and grayscale AA as the surrounding UI text.
            const auto visible_layout = visible == text && dw_layout
                ? dw_layout : DwEditLayout(visible, format, static_cast<float>(h));
            if (visible_layout && brush) {
                DWRITE_TEXT_METRICS metrics{};
                if (FAILED(visible_layout->GetMetrics(&metrics))) {
                    blit_dc->EndDraw();
                    blit_dc->SetTarget(nullptr);
                    return false;
                }
                const float y = std::round((static_cast<float>(h) - metrics.height) * 0.5f) - metrics.top;
                brush->SetColor(visible_color);
                blit_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                blit_dc->SetTextRenderingParams(params);
                const auto origin=D2D1::Point2F(std::round(pad-scroll),y);
                const auto draw_text = [&](float opacity, float dx, float dy) {
                    auto color = visible_color; color.a *= opacity;
                    brush->SetColor(color);
                    const auto point = D2D1::Point2F(origin.x + dx, origin.y + dy);
                    const bool luma = typography::UseLumaTextForUi() &&
                        DrawLayoutOn(edit_renderer, blit_dc.Get(), point, visible_layout.Get(), color, false);
                    if (luma) ++stats.edit_lumatext_draws;
                    else blit_dc->DrawTextLayout(point, visible_layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                };
                if (motion.change.pending && visible == text) {
                    const auto range = [&](size_t start, size_t end, float opacity, float dx, float dy, bool suffix = false) {
                        if (end <= start || opacity <= 0) return;
                        UINT32 count = 0;
                        visible_layout->HitTestTextRange(static_cast<UINT32>(start), static_cast<UINT32>(end - start),
                            origin.x + dx, origin.y + dy, nullptr, 0, &count);
                        std::vector<DWRITE_HIT_TEST_METRICS> regions(count);
                        if (count && SUCCEEDED(visible_layout->HitTestTextRange(static_cast<UINT32>(start),
                            static_cast<UINT32>(end - start), origin.x + dx, origin.y + dy,
                            regions.data(), count, &count))) {
                            for (const auto& region : regions) {
                                float shift_x = dx, shift_y = dy;
                                float range_opacity = opacity;
                                if (wraps && suffix && motion.change_layout) {
                                    const size_t old_position = motion.change.old_end + region.textPosition - motion.change.new_end;
                                    float old_x = 0, old_y = 0;
                                    DWRITE_HIT_TEST_METRICS old_hit{};
                                    motion.change_layout->HitTestTextPosition(static_cast<UINT32>(old_position),
                                        FALSE, &old_x, &old_y, &old_hit);
                                    DWRITE_TEXT_METRICS old_metrics{};
                                    motion.change_layout->GetMetrics(&old_metrics);
                                    const float old_origin_y = std::round((static_cast<float>(h) - old_metrics.height) * 0.5f) - old_metrics.top;
                                    shift_x = (pad - motion.change_scroll + old_x - region.left) * (1 - motion.change.progress);
                                    shift_y = (old_origin_y + old_y - region.top) * (1 - motion.change.progress);
                                    if (std::abs(old_origin_y + old_y - region.top) > region.height * 0.5f) {
                                        // Crossing a line uses a reveal rather than flying through
                                        // the unchanged text between the old and new line.
                                        shift_x = shift_y = 0;
                                        range_opacity *= motion.change.progress;
                                    }
                                }
                                auto clip = D2D1::RectF(region.left + shift_x - dx, region.top + shift_y - dy,
                                    region.left + region.width + shift_x - dx, region.top + region.height + shift_y - dy);
                                if (!suffix && start == motion.change.prefix && end == motion.change.new_end &&
                                    end < text.size() && motion.change_layout) {
                                    float old_x = 0, old_y = 0, new_x = 0, new_y = 0;
                                    DWRITE_HIT_TEST_METRICS old_hit{}, new_hit{};
                                    motion.change_layout->HitTestTextPosition(static_cast<UINT32>(motion.change.old_end),
                                        FALSE, &old_x, &old_y, &old_hit);
                                    visible_layout->HitTestTextPosition(static_cast<UINT32>(end), FALSE, &new_x, &new_y, &new_hit);
                                    if (std::abs(region.top - dy - origin.y - new_y) < 1 && std::abs(old_y - new_y) < 1) {
                                        const float tail_delta = wraps ? old_x - new_x + scroll - motion.change_scroll : motion.suffix_dx;
                                        const float tail = origin.x + new_x + tail_delta * (1 - motion.change.progress);
                                        clip.right = std::min(clip.right, tail);
                                    }
                                }
                                if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
                                if (clip.right < 0 || clip.left > w || clip.bottom < 0 || clip.top > h) continue;
                                blit_dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                                draw_text(range_opacity, shift_x, shift_y);
                                blit_dc->PopAxisAlignedClip();
                            }
                        }
                    };
                    const auto& change = motion.change;
                    range(0, change.prefix, 1, 0, 0);
                    range(change.prefix, change.new_end, change.progress, 0, (1 - change.progress) * 1.5f);
                    range(change.new_end, text.size(), 1, motion.suffix_dx * (1 - change.progress), 0, true);
                } else draw_text(1, 0, 0);
                blit_dc->SetTextRenderingParams(nullptr);
                text_drawn = true;
            }
        }
        UINT blink = GetCaretBlinkTime();
#ifdef PULSE_TEST_EDIT_VIEWPORT
        if (const auto interval = reinterpret_cast<ULONG_PTR>(GetPropW(hwnd, L"Pulse.Test.EditBlinkInterval")))
            blink = static_cast<UINT>(interval);
#endif
        const bool caret_on = GetFocus() == hwnd && GetCapture() != hwnd &&
            caret_blink.Visible(now, blink);
        const UINT blink_delay = caret_blink.NextDelay(now, blink);
        if (GetFocus() == hwnd && blink_delay) SetTimer(hwnd, kEditCaretTimer, blink_delay, nullptr);
        else KillTimer(hwnd, kEditCaretTimer);
        if (caret_on && brush) {
            brush->SetColor(foreground);
            // Fill whole pixels: a half-pixel origin splits the caret into two
            // half-intensity columns that blend into the first placeholder glyph.
            DWORD caret_width = 1;
            SystemParametersInfoW(SPI_GETCARETWIDTH, 0, &caret_width, 0);
            const float x = std::floor(motion.caret.value[0]);
            blit_dc->FillRectangle(
                D2D1::RectF(x, motion.caret.value[1], x + static_cast<float>(std::max<DWORD>(1, caret_width)),
                            motion.caret.value[2]),
                brush.Get());
        }
        const HRESULT end = blit_dc->EndDraw();
        blit_dc->SetTarget(nullptr);
        if (FAILED(end) || !text_drawn || !selection_drawn || !brush) return false;
        ++stats.edit_directwrite_draws;
        stats.edit_rendering_mode = params->GetRenderingMode();
        bool presented = true;
        if (bitmap) {
            *bitmap = blit_target.Get();
            (*bitmap)->AddRef();
        } else {
            if (FAILED(blit_staging->CopyFromBitmap(nullptr, blit_target.Get(), nullptr))) {
                return false;
            }
            D2D1_MAPPED_RECT mapped{};
            if (FAILED(blit_staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
            const size_t row_bytes = static_cast<size_t>(w) * 4u;
            const void* dib_bits = mapped.bits;
            std::vector<std::uint8_t> tight_pixels;
            if (mapped.pitch != row_bytes) {
                tight_pixels.resize(row_bytes * static_cast<size_t>(h));
                const auto* source = static_cast<const std::uint8_t*>(mapped.bits);
                for (int row = 0; row < h; ++row) {
                    std::memcpy(tight_pixels.data() + static_cast<size_t>(row) * row_bytes,
                                source + static_cast<size_t>(row) * mapped.pitch,
                                row_bytes);
                }
                dib_bits = tight_pixels.data();
            }
            presented = PresentEditBits(hwnd, hdc, dib_bits, w, h, background.a < 1.0f);
            blit_staging->Unmap();
        }

        HIMC imc = ImmGetContext(hwnd);
        if (imc) {
            COMPOSITIONFORM form{};
            form.dwStyle = CFS_POINT;
            form.ptCurrentPos = {static_cast<LONG>(pad + caret_x - scroll),
                                 wraps ? static_cast<LONG>(caret_top - 1.0f) : 0};
            ImmSetCompositionWindow(imc, &form);
            // The EDIT cannot answer IMR_QUERYCHARPOSITION, so without this an
            // IME guesses where candidates go; past the end of a long name it
            // put them at the top-left of the window (#188).
            CANDIDATEFORM candidate{};
            candidate.dwIndex = 0;
            candidate.dwStyle = CFS_EXCLUDE;
            candidate.ptCurrentPos = form.ptCurrentPos;
            candidate.rcArea = {form.ptCurrentPos.x, wraps ? form.ptCurrentPos.y : 0, form.ptCurrentPos.x + 1,
                                wraps ? static_cast<LONG>(std::ceil(caret_bottom + 1.0f)) : h};
            ImmSetCandidateWindow(imc, &candidate);
            ImmReleaseContext(hwnd, imc);
        }
        return presented;
    }
#else
    bool Init(IDWriteFactory*, ID2D1RenderTarget*) { return false; }
    void Shutdown() noexcept {}
#endif
};

LumaTextRenderer::LumaTextRenderer() : impl_(std::make_unique<Impl>()) {}
LumaTextRenderer::~LumaTextRenderer() { Shutdown(); }

void LumaTextRenderer::TrackEdit(HWND hwnd) {
    if (!hwnd || std::find(tracked_edits_.begin(), tracked_edits_.end(), hwnd) != tracked_edits_.end()) return;
    if (SetWindowSubclass(hwnd, EditSelectionProc, reinterpret_cast<UINT_PTR>(this),
        reinterpret_cast<DWORD_PTR>(this))) tracked_edits_.push_back(hwnd);
}
LRESULT CALLBACK LumaTextRenderer::EditSelectionProc(HWND hwnd, UINT message, WPARAM wparam,
                                                    LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<LumaTextRenderer*>(data);
    if (message == WM_TIMER && wparam == kEditMotionTimer) {
        if (GetFocus() == hwnd && IsWindowVisible(hwnd) && GetPropW(hwnd, L"Pulse.EditMotionPending") &&
            !GetPropW(hwnd, L"Pulse.NativeEditFallback") && !GetPropW(hwnd, L"Pulse.NativeEditPolicy"))
            InvalidateRect(hwnd, nullptr, FALSE);
        else {
            KillTimer(hwnd, kEditMotionTimer);
            RemovePropW(hwnd, L"Pulse.EditMotionPending");
#if defined(PULSE_HAS_LUMATEXT)
            if (self->impl_) self->impl_->edit_motion.erase(hwnd);
#endif
        }
        return 0;
    }
    if (message == WM_IME_STARTCOMPOSITION) SetPropW(hwnd, L"Pulse.EditComposing", reinterpret_cast<HANDLE>(1));
    if (message == WM_IME_ENDCOMPOSITION || message == WM_KILLFOCUS || message == WM_NCDESTROY)
        RemovePropW(hwnd, L"Pulse.EditComposing");
#if defined(PULSE_HAS_LUMATEXT)
    if (self->impl_ && self->impl_->DeferredSelection(hwnd)) {
        // Input and focus changes act on the dragged selection the user sees;
        // an explicit EM_SETSEL replaces it.
        switch (message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_IME_STARTCOMPOSITION:
        case WM_KILLFOCUS: case WM_SETTEXT: case EM_REPLACESEL: case WM_CUT: case WM_COPY:
        case WM_PASTE: case WM_CLEAR: case WM_UNDO: case EM_UNDO:
            self->impl_->CommitMouseSelection(hwnd);
            break;
        case EM_SETSEL:
            if (!self->impl_->setting_mouse_selection) self->impl_->mouse_selection_deferred = false;
            break;
        default:
            break;
        }
    }
    if (message == WM_KEYDOWN && (wparam == VK_UP || wparam == VK_DOWN) && self->impl_ &&
        (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
        self->impl_->MoveEditLine(hwnd, wparam == VK_DOWN, (GetKeyState(VK_SHIFT) & 0x8000) != 0))
        return 0;
#endif
    const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
    self->SyncEditSelection(hwnd, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditSelectionProc, id);
        std::erase(self->tracked_edits_, hwnd);
    }
    return result;
}
void LumaTextRenderer::SyncEditSelection(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
#if defined(PULSE_HAS_LUMATEXT)
    if (!impl_) return;
    if (!impl_->applying_mouse_selection && !impl_->setting_mouse_selection &&
        GetFocus() == hwnd && IsEditCaretActivity(message))
        impl_->edit_blinks[hwnd].Reset(EditPresentationTime(hwnd));
    if (message == WM_SETTEXT || message == WM_SIZE || message == WM_SETFONT ||
        message == WM_IME_STARTCOMPOSITION || message == WM_KILLFOCUS || message == WM_NCDESTROY) {
        impl_->edit_motion.erase(hwnd);
        KillTimer(hwnd, kEditMotionTimer);
        RemovePropW(hwnd, L"Pulse.EditMotionPending");
    }
    if (message == WM_NCDESTROY || message == WM_KILLFOCUS) {
        impl_->edit_blinks.erase(hwnd);
        KillTimer(hwnd, kEditCaretTimer);
        if (impl_->mouse_scroll_hwnd == hwnd) impl_->mouse_scroll_hwnd = nullptr;
        if (impl_->mouse_hwnd == hwnd) {
            impl_->mouse_hwnd = nullptr;
            impl_->mouse_selection_deferred = false;
        }
        if (message == WM_NCDESTROY) {
            impl_->edit_selections.erase(hwnd);
            impl_->edit_scrolls.erase(hwnd);
            impl_->edit_painted.erase(hwnd);
        }
        return;
    }
    // New text starts at the left edge, as native EDIT does.
    if (message == WM_SETTEXT) impl_->edit_scrolls.erase(hwnd);
    if (impl_->applying_mouse_selection || impl_->setting_mouse_selection) return;
    if (message != EM_SETSEL && message != WM_KEYDOWN && message != WM_CHAR &&
        message != WM_SETTEXT && message != EM_REPLACESEL && message != WM_CUT &&
        message != WM_PASTE && message != WM_CLEAR && message != WM_UNDO && message != EM_UNDO &&
        message != WM_IME_COMPOSITION && message != WM_IME_ENDCOMPOSITION) return;
    if (impl_->mouse_scroll_hwnd == hwnd) impl_->mouse_scroll_hwnd = nullptr;
    DWORD lo = 0, hi = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
    int anchor = static_cast<int>(lo), caret = static_cast<int>(hi);
    if (message == EM_SETSEL && static_cast<int>(wparam) >= 0) {
        const int length = GetWindowTextLengthW(hwnd);
        anchor = std::clamp(static_cast<int>(wparam), 0, length);
        caret = lparam < 0 ? length : std::clamp(static_cast<int>(lparam), 0, length);
    } else if (lo != hi) {
        if (message == WM_KEYDOWN && (wparam == VK_LEFT || wparam == VK_HOME || wparam == VK_UP)) {
            anchor = static_cast<int>(hi); caret = static_cast<int>(lo);
        } else if (impl_->mouse_hwnd == hwnd && impl_->mouse_anchor == static_cast<int>(hi)) {
            anchor = static_cast<int>(hi); caret = static_cast<int>(lo);
        }
    }
    impl_->mouse_hwnd = hwnd; impl_->mouse_anchor = anchor; impl_->mouse_caret = caret;
    impl_->edit_selections[hwnd] = {anchor, caret};
#else
    (void)hwnd; (void)message; (void)wparam; (void)lparam;
#endif
}

void LumaTextRenderer::SynchronizeEditSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
#if defined(PULSE_HAS_LUMATEXT)
    if (!impl_ || impl_->setting_mouse_selection) return;
    if (GetFocus() == hwnd && IsEditCaretActivity(msg))
        impl_->edit_blinks[hwnd].Reset(EditPresentationTime(hwnd));
    if (impl_->mouse_scroll_hwnd == hwnd) impl_->mouse_scroll_hwnd = nullptr;
    if (msg == WM_NCDESTROY || msg == WM_KILLFOCUS) {
        impl_->edit_blinks.erase(hwnd);
        KillTimer(hwnd, kEditCaretTimer);
        if (impl_->mouse_hwnd == hwnd) impl_->mouse_hwnd = nullptr;
        return;
    }
    DWORD lo = 0, hi = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&lo), reinterpret_cast<LPARAM>(&hi));
    int anchor = static_cast<int>(lo), caret = static_cast<int>(hi);
    if (msg == EM_SETSEL && static_cast<int>(wParam) >= 0) {
        const int length = GetWindowTextLengthW(hwnd);
        anchor = std::min(static_cast<int>(wParam), length);
        caret = static_cast<int>(lParam) < 0 ? length : std::min(static_cast<int>(lParam), length);
    } else if (lo != hi && impl_->mouse_hwnd == hwnd && impl_->mouse_anchor == static_cast<int>(hi)) {
        anchor = static_cast<int>(hi);
        caret = static_cast<int>(lo);
    }
    impl_->mouse_hwnd = hwnd;
    impl_->mouse_anchor = anchor;
    impl_->mouse_caret = caret;
#else
    (void)hwnd; (void)msg; (void)wParam; (void)lParam;
#endif
}

LRESULT LumaTextRenderer::CallEditDefaultMouse(HWND hwnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam, IDWriteTextFormat* format) {
#if defined(PULSE_HAS_LUMATEXT)
    if (format && impl_ && impl_->renderer) {
        TrackEdit(hwnd);
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd)
            impl_->edit_blinks[hwnd].Reset(EditPresentationTime(hwnd));
        struct MouseSelectionGuard {
            Impl& state;
            HWND window;
            MouseSelectionGuard(Impl& value, HWND hwnd) : state(value), window(hwnd) {
                state.applying_mouse_selection = true;
            }
            ~MouseSelectionGuard() {
                state.applying_mouse_selection = false;
                if (state.mouse_hwnd == window && IsWindow(window))
                    state.edit_selections[window] = {state.mouse_anchor, state.mouse_caret};
            }
        } guard(*impl_, hwnd);
        HideCaret(hwnd);
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            impl_->ApplyMouseSelection(hwnd, msg, wParam, lParam, format);
            if (GetFocus() != hwnd) SetFocus(hwnd);
            HideCaret(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (GetCapture() == hwnd)
                impl_->ApplyMouseSelection(hwnd, msg, wParam, lParam, format);
            return 0;
        case WM_LBUTTONUP:
            impl_->CommitMouseSelection(hwnd);
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            impl_->CommitMouseSelection(hwnd);
            return 0;
        default:
            break;
        }
    }
#endif
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool LumaTextRenderer::Init(IDWriteFactory* dwrite, ID2D1RenderTarget* target) {
    Shutdown();
    if (!EnvironmentEnabled() || !dwrite || !target || !LoadOptionalLumaText()) return false;
    if (impl_->Init(dwrite, target)) return true;
    // The renderer is allocated before fonts and cascades. A late
    // failure must not leave Enabled() true and suppress native EDIT painting.
    Shutdown();
    return false;
}

void LumaTextRenderer::Shutdown() noexcept {
    for (HWND hwnd : tracked_edits_) {
        KillTimer(hwnd, kEditCaretTimer);
        KillTimer(hwnd, kEditMotionTimer);
        RemovePropW(hwnd, L"Pulse.EditMotionPending");
        RemovePropW(hwnd, L"Pulse.EditComposing");
        RemoveWindowSubclass(hwnd, EditSelectionProc, reinterpret_cast<UINT_PTR>(this));
    }
    tracked_edits_.clear();
#if defined(PULSE_HAS_LUMATEXT)
    if (impl_) {
        impl_->edit_selections.clear();
        impl_->edit_scrolls.clear();
        impl_->edit_motion.clear();
        impl_->edit_blinks.clear();
        impl_->mouse_hwnd = nullptr;
        impl_->mouse_scroll_hwnd = nullptr;
        impl_->mouse_selection_deferred = false;
    }
#endif
    if (impl_) impl_->Shutdown();
}

bool LumaTextRenderer::Enabled() const noexcept {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && static_cast<bool>(impl_->renderer);
#else
    return false;
#endif
}

bool LumaTextRenderer::Draw(std::wstring_view text, IDWriteTextFormat* format,
                            const D2D1_RECT_F& bounds,
                            const D2D1_COLOR_F& foreground,
                            const D2D1_COLOR_F& background,
                            DWRITE_TEXT_ALIGNMENT alignment) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->Draw(text, format, bounds, foreground, background, alignment);
#else
    (void)text;
    (void)format;
    (void)bounds;
    (void)foreground;
    (void)background;
    (void)alignment;
    return false;
#endif
}

bool LumaTextRenderer::DrawLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                                  const D2D1_COLOR_F& foreground, bool clip) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->DrawLayout(origin, layout, foreground, clip);
#else
    (void)origin;
    (void)layout;
    (void)foreground;
    (void)clip;
    return false;
#endif
}

bool LumaTextRenderer::DrawSplit(std::wstring_view text, IDWriteTextFormat* format,
                                 const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                                 std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                                 const D2D1_COLOR_F& background) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->DrawSplit(text, format, bounds, foreground, split,
                                     split_foreground, background);
#else
    (void)text;
    (void)format;
    (void)bounds;
    (void)foreground;
    (void)split;
    (void)split_foreground;
    (void)background;
    return false;
#endif
}

bool LumaTextRenderer::Measure(std::wstring_view text, IDWriteTextFormat* format,
                               float& width, float* height) {
#if defined(PULSE_HAS_LUMATEXT)
    return impl_ && impl_->Measure(text, format, width, height);
#else
    (void)text;
    (void)format;
    width = 0.0f;
    if (height) *height = 0.0f;
    return false;
#endif
}

bool LumaTextRenderer::PaintEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                                 const D2D1_COLOR_F& foreground,
                                 const D2D1_COLOR_F& background) {
#if defined(PULSE_HAS_LUMATEXT)
    TrackEdit(hwnd);
    return impl_ && impl_->PaintEdit(hwnd, hdc, format, foreground, background);
#else
    (void)hwnd;
    (void)hdc;
    (void)format;
    (void)foreground;
    (void)background;
    return false;
#endif
}

bool LumaTextRenderer::PaintEditBitmap(HWND hwnd, IDWriteTextFormat* format,
                                      const D2D1_COLOR_F& foreground,
                                      const D2D1_COLOR_F& background, ID2D1Bitmap1** bitmap) {
    if (!bitmap) return false;
    *bitmap = nullptr;
#if defined(PULSE_HAS_LUMATEXT)
    TrackEdit(hwnd);
    return impl_ && impl_->PaintEdit(hwnd, nullptr, format, foreground, background, bitmap);
#else
    (void)hwnd; (void)format; (void)foreground; (void)background;
    return false;
#endif
}

const LumaTextStats& LumaTextRenderer::Stats() const noexcept {
    return impl_->stats;
}

void LumaTextRenderer::RecordFallback() noexcept {
    impl_->stats.fallback_draws++;
}

} // namespace pulse::ui
