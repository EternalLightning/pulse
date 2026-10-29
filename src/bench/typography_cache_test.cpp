// Standalone white-box test: compile this file only, not typography.cpp again.
#include "../ui/lumatext_renderer.h"
#include "../ui/typography.cpp"

#include <iostream>
#include <thread>
#include <vector>

namespace {
int failures = 0;
bool luma_enabled = false;
float luma_width = 0.0f;
int luma_calls = 0;
void Check(bool value, const char* name) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!value) ++failures;
}
bool Near(float a, float b) { return std::abs(a - b) < 0.001f; }
}

// No window, D2D device, or real Luma renderer is created. DWrite is real;
// this deterministic Luma boundary verifies that its widths stay distinct.
namespace pulse::l10n {
Language effective_language() noexcept { return Language::EnUS; }
const wchar_t* LocaleName() noexcept { return L"en-US"; }
}
namespace pulse::ui {
struct LumaTextRenderer::Impl {};
LumaTextRenderer::~LumaTextRenderer() = default;
Compositor::Compositor() {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory2),
                       reinterpret_cast<IUnknown**>(&dwriteFactory_));
    if (dwriteFactory_.get()) {
        dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-US",
            &fileNameFormat_);
    }
}
Compositor::~Compositor() = default;
bool Compositor::LumaTextEnabled() const noexcept { return luma_enabled && typography::UseLumaTextForUi(); }
bool Compositor::MeasureLumaText(std::wstring_view, IDWriteTextFormat* format, float& width, float*) {
    ++luma_calls;
    if (!typography::UsesLumaText(this, format)) return false;
    width = luma_width;
    return true;
}
}

namespace {
// The deleted renderer implementation, independent of typography's cache.
float LegacyWidth(IDWriteFactory2* factory, IDWriteTextFormat* format, std::wstring_view text) {
    if (!factory || !format || text.empty()) return 0.0f;
    const float fallback = format->GetFontSize() * static_cast<float>(text.size());
    pulse::ui::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                         format, 10000.0f, 100.0f, &layout)) || !layout.get())
        return fallback;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return fallback;
    DWRITE_OVERHANG_METRICS overhang{};
    layout->GetOverhangMetrics(&overhang);
    return metrics.widthIncludingTrailingWhitespace + std::max(0.0f, overhang.right) + 1.0f;
}

void CheckModeLayouts(IDWriteFactory2* factory) {
    using pulse::ui::ComPtr;
    namespace type = pulse::ui::typography;
    for (auto mode : {type::TextRenderMode::Auto, type::TextRenderMode::Sharp, type::TextRenderMode::Smooth}) {
        type::SetTextRenderMode(mode);
        const bool sharp = mode == type::TextRenderMode::Sharp;
        ComPtr<IDWriteRenderingParams2> params;
        Check(SUCCEEDED(type::CreateRenderingParams(factory, nullptr, &params)) && params.get() &&
              params->GetRenderingMode() == (sharp ? DWRITE_RENDERING_MODE_GDI_CLASSIC : DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC) &&
              params->GetGridFitMode() == (sharp ? DWRITE_GRID_FIT_MODE_ENABLED : DWRITE_GRID_FIT_MODE_DISABLED) &&
              type::MeasuringMode() == (sharp ? DWRITE_MEASURING_MODE_GDI_CLASSIC : DWRITE_MEASURING_MODE_NATURAL),
              "selected mode pairs rasterization, grid fit and DrawText measurement");
        bool positions_match = true;
        bool widths_match = true;
        size_t layouts = 0;
        for (const wchar_t* family : {L"Microsoft YaHei UI", L"Segoe UI Variable Text"}) {
            for (float size : {13.0f, 14.0f, 17.5f, 21.0f}) {
                ComPtr<IDWriteTextFormat> format;
                if (FAILED(factory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &format))) {
                    positions_match = false;
                    continue;
                }
                for (const std::wstring text : {L"appcompat", L"Downloaded Program Files", L"Program",
                        L"\u4e2d\u6587\u6587\u4ef6.txt", L"space  ffi AV", L"long-file-name.7z"}) {
                    ComPtr<IDWriteTextLayout> actual, reference;
                    const HRESULT actual_hr = type::CreateTextLayout(factory, text.data(), static_cast<UINT32>(text.size()),
                        format.get(), 10000.0f, 100.0f, &actual);
                    const HRESULT expected_hr = sharp
                        ? factory->CreateGdiCompatibleTextLayout(text.data(), static_cast<UINT32>(text.size()),
                            format.get(), 10000.0f, 100.0f, 1.0f, nullptr, FALSE, &reference)
                        : factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                            format.get(), 10000.0f, 100.0f, &reference);
                    if (FAILED(actual_hr) || FAILED(expected_hr) || !actual.get() || !reference.get()) {
                        positions_match = false;
                        continue;
                    }
                    ++layouts;
                    for (UINT32 i = 0; i <= text.size(); ++i) {
                        FLOAT x = 0, y = 0, ref_x = 0, ref_y = 0;
                        DWRITE_HIT_TEST_METRICS hit{}, ref_hit{};
                        actual->HitTestTextPosition(i, FALSE, &x, &y, &hit);
                        reference->HitTestTextPosition(i, FALSE, &ref_x, &ref_y, &ref_hit);
                        positions_match = positions_match && Near(x, ref_x) && Near(y, ref_y) &&
                            Near(hit.width, ref_hit.width) && hit.textPosition == ref_hit.textPosition;
                    }
                    DWRITE_TEXT_METRICS metrics{};
                    DWRITE_OVERHANG_METRICS overhang{};
                    reference->GetMetrics(&metrics);
                    reference->GetOverhangMetrics(&overhang);
                    widths_match = widths_match && Near(type::MeasureAdvance(factory, format.get(), text),
                        metrics.widthIncludingTrailingWhitespace + std::max(0.0f, overhang.right) + 1.0f);
                    // Width cache keys include format identity. Clear before releasing a format,
                    // as Compositor::RecreateTextFormats does in production.
                }
                type::ClearMeasurementCache();
            }
        }
        Check(positions_match && layouts == 48, "48 Latin/CJK layouts match native reference caret positions at 100/125/150 percent");
        Check(widths_match, "fitting widths match the selected layout including right overhang");
        ComPtr<IDWriteTextFormat> dip_format;
        ComPtr<IDWriteTextLayout> dip_layout, dip_reference;
        factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &dip_format);
        const HRESULT dip_result = type::CreateTextLayout(factory, L"Downloaded Program Files", 24,
            dip_format.get(), 1000.0f, 100.0f, &dip_layout, 1.25f);
        if (sharp) factory->CreateGdiCompatibleTextLayout(L"Downloaded Program Files", 24,
            dip_format.get(), 1000.0f, 100.0f, 1.25f, nullptr, FALSE, &dip_reference);
        else factory->CreateTextLayout(L"Downloaded Program Files", 24,
            dip_format.get(), 1000.0f, 100.0f, &dip_reference);
        DWRITE_TEXT_METRICS dip_metrics{}, dip_expected{};
        const bool dip_ok = SUCCEEDED(dip_result) && dip_layout.get() && dip_reference.get() &&
            SUCCEEDED(dip_layout->GetMetrics(&dip_metrics)) && SUCCEEDED(dip_reference->GetMetrics(&dip_expected));
        Check(dip_ok && Near(dip_metrics.width, dip_expected.width),
            "DIP-coordinate floating search layouts use their actual pixels per DIP");
    }
    type::SetTextRenderMode(type::TextRenderMode::Auto);
}
}

int main() {
    using namespace pulse::ui;
    namespace type = pulse::ui::typography;
    Compositor compositor;
    IDWriteFactory2* factory = compositor.DwriteFactory();
    ComPtr<IDWriteTextFormat> format;
    if (!factory || !compositor.FileNameFormat() || FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"en-US", &format)) || !format.get()) {
        Check(false, "real DirectWrite factory/format created");
        return 1;
    }
    Check(true, "real DirectWrite factory/format created without a window");
    CheckModeLayouts(factory);
    const std::vector<std::wstring> cases{
        L"", L"Pulse", L"trailing  ", L"\u4f60\u597d\u6587\u4ef6.txt", L"italic ffi AV",
        L"line one\nline two", L"one\ttwo", std::wstring(1100, L'W')};
    bool equivalent = true;
    for (const auto& text : cases) {
        const float expected = LegacyWidth(factory, format.get(), text);
        equivalent = equivalent && Near(type::MeasureAdvance(factory, format.get(), text), expected) &&
            Near(type::MeasureLine(&compositor, format.get(), text), expected);
    }
    Check(equivalent, "advance and Luma-off line equal old renderer formula including CJK/whitespace/long text");
    type::ClearMeasurementCache();
    const std::wstring label = L"same input";
    const float advance = type::MeasureAdvance(factory, format.get(), label);
    const size_t entries = type::CurrentMeasureCache().widths.size();
    Check(entries == 1 && Near(type::MeasureLine(&compositor, format.get(), label), advance) &&
          type::CurrentMeasureCache().widths.size() == entries,
          "Luma-off line and direct advance share one cached entry");
    Compositor other_compositor;
    type::MeasureAdvance(other_compositor.DwriteFactory(), format.get(), label);
    Check(type::CurrentMeasureCache().widths.size() == 2,
          "distinct DirectWrite factories cannot share an advance cache entry");
    type::ClearMeasurementCache();
    type::MeasureAdvance(factory, format.get(), label);
    luma_enabled = true;
    luma_width = advance + 20.0f;
    const float line = type::MeasureLine(&compositor, format.get(), label);
    const int measured_calls = luma_calls;
    Check(Near(line, luma_width + type::InkPad(format.get())) &&
          Near(type::MeasureAdvance(factory, format.get(), label), advance) &&
          type::CurrentMeasureCache().widths.size() == 2 &&
          Near(type::MeasureLine(&compositor, format.get(), label), line) && luma_calls == measured_calls,
          "Luma composite width stays separate and hits cache without altering advance");
    Check(type::UsesLumaText(&compositor, format.get()) &&
          type::UsesLumaText(&compositor, compositor.FileNameFormat()) &&
          !type::UsesLumaText(nullptr, format.get()) && !type::UsesLumaText(&compositor, nullptr),
          "Auto routes valid filename and UI formats through Luma");
    type::SetTextRenderMode(type::TextRenderMode::Sharp);
    Check(!type::UsesLumaText(&compositor, format.get()) &&
          !type::UsesLumaText(&compositor, compositor.FileNameFormat()) &&
          type::CurrentMeasureCache().widths.empty(),
          "Sharp bypasses Luma for all formats and invalidates widths");
    IDWriteTextFormat* filename_format = compositor.FileNameFormat();
    const float filename_advance = type::MeasureAdvance(factory, filename_format, label);
    const int filename_calls = luma_calls;
    Check(Near(type::MeasureLine(&compositor, filename_format, label), filename_advance) &&
          type::CurrentMeasureCache().widths.size() == 1 && luma_calls == filename_calls,
          "Sharp filename layout and fitting share compatible advance without Luma");
    type::SetTextRenderMode(type::TextRenderMode::Smooth);
    Check(!type::UsesLumaText(&compositor, format.get()) &&
          Near(type::MeasureAdvance(factory, filename_format, label), LegacyWidth(factory, filename_format, label)),
          "Smooth bypasses Luma and restores natural advances");
    type::SetTextRenderMode(type::TextRenderMode::Auto);
    luma_enabled = false;
    const auto generation = type::Generation();
    type::InvalidateCaches();
    Check(type::Generation() != generation && type::CurrentMeasureCache().widths.empty(),
          "generation invalidation discards both measurement semantics");
    type::MeasureAdvance(factory, format.get(), L"one\ttwo");
    format->SetIncrementalTabStop(90.0f);
    Check(Near(type::MeasureAdvance(factory, format.get(), L"one\ttwo"),
               LegacyWidth(factory, format.get(), L"one\ttwo")) &&
          type::CurrentMeasureCache().widths.size() == 2,
          "in-place tab-stop change is a new key and matches real DWrite");
    format->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 26.0f, 19.0f);
    Check(Near(type::MeasureAdvance(factory, format.get(), L"one\ttwo"),
               LegacyWidth(factory, format.get(), L"one\ttwo")) &&
          type::CurrentMeasureCache().widths.size() == 3,
          "direction/alignment/line-spacing changes cannot reuse old key");
    const auto before_clear_generation = type::Generation();
    type::ClearMeasurementCache();
    Check(type::Generation() == before_clear_generation && type::CurrentMeasureCache().widths.empty(),
          "local device/format clear drops widths without changing generation");
    const auto before_invalid = type::CurrentMeasureCache().widths.size();
    Check(type::MeasureAdvance(nullptr, format.get(), label) == 0.0f &&
          type::MeasureAdvance(factory, nullptr, label) == 0.0f &&
          type::MeasureAdvance(factory, format.get(), L"") == 0.0f &&
          type::CurrentMeasureCache().widths.size() == before_invalid,
          "invalid and empty measurements do not poison cache");
    type::MeasureAdvance(factory, format.get(), std::wstring(1100, L'W'));
    Check(type::CurrentMeasureCache().widths.empty(), "long text preserves result without occupying bounded cache");
    type::MeasureAdvance(factory, format.get(), label);
    bool isolated = false;
    std::thread worker([&] {
        isolated = type::CurrentMeasureCache().widths.empty();
        type::ClearMeasurementCache();
    });
    worker.join();
    Check(isolated && type::CurrentMeasureCache().widths.size() == 1,
          "measurement storage and explicit clear remain thread-local");
    for (size_t i = 1; i <= type::kMeasureCacheLimit; ++i) {
        auto key = type::MakeMeasureKey(factory, nullptr, format.get(), std::to_wstring(i));
        type::RememberWidth(type::CurrentMeasureCache(), std::move(key), static_cast<float>(i));
    }
    Check(type::CurrentMeasureCache().widths.size() <= type::kMeasureCacheLimit,
          "shared width table remains bounded at 16384 entries");
    type::ClearMeasurementCache();
    return failures == 0 ? 0 : 1;
}
