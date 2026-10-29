// Included inside ui_renderer_internal.h's anonymous namespace.
inline model::BuiltinMenuSurface PulseMenuSurface(const WindowViewModel& vm) {
    return vm.settings_context_tab == 1 ? model::BuiltinMenuSurface::Background
         : vm.settings_context_tab == 2 ? model::BuiltinMenuSurface::RowButtons
                                        : model::BuiltinMenuSurface::Item;
}
// Preview metrics (DIP), shared by layout and drawing.
inline constexpr float kPulsePreviewTitle = 24.0f, kPulsePreviewRow = 34.0f,
    kPulsePreviewSwatches = 34.0f, kPulsePreviewSeparator = 9.0f, kPulsePreviewPad = 5.0f;
// Hint line under the preview box ("drag to reorder").
inline constexpr float kPulsePreviewHint = 26.0f;
inline const model::BuiltinMenuOrder& PulseMenuOrder(const WindowViewModel& vm) {
    return vm.settings_builtin_order[static_cast<size_t>(PulseMenuSurface(vm))];
}
inline bool PulseMenuOrderCustom(const WindowViewModel& vm, model::BuiltinMenuSurface surface) {
    const auto& order = vm.settings_builtin_order[static_cast<size_t>(surface)];
    return !order.empty() && model::NormalizeBuiltinMenuOrder(surface, order) != model::BuiltinMenuDefaultOrder(surface);
}
inline float PulseMenuPreviewHeight(const WindowViewModel& vm, float scale) {
    if (vm.settings_context_tab == 2) return (kPulsePreviewTitle + 56.0f + kPulsePreviewHint) * scale;
    float h = 2 * kPulsePreviewPad;
    for (const auto& r : model::BuiltinMenuVisibleRows(PulseMenuSurface(vm), vm.settings_builtin_hidden, PulseMenuOrder(vm))) {
        h += kPulsePreviewRow;
        if (r.row.item == model::BuiltinMenuItem::Tags) h += kPulsePreviewSwatches;
        if (r.separator_after) h += kPulsePreviewSeparator;
    }
    return (kPulsePreviewTitle + h + kPulsePreviewHint) * scale;
}
// The menu box inside l.pulse_preview (below the title, above the hint).
inline D2D1_RECT_F PulsePreviewBox(const D2D1_RECT_F& preview, float scale) {
    return D2D1::RectF(preview.left, preview.top + kPulsePreviewTitle * scale,
                       preview.right, preview.bottom - kPulsePreviewHint * scale);
}
inline int PulseMenuRowHit(const model::BuiltinMenuRow& row) {
    return row.fixed != model::BuiltinFixedRow::None ? kSettingsMenuFixedHit + static_cast<int>(row.fixed)
                                                   : kSettingsMenuRowHit + static_cast<int>(row.item);
}

// One preview row (the tag swatches belong to the 标签... row) or, on the
// row-buttons tab, one hover button.
struct PulsePreviewRow {
    model::BuiltinMenuRow row{};
    D2D1_RECT_F rect{};
    bool separator_after = false;
};
struct PulsePreviewLayout {
    std::vector<PulsePreviewRow> rows; // without the dragged row while dragging
    bool dragging = false;
    model::BuiltinMenuRow drag_row{};
    D2D1_RECT_F ghost{}, gap{};        // floating row; the slot it would land in
    model::BuiltinMenuOrder order;       // order a drop now would give
};
inline std::vector<PulsePreviewRow> LayoutPulsePreviewRows(const WindowViewModel& vm, const model::BuiltinMenuOrder& order,
                                                           const D2D1_RECT_F& box, float s) {
    std::vector<PulsePreviewRow> out;
    if (vm.settings_context_tab == 2) {
        const auto visible = model::BuiltinMenuVisibleRows(model::BuiltinMenuSurface::RowButtons, vm.settings_builtin_hidden, order);
        const auto row = D2D1::RectF(box.left + 8 * s, box.top + 12 * s, box.right - 8 * s, box.bottom - 12 * s);
        out.resize(visible.size());
        float bx = row.right - 28 * s;
        for (size_t k = visible.size(); k-- > 0;) {
            out[k] = { visible[k].row, D2D1::RectF(bx, row.top + 4 * s, bx + 24 * s, row.bottom - 4 * s), false };
            bx -= 26 * s;
        }
        return out;
    }
    float y = box.top + kPulsePreviewPad * s;
    const float x0 = box.left + 6 * s, x1 = box.right - 6 * s;
    for (const auto& v : model::BuiltinMenuVisibleRows(PulseMenuSurface(vm), vm.settings_builtin_hidden, order)) {
        const float h = (kPulsePreviewRow + (v.row.item == model::BuiltinMenuItem::Tags ? kPulsePreviewSwatches : 0.0f)) * s;
        out.push_back({ v.row, D2D1::RectF(x0, y, x1, y + h), v.separator_after });
        y += h;
        if (v.separator_after) y += kPulsePreviewSeparator * s;
    }
    return out;
}
// While a row is dragged, every landing place is tried and the one whose
// slot is nearest the floating row wins; the rows are laid out as that drop
// would leave them, so separators and groups preview truthfully.
inline PulsePreviewLayout LayoutPulsePreview(const WindowViewModel& vm, const D2D1_RECT_F& box, float s) {
    PulsePreviewLayout l;
    const auto surface = PulseMenuSurface(vm);
    const bool horizontal = vm.settings_context_tab == 2;
    l.order = model::NormalizeBuiltinMenuOrder(surface, PulseMenuOrder(vm));
    l.rows = LayoutPulsePreviewRows(vm, l.order, box, s);
    if (vm.settings_menu_drag < 0 || vm.settings_menu_drag >= model::kBuiltinMenuItemCount) return l;
    const auto dragged = static_cast<model::BuiltinMenuItem>(vm.settings_menu_drag);
    auto movable = [&](const PulsePreviewRow& r) { return r.row.fixed == model::BuiltinFixedRow::None && r.row.item < model::BuiltinMenuItem::Count; };
    const auto held = std::find_if(l.rows.begin(), l.rows.end(), [&](const PulsePreviewRow& r) { return movable(r) && r.row.item == dragged; });
    if (held == l.rows.end()) return l;
    model::BuiltinMenuOrder rest = l.order;
    rest.erase(std::remove(rest.begin(), rest.end(), dragged), rest.end());
    std::vector<model::BuiltinMenuItem> rest_visible;
    for (const auto& r : l.rows) if (movable(r) && r.row.item != dragged) rest_visible.push_back(r.row.item);
    if (rest_visible.empty()) return l;
    const float size = horizontal ? held->rect.right - held->rect.left : held->rect.bottom - held->rect.top;
    const float lo = horizontal ? box.left : box.top, hi = horizontal ? box.right : box.bottom;
    const float start = std::clamp(vm.settings_menu_drag_pos - vm.settings_menu_drag_grab, lo, std::max(lo, hi - size));
    float best = 1e30f;
    for (size_t p = 0; p <= rest_visible.size(); ++p) {
        model::BuiltinMenuOrder candidate = rest;
        auto at = p < rest_visible.size() ? std::find(candidate.begin(), candidate.end(), rest_visible[p])
                                          : std::find(candidate.begin(), candidate.end(), rest_visible.back()) + 1;
        candidate.insert(at, dragged);
        auto rows = LayoutPulsePreviewRows(vm, candidate, box, s);
        const auto slot = std::find_if(rows.begin(), rows.end(), [&](const PulsePreviewRow& r) { return movable(r) && r.row.item == dragged; });
        if (slot == rows.end()) continue;
        const float d = std::abs((horizontal ? slot->rect.left : slot->rect.top) - start);
        if (d >= best) continue;
        best = d;
        l.order = std::move(candidate);
        l.gap = slot->rect;
        l.drag_row = slot->row;
        rows.erase(slot);
        l.rows = std::move(rows);
    }
    l.dragging = true;
    l.ghost = horizontal ? D2D1::RectF(start, l.gap.top, start + size, l.gap.bottom)
                         : D2D1::RectF(l.gap.left, start, l.gap.right, start + size);
    return l;
}

inline float LayoutSettingsPulseMenu(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                                     const fluent::Painter* painter) {
    using I = l10n::StringId;
    const float left = l.content.left + 20 * s, right = l.content.right - 20 * s;
    const float width = std::max(1.0f, right - left);
    auto text_width = [&](std::wstring_view text) {
        return painter ? painter->MeasureButtonWidth(text) : 100 * s;
    };
    const auto& intro = l10n::Get(I::ContextMenuDesc);
    const float intro_h = SettingsCaptionHeight(intro, width, s, painter);
    l.ctx_intro = D2D1::RectF(left, y, right, y + intro_h);
    y += std::max(intro_h + 12 * s, 46 * s);
    if (!vm.settings_index_error.empty()) {
        const float error_h = std::max(44 * s, SettingsCaptionHeight(vm.settings_index_error, width - 48 * s, s, painter) + 20 * s);
        l.ctx_error = D2D1::RectF(left, y, right, y + error_h);
        y += error_h + 12 * s;
    }
    y = LayoutCtxLayerCards(l.ctx_layer[0], l.ctx_layer[1], left, right, y, painter, s);

    const float top = y, body_left = left + 16 * s, body_right = right - 16 * s;
    const float available = std::max(1.0f, body_right - body_left);
    const float restore_w = std::min(available * 0.42f, text_width(l10n::Get(I::RestoreDefaults)) - 8 * s);
    l.pulse_restore = D2D1::RectF(body_right - restore_w, top + 12 * s, body_right, top + 40 * s);
    l.pulse_title = D2D1::RectF(left + 54 * s, top + 12 * s, l.pulse_restore.left - 8 * s, top + 36 * s);
    const float caption_left = left + 54 * s;
    const float caption_h = SettingsCaptionHeight(l10n::Get(I::ContextPulseMenuDesc), right - 16 * s - caption_left, s, painter);
    l.pulse_description = D2D1::RectF(caption_left, top + 40 * s, right - 16 * s, top + 40 * s + caption_h);
    y = std::max(top + 72 * s, l.pulse_description.bottom + 14 * s);

    static constexpr I kPresetDesc[] = {I::ContextPresetSlimDesc, I::ContextPresetStandardDesc,
        I::ContextPresetFullDesc, I::ContextPresetCustomDesc};
    const int columns = available >= 560 * s ? 4 : available >= 280 * s ? 2 : 1;
    const float preset_gap = 8 * s, preset_w = (available - (columns - 1) * preset_gap) / columns;
    for (int first = 0; first < 4; first += columns) {
        float height = 50 * s;
        const int count = std::min(columns, 4 - first);
        for (int i = 0; i < count; ++i)
            height = std::max(height, 29 * s + SettingsCaptionHeight(l10n::Get(kPresetDesc[first + i]), preset_w - 20 * s, s, painter));
        for (int i = 0; i < count; ++i) {
            const float x = body_left + i * (preset_w + preset_gap);
            l.pulse_preset[first + i] = D2D1::RectF(x, y, x + preset_w, y + height);
        }
        y += height + preset_gap;
    }
    y += 4 * s;
    static constexpr I kTabs[] = {I::ContextTabItem, I::ContextTabBackground, I::ContextTabRow};
    const float tabs_top = y;
    float tx = body_left + 3 * s;
    for (int i = 0; i < 3; ++i) {
        const float tab_w = std::min(available - 6 * s, std::max(72 * s, text_width(l10n::Get(kTabs[i]))));
        if (i > 0 && tx + tab_w > body_right - 3 * s) {
            tx = body_left + 3 * s;
            y += 36 * s;
        }
        l.pulse_tab[i] = D2D1::RectF(tx, y + 3 * s, tx + tab_w, y + 33 * s);
        tx += tab_w;
    }
    l.pulse_tabs = D2D1::RectF(body_left, tabs_top, body_right, y + 36 * s);
    y += 52 * s;

    l.context_rows.resize(vm.settings_items.size());
    const bool side_preview = width >= 700 * s;
    const float preview_w = std::min(244 * s, available);
    const float list_right = side_preview ? body_right - preview_w - 28 * s : body_right;
    const float list_top = y;
    for (const auto& row : model::BuiltinMenuOrderedRows(PulseMenuSurface(vm), PulseMenuOrder(vm))) {
        const bool shared = vm.settings_context_tab != 2 && row.fixed == model::BuiltinFixedRow::None && model::BuiltinMenuShared(row.item);
        const auto row_rect = D2D1::RectF(body_left, y, list_right, y + (shared ? 56 : 44) * s);
        if (row.fixed != model::BuiltinFixedRow::None) {
            l.pulse_fixed[row.fixed == model::BuiltinFixedRow::Open ? 0 : 1] = row_rect;
        } else {
            const size_t i = vm.settings_builtin_first + static_cast<size_t>(row.item);
            if (i < l.context_rows.size()) l.context_rows[i] = row_rect;
        }
        y = row_rect.bottom;
    }
    float preview_top = side_preview ? list_top : y + 24 * s;
    const float preview_left = side_preview ? body_right - preview_w : body_left;
    if (PulseMenuOrderCustom(vm, PulseMenuSurface(vm))) {
        const float order_w = std::min(preview_w, text_width(l10n::Get(I::ContextOrderReset)) - 8 * s);
        l.pulse_order_reset = D2D1::RectF(preview_left + preview_w - order_w, preview_top,
            preview_left + preview_w, preview_top + 28 * s);
        preview_top += 32 * s;
    }
    l.pulse_preview = D2D1::RectF(preview_left, preview_top, preview_left + preview_w,
        preview_top + PulseMenuPreviewHeight(vm, s));
    y = std::max(y, l.pulse_preview.bottom) + 16 * s;
    l.pulse_card = D2D1::RectF(left, top, right, y);
    return y + 24 * s;
}
