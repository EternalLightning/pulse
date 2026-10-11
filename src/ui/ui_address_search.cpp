#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "address_search_layout.h"

namespace pulse::ui {

void MainRenderer::DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme) {
    const auto field = SearchBarRect(w);
    if (!vm.address_searching && w-EffectiveSidebarWidth(w)<480*scale_) {
        const auto button=D2D1::RectF(field.right-32*scale_,field.top,field.right,field.bottom);
        DrawButton(button,theme,IsHovered(vm,HitTestResult::AddressSearch) ? theme.fill_hover : kTransparent,
            kIconSearch,L"",theme.text_secondary,true,true);
        return;
    }
    fluent::ControlState state{};
    state.focused = vm.address_searching;
    state.hovered = IsHovered(vm, HitTestResult::AddressSearch);
    if (vm.address_searching) painter_.DrawTextFieldFrame(field, state);
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
    if (vm.address_searching) {
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
    } else {
        fluent::TextFieldSpec search;
        search.bounds = field;
        search.state = state;
        search.placeholder = vm.address_search_placeholder.empty()
            ? l10n::Get(l10n::StringId::Search) : vm.address_search_placeholder;
        search.leading_glyph = L"\xE721";
        search.trailing_keycap = L"Ctrl+K";
        search.suppress_text = true;
        painter_.DrawTextField(search);
        MakeBrush(compositor_->Dc(),theme.text_secondary,brTextSecondary_);
        const float text_left=field.left+36*scale_;
        const float text_right=field.right-14*scale_-painter_.MeasureBadgeWidth(search.trailing_keycap);
        DrawTextEndEllipsis(compositor_->Dc(),compositor_->DwriteFactory(),compositor_->TextFormat(),
            brTextSecondary_.get(),std::wstring(search.placeholder),text_left,field.top,
            std::max(0.0f,text_right-text_left),field.bottom-field.top);
    }
    if (vm.address_search_animation > 0.0f) {
        auto color = theme.accent;
        color.a *= vm.address_search_animation;
        ComPtr<ID2D1SolidColorBrush> brush;
        compositor_->Dc()->CreateSolidColorBrush(color, &brush);
        const float right = field.right - 6.0f * scale_;
        const float width = (field.right - field.left - 12.0f * scale_) * vm.address_search_animation;
        if (brush.get()) compositor_->Dc()->DrawLine(D2D1::Point2F(right - width, field.bottom - scale_),
            D2D1::Point2F(right, field.bottom - scale_), brush.get(), 2.0f * scale_);
    }
}

} // namespace pulse::ui
