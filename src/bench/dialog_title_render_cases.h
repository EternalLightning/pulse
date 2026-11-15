// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Included inside OperationCenterTestPeer.

static int RunTitleRendering(Compositor& compositor, const std::filesystem::path& output) {
    int failures = 0, checks = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        failures += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto previous_mode = typography::CurrentTextRenderMode();
    HighContrastFrameScope normal_palette(false);
    auto* dc = compositor.Dc();
    const auto read_pixels = [&]() -> std::vector<BYTE> {
        ComPtr<ID2D1Image> image;
        dc->GetTarget(&image);
        ComPtr<ID2D1Bitmap1> target, readable;
        if (!image.get() || FAILED(image->QueryInterface(IID_PPV_ARGS(&target)))) return {};
        const auto size = target->GetPixelSize();
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, target->GetPixelFormat(), 96, 96);
        if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, &readable)) ||
            FAILED(readable->CopyFromBitmap(nullptr, target.get(), nullptr))) return {};
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return {};
        std::vector<BYTE> pixels(static_cast<size_t>(size.width) * size.height * 4);
        for (UINT y = 0; y < size.height; ++y)
            std::memcpy(pixels.data() + static_cast<size_t>(y) * size.width * 4,
                mapped.bits + static_cast<size_t>(y) * mapped.pitch, static_cast<size_t>(size.width) * 4);
        readable->Unmap();
        return pixels;
    };
    for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f}) for (const bool dark : {false, true}) {
        const UINT width = static_cast<UINT>(360 * scale), height = static_cast<UINT>(150 * scale);
        compositor.Resize(width, height);
        const auto theme = MakeTheme(dark, HexColor(0x0078D4));
        const auto title_bounds = D2D1::RectF(20 * scale, 0, 170 * scale, 44 * scale);
        const auto body_bounds = D2D1::RectF(20 * scale, 64 * scale, 340 * scale, 112 * scale);
        const std::wstring title_text = L"新建文本文档 照明警醒 English long caption";
        const std::wstring body_text = L"正文与输入保持清晰 English 0123";
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(theme.text, &brush);
        std::vector<BYTE> sharp_title;
        for (const auto mode : {typography::TextRenderMode::Sharp, typography::TextRenderMode::Smooth,
                                typography::TextRenderMode::Auto}) {
            typography::SetTextRenderMode(mode);
            compositor.RecreateTextFormats(scale);
            check(compositor.DialogTitleFormat()->GetFontWeight() == DWRITE_FONT_WEIGHT_BOLD &&
                compositor.DialogTitleFormat()->GetFontSize() == static_cast<float>(typography::EditFontPixels(scale, 14)),
                "title retains the approved 14 DIP bold face at each DPI");
            ComPtr<IDWriteRenderingParams> before;
            dc->GetTextRenderingParams(&before);
            const auto antialias = dc->GetTextAntialiasMode();
            const auto draw = [&](bool title) {
                dc->BeginDraw();
                dc->Clear(theme.bg);
                if (title) check(compositor.DrawDialogTitle(title_text, title_bounds, theme.text),
                    "shared title draws a long mixed-script caption in narrow bounds");
                dc->DrawTextW(body_text.c_str(), static_cast<UINT32>(body_text.size()), compositor.TextFormat(),
                    body_bounds, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP, typography::MeasuringMode());
                check(SUCCEEDED(dc->EndDraw()), "title and unchanged body frame completes without a D2D error");
                return read_pixels();
            };
            const auto baseline = draw(false);
            std::vector<BYTE> first;
            int frame_index = 0;
            for (const bool high_contrast : {false, true, false}) {
                HighContrastFrameScope contrast(high_contrast);
                const auto calls = compositor.GetLumaTextStats()->draw_calls;
                const auto pixels = draw(true);
                const auto draws = compositor.GetLumaTextStats()->draw_calls - calls;
                check(mode == typography::TextRenderMode::Auto && !high_contrast ? draws == 1 : draws == 0,
                    "Auto keeps LumaText while explicit modes and high contrast retain DirectWrite");
                ComPtr<IDWriteRenderingParams> after;
                dc->GetTextRenderingParams(&after);
                check(after.get() == before.get() && dc->GetTextAntialiasMode() == antialias,
                    "title restores body rendering parameters and antialias mode");
                const size_t header_bytes = static_cast<size_t>(width) * static_cast<UINT>(44 * scale) * 4;
                const bool readable = !pixels.empty() && pixels.size() == baseline.size();
                check(readable, "read back the title and body pixels");
                if (!readable) continue;
                check(std::memcmp(pixels.data() + header_bytes, baseline.data() + header_bytes,
                    pixels.size() - header_bytes) == 0, "body pixels remain identical after drawing the title");
                bool ink = false, clipped = true;
                for (UINT y = 0; y < static_cast<UINT>(44 * scale); ++y) for (UINT x = 0; x < width; ++x) {
                    const size_t at = (static_cast<size_t>(y) * width + x) * 4;
                    const bool changed = std::memcmp(pixels.data() + at, baseline.data() + at, 4) != 0;
                    if (x >= title_bounds.left && x < title_bounds.right) ink |= changed;
                    else clipped &= !changed;
                }
                check(ink && clipped, "title ink and ellipsis stay inside the caption bounds");
                if (frame_index == 0) {
                    first = pixels;
                    if (mode == typography::TextRenderMode::Sharp) sharp_title.assign(pixels.begin(), pixels.begin() + header_bytes);
                    if (mode == typography::TextRenderMode::Smooth)
                        check(sharp_title.size() == header_bytes && std::memcmp(sharp_title.data(), pixels.data(), header_bytes) == 0,
                            "Sharp caption matches the approved natural title policy without changing the body mode");
                    if (mode == typography::TextRenderMode::Sharp && scale == 1.0f)
                        check(compositor.SaveSnapshot((output / (dark ? L"title-sharp-dark.png" : L"title-sharp-light.png")).c_str()),
                            "save only the two representative production title frames");
                } else if (frame_index == 1 && mode == typography::TextRenderMode::Sharp && scale == 1.0f)
                    check(std::memcmp(first.data(), pixels.data(), header_bytes) != 0,
                        "high contrast bypasses the special Sharp title policy without a mode change");
                else if (frame_index == 2) check(first == pixels,
                    "leaving high contrast restores the cached normal title appearance");
                ++frame_index;
            }
        }
    }
    typography::SetTextRenderMode(previous_mode);
    std::printf("Dialog titles: %d PASS / %d FAIL\n", checks - failures, failures);
    return failures ? 1 : 0;
}
