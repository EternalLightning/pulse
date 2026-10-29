#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "address_search_layout.h"

namespace pulse::ui {

void MainRenderer::DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme) {
    const auto field = SearchBarRect(w);
    const float progress = std::clamp(vm.address_search_animation, 0.0f, 1.0f);
    auto* dc = compositor_->Dc();
    D2D1_MATRIX_3X2_F transform;
    dc->GetTransform(&transform);
    const auto begin_content = [&](float opacity, float offset) {
        dc->PushLayer(D2D1::LayerParameters(field, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Identity(), opacity), nullptr);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(offset, 0) * transform);
    };
    const auto end_content = [&] { dc->SetTransform(transform); dc->PopLayer(); };
    fluent::ControlState state{};
    state.hovered = IsHovered(vm, HitTestResult::AddressSearch);
    const bool compact = w - EffectiveSidebarWidth(w) < 480 * scale_;
    if (!compact) painter_.DrawTextFieldFrame(field, state);
    const float collapsed_opacity = AddressSearchContentOpacity(1.0f - progress);
    const float expanded_opacity = AddressSearchContentOpacity(progress);
    if (collapsed_opacity > 0.0f) {
        begin_content(collapsed_opacity, -6.0f * scale_ * progress);
        if (compact) {
            const auto icon = D2D1::RectF(field.right - 32 * scale_, field.top, field.right, field.bottom);
            DrawButton(icon, theme, state.hovered ? theme.fill_hover : kTransparent,
                kIconSearch, L"", theme.text_secondary, true, true);
        } else {
            DrawIconText(field.left + 10 * scale_, field.top, 18 * scale_, field.bottom - field.top,
                kIconSearch, L"", theme.text_secondary);
            const float badge_width = painter_.MeasureBadgeWidth(L"Ctrl+K");
            painter_.DrawBadge({D2D1::RectF(field.right - 8 * scale_ - badge_width, field.top + 3 * scale_,
                field.right - 8 * scale_, field.bottom - 3 * scale_), L"Ctrl+K", fluent::BadgeKind::Keycap});
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            const auto placeholder = vm.address_search_placeholder.empty() ? l10n::Get(l10n::StringId::Search)
                : vm.address_search_placeholder;
            DrawTextEndEllipsis(compositor_, theme.bg, dc, compositor_->DwriteFactory(), compositor_->TextFormat(),
                brTextSecondary_.get(), placeholder, field.left + 36 * scale_, field.top,
                std::max(0.0f, field.right - 14 * scale_ - badge_width - field.left - 36 * scale_), field.bottom - field.top);
        }
        end_content();
    }
    auto button = [&](D2D1_RECT_F bounds, const std::wstring& text, const wchar_t* glyph,
                      HitTestResult::Region region, bool dropdown = false) {
        fluent::ButtonSpec spec;
        spec.bounds = bounds;
        spec.text = text;
        spec.glyph = glyph;
        spec.icon_only = text.empty();
        spec.skip_glyph = true;
        spec.kind = fluent::ButtonKind::Transparent;
        spec.bordered = false;
        spec.drop_down = dropdown;
        spec.state.hovered = IsHovered(vm, region);
        spec.state.pressed = (region == HitTestResult::AddressSearchScope && vm.address_search_scope_open) ||
                             (region == HitTestResult::AddressSearchOptions && vm.address_search_options_open);
        painter_.DrawButton(spec);
        {
            // Compact buttons cannot use the text button's 8-DIP side padding.
            const float edge = std::min({20.0f * scale_, bounds.right - bounds.left - 4.0f * scale_,
                                         bounds.bottom - bounds.top - 4.0f * scale_});
            const float cx = spec.icon_only ? (bounds.left + bounds.right) * 0.5f
                                            : bounds.left + 18.0f * scale_;
            const float cy = (bounds.top + bounds.bottom) * 0.5f;
            DrawIconText(cx - edge * 0.5f, cy - edge * 0.5f, edge, edge,
                         glyph, L"", theme.text, 0.8f);
        }
    };
    if (expanded_opacity > 0.0f) {
        begin_content(expanded_opacity, 0);
        state.focused = true;
        painter_.DrawTextFieldFrame(field, state, true);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(6.0f * scale_ * (1.0f - progress), 0) * transform);
        const auto layout = LayoutAddressSearch(field, scale_);
        if (!vm.address_editing) {
            const auto text = vm.address_search_text.empty()
                ? l10n::Get(vm.address_search_content ? l10n::StringId::SearchContentHint : l10n::StringId::SearchNameHint) : vm.address_search_text;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(vm.address_search_text.empty()
                ? theme.text_secondary : theme.text, &brush);
            if (brush.get()) DrawTextRect(compositor_->Dc(), compositor_->AddressFormat(), brush.get(),
                text, layout.input.left, layout.input.top, layout.input.right - layout.input.left,
                layout.input.bottom - layout.input.top);
        }
        if (vm.address_scope_animation > 0.0f) {
            auto color = theme.accent;
            color.a *= vm.address_scope_animation * 0.18f;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(color, &brush);
            if (brush.get()) compositor_->Dc()->FillRoundedRectangle(
                D2D1::RoundedRect(layout.scope, 4.0f * scale_, 4.0f * scale_), brush.get());
        }
        const auto label = l10n::Get(vm.address_search_current
            ? l10n::StringId::SearchScopeHere : l10n::StringId::SearchScopeAll);
        if (layout.scope.right > layout.scope.left)
            button(layout.scope, layout.scope_label ? label : L"", L"\xE721",
                   HitTestResult::AddressSearchScope, layout.scope_label);
        painter_.DrawSegmentedTrack(layout.mode);
        auto segment = [&](D2D1_RECT_F bounds, bool content) {
            fluent::ButtonSpec spec;
            spec.bounds = D2D1::RectF(bounds.left + scale_, bounds.top + scale_, bounds.right - scale_, bounds.bottom - scale_);
            spec.text = layout.mode_label ? l10n::Get(content ? l10n::StringId::SearchModeContent : l10n::StringId::SearchModeName) : L"";
            spec.glyph = layout.mode_label ? L"" : content ? L"\xE8A5" : L"\xE8B7";
            spec.icon_only = !layout.mode_label;
            spec.skip_glyph = !layout.mode_label;
            spec.kind = fluent::ButtonKind::TransparentToggle;
            spec.state.checked = vm.address_search_content == content;
            spec.state.hovered = IsHovered(vm, content ? HitTestResult::AddressSearchContent : HitTestResult::AddressSearchMode);
            painter_.DrawButton(spec);
            if (!layout.mode_label) DrawIconText(bounds.left, bounds.top,
                bounds.right-bounds.left, bounds.bottom-bounds.top, content ? L"\xE8A5" : L"\xE8B7", L"",
                theme.text, 0.8f);
        };
        segment(layout.name, false);
        segment(layout.content, true);
        button(layout.options, L"", L"\xE9E9", HitTestResult::AddressSearchOptions);
        if (vm.address_search_has_text && layout.clear.right > layout.clear.left)
            button(layout.clear, L"", L"\xE711", HitTestResult::AddressSearchClear);
        if (layout.close.right > layout.close.left)
            button(layout.close, L"", L"\xE72B", HitTestResult::AddressSearchClose);
        end_content();
    }
}

} // namespace pulse::ui
