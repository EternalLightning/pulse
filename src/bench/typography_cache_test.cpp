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
}
Compositor::~Compositor() = default;
bool Compositor::LumaTextEnabled() const noexcept { return luma_enabled; }
bool Compositor::MeasureLumaText(std::wstring_view, IDWriteTextFormat*, float& width, float*) {
    ++luma_calls;
    width = luma_width;
    return luma_enabled;
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
}

int main() {
    using namespace pulse::ui;
    namespace type = pulse::ui::typography;
    Compositor compositor;
    IDWriteFactory2* factory = compositor.DwriteFactory();
    ComPtr<IDWriteTextFormat> format;
    if (!factory || FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            14.0f, L"en-US", &format)) || !format.get()) {
        Check(false, "real DirectWrite factory/format created");
        return 1;
    }
    Check(true, "real DirectWrite factory/format created without a window");
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
