// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/app_internal.h"
#include "../ui/content_reveal_animation.h"
#include "../ui/animation_clock.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace pulse::ui {

struct ThumbnailCacheTestAccess {
    static void Seed(ThumbnailCache& cache, const std::wstring& path, bool text) {
        std::lock_guard lock(cache.mutex_);
        for (const auto pixels : {0u, 2048u}) {
            const auto key = cache.Key(path, pixels, 0, 64) + (pixels ? L"" : L":properties");
            ThumbnailCache::Item item;
            if (pixels) {
                item.kind = text ? ipc::PreviewContentKind::Text : ipc::PreviewContentKind::Bitmap;
                if (text) { item.text = L"Preview content\n第二行文本"; item.bytes_read = 64; }
                else {
                    item.w = item.h = 8;
                    item.stride = 32;
                    item.pixels.resize(256);
                    for (size_t p = 0; p < item.pixels.size(); p += 4) {
                        item.pixels[p] = 40; item.pixels[p + 1] = 60;
                        item.pixels[p + 2] = 230; item.pixels[p + 3] = 255;
                    }
                    item.cost = item.pixels.size();
                }
            }
            cache.cache_bytes_ += item.cost;
            cache.lru_.push_back(key);
            item.lru_position = std::prev(cache.lru_.end());
            cache.items_.emplace(key, std::move(item));
        }
    }
};

struct ContentRevealUiTest {
    static bool Run(AppState& s, const std::filesystem::path& root) {
        int failures = 0;
        auto check = [&](bool passed, const char* label) {
            std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
            if (!passed) ++failures;
        };
        ContentRevealAnimation reveal;
        reveal.Tick(false, 1000, true);
        check(!reveal.pending && reveal.opacity == 0, "waiting for content schedules no reveal frames");
        reveal.Tick(true, 1100, true);
        check(reveal.pending && reveal.opacity == 0, "first content frame starts without a full-opacity flash");
        reveal.Tick(true, 1180, true);
        check(reveal.pending && reveal.opacity > 0 && reveal.opacity < 1, "ready redraw advances a bounded reveal");
        reveal.Tick(true, 1260, true);
        reveal.Tick(true, 2000, true);
        check(!reveal.pending && reveal.opacity == 1, "revealed content remains settled on later redraws");
        reveal.Tick(false, 2100, true);
        reveal.Tick(true, 2200, true);
        reveal.Sample(2220, false);
        check(!reveal.pending && reveal.opacity == 1, "disabling motion settles an active reveal immediately");
        reveal.Tick(false, 2300, false);
        reveal.Tick(true, 2400, false);
        check(!reveal.pending && reveal.opacity == 1, "content appears immediately with motion disabled");

        auto& renderer = s.renderer;
        auto& cache = renderer.thumbnail_cache_;
        const auto bitmap = (std::filesystem::absolute(root) / L"reveal.png").wstring();
        const auto text = (std::filesystem::absolute(root) / L"reveal.txt").wstring();
        ThumbnailCacheTestAccess::Seed(cache, bitmap, false);
        ThumbnailCacheTestAccess::Seed(cache, text, true);
        WindowViewModel vm;
        vm.details_visible = true;
        vm.safe_mode = true;
        vm.details.has_selection = true;
        vm.details.name = L"reveal.png";
        vm.details.path = bitmap;
        vm.details.attrs = FILE_ATTRIBUTE_NORMAL;
        vm.details.size_value = 64;
        vm.details.preview_enabled = true;
        renderer.SetDetailsPanelVisible(true);
        renderer.motion_enabled_ = true;
        auto draw = [&](const wchar_t* filename) {
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0));
            renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                static_cast<float>(s.compositor.Height())), MakeTheme(vm.dark, HexColor(0x0078D4)));
            check(SUCCEEDED(dc->EndDraw()), "content reveal renders without Direct2D errors");
            if (filename) check(s.compositor.SaveSnapshot((root / filename).c_str()), "save real preview reveal frame");
        };
        for (const bool dark : {false, true}) {
            vm.dark = dark;
            s.scale = dark ? 1.5f : 1.0f;
            renderer.SetScale(s.scale);
            s.compositor.RecreateTextFormats(s.scale);
            s.compositor.Resize(dark ? 1800 : 1200, dark ? 960 : 640);
            const auto panel = renderer.DetailsPanelRect(static_cast<float>(s.compositor.Width()),
                static_cast<float>(s.compositor.Height()));
            check(panel.right > panel.left, "preview fixture exceeds the existing 1000 DIP panel visibility threshold");
            renderer.details_preview_identity_.clear();
            draw(dark ? L"preview-dark-start.png" : L"preview-light-start.png");
            check(renderer.DetailsPreviewAnimating() && renderer.details_content_reveal_.opacity == 0,
                "actual bitmap preview starts one reveal for the selected identity");
            renderer.details_content_reveal_.Reset();
            renderer.details_content_reveal_.Tick(true, AnimationTimeMs() - 65, true);
            draw(dark ? L"preview-dark-middle.png" : L"preview-light-middle.png");
            check(renderer.DetailsPreviewAnimating() && renderer.details_content_reveal_.opacity > 0 &&
                renderer.details_content_reveal_.opacity < 1, "real preview has an intermediate opacity across theme and DPI");
            renderer.details_content_reveal_.Reset();
            renderer.details_content_reveal_.Tick(true, AnimationTimeMs() - 200, true);
            draw(dark ? L"preview-dark-end.png" : L"preview-light-end.png");
            check(!renderer.DetailsPreviewAnimating() && renderer.details_content_reveal_.opacity == 1,
                "real bitmap preview releases animation frames at the endpoint");
        }
        vm.details.path = text;
        vm.details.name = L"reveal.txt";
        draw(nullptr);
        check(renderer.DetailsPreviewAnimating() && renderer.CanDetailsPreviewPan(),
            "selecting text resets reveal while preserving preview interaction");
        renderer.motion_enabled_ = false;
        draw(L"preview-text-no-motion.png");
        check(!renderer.DetailsPreviewAnimating() && renderer.details_content_reveal_.opacity == 1,
            "actual text preview settles immediately when motion is disabled");
        renderer.motion_enabled_ = true;
        vm.details_visible = false;
        draw(nullptr);
        check(!renderer.DetailsPreviewAnimating(), "hidden preview cannot request reveal frames");

        auto& toast = s.notification_toast;
        toast.SetMotionEnabled(false);
        toast.Show(s.hwnd, L"Completed", L"Animation preview", false);
        ValidateRect(s.hwnd, nullptr);
        toast.HandleMessage(s.hwnd, WM_TIMER, 0x50554C54, 0);
        check(!GetUpdateRect(s.hwnd, nullptr, FALSE), "settled toast lifetime timer does not repaint unchanged content");
        auto* dc = s.compositor.Dc();
        dc->BeginDraw();
        toast.Draw(s.compositor, MakeTheme(true, HexColor(0x0078D4)), s.scale, false);
        check(SUCCEEDED(dc->EndDraw()), "toast renders with motion disabled");
        check(s.compositor.SaveSnapshot((root / L"toast-no-motion.png").c_str()), "save final toast layout");
        toast.HandleMessage(s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!toast.IsVisible(), "Escape still dismisses the toast immediately");
        renderer.RefreshMotionPreferences();
        renderer.SetDetailsPanelVisible(false);
        return failures == 0;
    }
};
} // namespace pulse::ui

bool RunContentRevealUiChecks(pulse::AppState& s, const std::filesystem::path& root) {
    return pulse::ui::ContentRevealUiTest::Run(s, root);
}
