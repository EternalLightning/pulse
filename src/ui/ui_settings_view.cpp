// ui_settings_view.cpp — Settings page layout and painting.
#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {

void MainRenderer::DrawSettings(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    D2D1_COLOR_F nav_bg = theme.tab_bg;
    if (vm.backdrop_enabled) nav_bg.a = vm.dark ? 0.62f : 0.70f;
    MakeBrush(dc, nav_bg, brFillHover_);
    FillRect(dc, brFillHover_.get(), lay.nav.left, lay.nav.top,
             lay.nav.right - lay.nav.left, lay.nav.bottom - lay.nav.top);
    FillRect(dc, brStrokeDivider_.get(), lay.nav.right - 1.0f, lay.nav.top, 1.0f,
             lay.nav.bottom - lay.nav.top);

    const bool compact_nav = lay.nav.right - lay.nav.left < 100*scale_;
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    FillRect(dc, brStrokeDivider_.get(), lay.nav.left+16*scale_, lay.nav_row[3].top-12*scale_,
        lay.nav.right-lay.nav.left-32*scale_, 1);
    if (!compact_nav) painter_.DrawText(L"Pulse " + vm.settings_version,
        D2D1::RectF(lay.nav.left+20*scale_,lay.nav.bottom-40*scale_,lay.nav.right-12*scale_,lay.nav.bottom-16*scale_),
        compositor_->SmallFormat(),theme.text_secondary);

    static constexpr pulse::l10n::StringId kNav[] = {
        pulse::l10n::StringId::SettingsGeneral,
        pulse::l10n::StringId::SettingsSearchIndex,
        pulse::l10n::StringId::SettingsContextMenu,
        pulse::l10n::StringId::SettingsAboutDiagnostics,
        pulse::l10n::StringId::SettingsDuplicates,
    };
    static constexpr const wchar_t* kNavIcon[] = {
        kIconHome, kIconSearch, kIconSettings, kIconInfo, kIconCopy
    };
    for (int i = 0; i < kSettingsNavCount; ++i) {
        const bool active = vm.settings_page == i;
        const bool hovered = IsHovered(vm, HitTestResult::SettingsNav, i);
        const auto& rc = lay.nav_row[i];
        if (active || hovered) {
            MakeBrush(dc, active ? theme.fill_selected : theme.fill_hover, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), rc.left, rc.top,
                            rc.right - rc.left, rc.bottom - rc.top, 6.0f * scale_);
        }
        if (active) {
            MakeBrush(dc, theme.accent, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), rc.left, rc.top+10*scale_, 3*scale_, 20*scale_, 1.5f*scale_);
        }
        DrawIconText(rc.left + (compact_nav ? 13.0f : 12.0f) * scale_, rc.top, 22.0f * scale_, rc.bottom - rc.top,
                     kNavIcon[i], L"*", active ? theme.accent : theme.text_secondary, 0.85f);
        MakeBrush(dc, theme.text, brText_);
        if (!compact_nav) DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), pulse::l10n::Get(kNav[i]),
                     rc.left + 42.0f * scale_, rc.top, rc.right - rc.left - 48.0f * scale_,
                     rc.bottom - rc.top);
    }

    dc->PushAxisAlignedClip(lay.content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float pad = 20.0f * scale_;
    const float origin = lay.content_origin;

    const auto page_title_id = vm.settings_page == 0
        ? pulse::l10n::StringId::SettingsGeneral
        : vm.settings_page == 1 ? pulse::l10n::StringId::SettingsSearchIndex
        : vm.settings_page == 2 ? pulse::l10n::StringId::SettingsContextMenu
        : vm.settings_page == 4 ? pulse::l10n::StringId::SettingsDuplicates
                                : pulse::l10n::StringId::SettingsAboutDiagnostics;
    MakeBrush(dc, theme.text, brText_);
    const auto& title = l10n::Get(page_title_id);
    ComPtr<IDWriteTextLayout> title_layout;
    compositor_->DwriteFactory()->CreateTextLayout(title.c_str(), static_cast<UINT32>(title.size()),
        compositor_->HeaderFormat(), lay.content.right-lay.content.left-pad*2, 40*scale_, &title_layout);
    if (title_layout.get()) {
        title_layout->SetFontSize(26*scale_, {0,static_cast<UINT32>(title.size())});
        title_layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {0,static_cast<UINT32>(title.size())});
        dc->DrawTextLayout(D2D1::Point2F(lay.content.left+pad,origin+pad),title_layout.get(),brText_.get());
    }
    if (vm.settings_page == 0 || vm.settings_page == 1)
        painter_.DrawText(l10n::Get(vm.settings_page == 0 ? l10n::StringId::SettingsGeneralIntro : l10n::StringId::SettingsSearchIntro),
            D2D1::RectF(lay.content.left+pad,origin+60*scale_,lay.content.right-pad,origin+84*scale_),
            compositor_->SmallFormat(),theme.text_secondary);


    if (vm.settings_page == 0) {
        DrawSettingsCore(vm, rect, theme);
    } else if (vm.settings_page == 1) {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };
        DrawSettingsCore(vm, rect, theme);
        if (vm.settings_expanded & 2u) {
        fluent::InfoBarSpec info;
        info.bounds = lay.index_info;
        info.title = pulse::l10n::Get((vm.settings_index_service || vm.settings_index_installed)
            ? pulse::l10n::StringId::SettingsFullIndex
            : pulse::l10n::StringId::SettingsUserIndex);
        info.message = vm.settings_index_service
            ? pulse::l10n::Get(pulse::l10n::StringId::SettingsFullIndexDesc)
            : pulse::l10n::Get(vm.settings_index_installed
                ? pulse::l10n::StringId::SettingsServiceWaiting
                : pulse::l10n::StringId::SettingsUserIndexDesc);
        info.kind = vm.settings_index_error.empty() ? fluent::InfoBarKind::Informational
                                                     : fluent::InfoBarKind::Error;
        if (!vm.settings_index_error.empty()) info.message = vm.settings_index_error;
        else if (vm.settings_index_migrating)
            info.message = pulse::l10n::Get(pulse::l10n::StringId::IndexMigratingShort);
        info.show_close = false;
        painter_.DrawInfoBar(info);


        draw_card(lay.index_status);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::IndexStatus),
                     lay.index_status.left + 16.0f * scale_, lay.index_status.top + 12.0f * scale_,
                     lay.index_status.right - lay.index_status.left - 32.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.settings_index_status,
                     lay.index_status.left + 16.0f * scale_, lay.index_status.top + 40.0f * scale_,
                     lay.index_status.right - lay.index_status.left - 32.0f * scale_, 24.0f * scale_);

        draw_card(lay.index_path);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::IndexLocation),
                     lay.index_path.left + 16.0f * scale_, lay.index_path.top + 8.0f * scale_,
                     100.0f * scale_, 22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.settings_index_path,
                     lay.index_path.left + 16.0f * scale_, lay.index_path.top + 32.0f * scale_,
                     lay.index_path.right - lay.index_path.left - 32.0f * scale_,
                     20.0f * scale_);
        const auto storage_status_bounds = D2D1::RectF(lay.index_path.left + 16.0f * scale_, lay.index_path.top + 56.0f * scale_,
                                                       lay.index_path.right - 16.0f * scale_, lay.index_path.top + 92.0f * scale_);
        dc->PushAxisAlignedClip(storage_status_bounds,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto& storage_status = vm.settings_index_storage_status.empty() && !vm.settings_index_service && !vm.settings_index_installed
            ? l10n::Get(l10n::StringId::StorageRestartDescription) : vm.settings_index_storage_status;
        painter_.DrawWrappedCaption(storage_status,D2D1::Point2F(storage_status_bounds.left,storage_status_bounds.top),
                                    storage_status_bounds.right-storage_status_bounds.left,theme.text_secondary);
        dc->PopAxisAlignedClip();
        const std::wstring actions[] = {
            pulse::l10n::Get(pulse::l10n::StringId::Rebuild),
            pulse::l10n::Get(pulse::l10n::StringId::OpenLocation),
            pulse::l10n::Get(pulse::l10n::StringId::ChangeLocation),
            pulse::l10n::Get(pulse::l10n::StringId::InstallService),
        };
        for (int i = 0; i < ((vm.settings_index_service || vm.settings_index_installed) ? 3 : 4); ++i) {
            fluent::ControlState st{};
            st.enabled = (i != 0 || vm.settings_index_service) && (!vm.settings_index_migrating || i == 1);
            st.hovered = st.enabled && IsHovered(vm, HitTestResult::SettingsIndexAction, i);
            painter_.DrawButton({ lay.index_action[i], actions[i], {},
                                  i == 2 ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                  st });
        }

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        const float header_y = lay.index_volume_rows.empty()
            ? lay.index_path.bottom + 18.0f * scale_
            : lay.index_volume_rows.front().top - 30.0f * scale_;
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::LocalDrives),
                     lay.content.left + pad, header_y, 200.0f * scale_, 22.0f * scale_);
        for (size_t i = 0; i < vm.settings_index_volumes.size() && i < lay.index_volume_rows.size(); ++i) {
            const auto& volume = vm.settings_index_volumes[i];
            const auto& row = lay.index_volume_rows[i];
            if (IsHovered(vm, HitTestResult::SettingsIndexVolume, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left, row.top,
                                row.right - row.left, row.bottom - row.top, 6.0f * scale_);
            }
            fluent::ControlState check{};
            check.checked = volume.checked;
            check.enabled = volume.enabled && !volume.pending;
            check.hovered = IsHovered(vm, HitTestResult::SettingsIndexVolume, static_cast<int>(i));
            painter_.DrawCheckBox(D2D1::RectF(row.left + 12.0f * scale_, row.top,
                                              row.left + 44.0f * scale_, row.bottom), L"", check);
            const float badge_h = 22.0f * scale_;
            const float badge_w = volume.state.empty() ? 0.0f
                : (std::min)(painter_.MeasureBadgeWidth(volume.state), 148.0f * scale_);
            const float text_w = (std::max)(40.0f * scale_,
                row.right - row.left - 60.0f * scale_ - (badge_w > 0 ? badge_w + 16.0f * scale_ : 0));
            MakeBrush(dc, check.enabled ? theme.text : theme.text_disabled, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), volume.title,
                         row.left + 48.0f * scale_, row.top + 6.0f * scale_,
                         text_w, 22.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), volume.detail,
                         row.left + 48.0f * scale_, row.top + 30.0f * scale_,
                         text_w, 18.0f * scale_);
            if (badge_w > 0.0f) {
                const float badge_x = row.right - 12.0f * scale_ - badge_w;
                const float badge_y = row.top + ((row.bottom - row.top) - badge_h) * 0.5f;
                painter_.DrawBadge({ D2D1::RectF(badge_x, badge_y,
                                                 badge_x + badge_w, badge_y + badge_h),
                                     volume.state, IndexVolumeBadgeKind(volume.state) });
            }
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            FillRect(dc, brStrokeDivider_.get(), row.left + 12.0f * scale_, row.bottom - 1.0f,
                     row.right - row.left - 24.0f * scale_, 1.0f);
        }

        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::Exclusions),
                     lay.content.left + pad, lay.index_exclude_action.top + 5.0f * scale_,
                     160.0f * scale_, 22.0f * scale_);
        fluent::ControlState add_exclude{};
        add_exclude.enabled = vm.settings_index_service;
        add_exclude.hovered = add_exclude.enabled &&
            IsHovered(vm, HitTestResult::SettingsIndexExcludeAction, 0);
        painter_.DrawButton({ lay.index_exclude_action,
                              pulse::l10n::Get(pulse::l10n::StringId::AddFolder), {},
                              fluent::ButtonKind::Primary, add_exclude });
        if (vm.settings_index_excluded_paths.empty() &&
            lay.index_exclude_empty.bottom > lay.index_exclude_empty.top) {
            draw_card(lay.index_exclude_empty);
            const float art_top = lay.index_exclude_empty.top + 8.0f * scale_;
            const float art_bottom = lay.index_exclude_empty.bottom - 48.0f * scale_;
            const D2D1_RECT_F art = D2D1::RectF(lay.index_exclude_empty.left + 16.0f * scale_,
                                                art_top,
                                                lay.index_exclude_empty.right - 16.0f * scale_,
                                                art_bottom);
            const float svg_opacity = theme.bg.r > 0.5f ? 0.68f : 1.0f;
            if (!DrawExcludeEmptySvg(art, svg_opacity)) {
                fluent::EmptyStateSpec fallback;
                fallback.bounds = art;
                fallback.glyph = L"\xE738";
                fallback.title = pulse::l10n::Get(pulse::l10n::StringId::NoExcludedFolders);
                painter_.DrawEmptyState(fallback);
            } else {
                const D2D1_RECT_F caption = D2D1::RectF(
                    lay.index_exclude_empty.left + 16.0f * scale_,
                    art_bottom + 6.0f * scale_,
                    lay.index_exclude_empty.right - 16.0f * scale_,
                    lay.index_exclude_empty.bottom - 12.0f * scale_);
                painter_.DrawText(
                    pulse::l10n::Get(pulse::l10n::StringId::NoExcludedFoldersDesc),
                    caption, compositor_->SmallFormat(), theme.text_secondary,
                    fluent::HorizontalAlignment::Center);
            }
        }
        for (size_t i = 0; i < vm.settings_index_excluded_paths.size() &&
                           i < lay.index_exclude_rows.size() &&
                           i < lay.index_exclude_remove.size(); ++i) {
            const auto& row = lay.index_exclude_rows[i];
            const auto& remove_rc = lay.index_exclude_remove[i];
            if (IsHovered(vm, HitTestResult::SettingsIndexExcludeRemove, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left, row.top,
                                row.right - row.left, row.bottom - row.top, 6.0f * scale_);
            }
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(),
                         vm.settings_index_excluded_paths[i],
                         row.left + 16.0f * scale_, row.top + 17.0f * scale_,
                         std::max(40.0f * scale_, remove_rc.left - 8.0f * scale_ -
                                  (row.left + 16.0f * scale_)), 22.0f * scale_);
            fluent::ControlState remove{};
            remove.enabled = vm.settings_index_service;
            remove.hovered = remove.enabled &&
                IsHovered(vm, HitTestResult::SettingsIndexExcludeRemove, static_cast<int>(i));
            painter_.DrawButton({ remove_rc,
                                  pulse::l10n::Get(pulse::l10n::StringId::Remove), {},
                                  fluent::ButtonKind::Standard, remove });
        }

        const float network_header_y = lay.network_action[0].top + 5.0f * scale_;
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::ServerFolders),
                     lay.content.left + pad, network_header_y, 160.0f * scale_, 22.0f * scale_);
        const std::wstring network_actions[] = {
            pulse::l10n::Get(pulse::l10n::StringId::AddFolder),
            pulse::l10n::Get(pulse::l10n::StringId::Rescan),
        };
        for (int i = 0; i < 2; ++i) {
            fluent::ControlState state{};
            state.enabled = i == 0 || !vm.settings_network_roots.empty();
            state.hovered = state.enabled && IsHovered(vm, HitTestResult::SettingsNetworkAction, i);
            painter_.DrawButton({ lay.network_action[i], network_actions[i], {},
                                  i == 0 ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                  state });
        }
        if (vm.settings_network_roots.empty()) {
            const float empty_y = lay.network_action[0].bottom + 10.0f * scale_;
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::NoServerFoldersDesc),
                         lay.content.left + pad, empty_y,
                         lay.content.right - lay.content.left - pad * 2, 22.0f * scale_);
        }
        for (size_t i = 0; i < vm.settings_network_roots.size() && i < lay.network_rows.size(); ++i) {
            const auto& network = vm.settings_network_roots[i];
            const auto& row = lay.network_rows[i];
            draw_card(row);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), network.path,
                         row.left + 16.0f * scale_, row.top + 8.0f * scale_,
                         (std::max)(40.0f * scale_,
                                    (i < lay.network_remove.size()
                                         ? lay.network_remove[i].left - 8.0f * scale_
                                         : row.right - 12.0f * scale_) -
                                        (row.left + 16.0f * scale_)),
                         22.0f * scale_);
            MakeBrush(dc, network.online ? theme.text_secondary : theme.text_disabled,
                      brTextSecondary_);
            const std::wstring detail = network.detail.empty() ? network.state
                                                               : network.state + L" · " + network.detail;
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), detail,
                         row.left + 16.0f * scale_, row.top + 34.0f * scale_,
                         (std::max)(40.0f * scale_,
                                    (i < lay.network_remove.size()
                                         ? lay.network_remove[i].left - 8.0f * scale_
                                         : row.right - 12.0f * scale_) -
                                        (row.left + 16.0f * scale_)),
                         18.0f * scale_);
            fluent::ControlState remove{};
            remove.enabled = true;
            remove.hovered = remove.enabled &&
                IsHovered(vm, HitTestResult::SettingsNetworkRemove, static_cast<int>(i));
            painter_.DrawButton({ lay.network_remove[i],
                                  pulse::l10n::Get(pulse::l10n::StringId::Remove), {},
                                  fluent::ButtonKind::Standard, remove });
        }
        }
    } else if (vm.settings_page == 2) {
        DrawSettingsContext(vm,rect,theme);
    } else if (vm.settings_page == 3) {
        auto draw_card = [&](const D2D1_RECT_F& card) {
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };

        draw_card(lay.about_card);
        {
            const D2D1_RECT_F& card = lay.about_card;
            const float x0 = card.left + 16.0f * scale_;
            const float inner_w = card.right - card.left - 32.0f * scale_;
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::AboutPulse),
                         x0, card.top + 12.0f * scale_, inner_w, 24.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(),
                         pulse::l10n::Get(pulse::l10n::StringId::AboutTagline),
                         x0, card.top + 38.0f * scale_, inner_w, 20.0f * scale_);
            const auto& rows = vm.settings_about_rows;
            const bool two_columns = AboutTwoColumns(card.right - card.left, scale_);
            const size_t lines = AboutRowLines(vm, card.right - card.left, scale_);
            const float col_w = two_columns ? inner_w * 0.5f : inner_w;
            const float label_w = (std::min)(96.0f * scale_, col_w * 0.4f);
            const float value_w = (std::max)(0.0f, col_w - label_w - 12.0f * scale_);
            IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
            const auto measure = [&](const std::wstring& t) {
                return MeasureTextWidth(compositor_->DwriteFactory(), small_fmt, t);
            };
            MakeBrush(dc, theme.text, brText_);
            for (size_t i = 0; lines > 0 && i < rows.size(); ++i) {
                const float col = two_columns ? static_cast<float>(i / lines) : 0.0f;
                const float row = static_cast<float>(two_columns ? i % lines : i);
                const float rx = x0 + col * col_w;
                const float ry = card.top + 66.0f * scale_ + row * kAboutRowDip * scale_;
                DrawTextRect(dc, small_fmt, brTextSecondary_.get(), rows[i].first,
                             rx, ry, label_w, 22.0f * scale_);
                DrawTextRect(dc, small_fmt, brText_.get(), FitEndEllipsis(rows[i].second, value_w, measure),
                             rx + label_w, ry, value_w, 22.0f * scale_);
            }
            static constexpr pulse::l10n::StringId kAboutActions[] = {
                pulse::l10n::StringId::AboutCopyInfo,
                pulse::l10n::StringId::AboutHomepage,
            };
            for (int i = 0; i < 2; ++i) {
                fluent::ControlState state{};
                state.enabled = true;
                state.hovered = IsHovered(vm, HitTestResult::SettingsAboutAction, i);
                painter_.DrawButton({lay.about_action[i], pulse::l10n::Get(kAboutActions[i]), {},
                                     fluent::ButtonKind::Standard, state});
            }
        }

        draw_card(lay.diagnostics_card);
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->TextFormat(), brText_.get(),
                     pulse::l10n::Get(pulse::l10n::StringId::Diagnostics),
                     lay.diagnostics_card.left + 16.0f * scale_,
                     lay.diagnostics_card.top + 12.0f * scale_,
                     lay.diagnostics_card.right - lay.diagnostics_card.left - 32.0f * scale_,
                     22.0f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        const std::wstring diagnostics_status = vm.settings_diagnostics_exporting
            ? pulse::l10n::Get(pulse::l10n::StringId::DiagnosticsExporting)
            : pulse::l10n::Get(pulse::l10n::StringId::DiagnosticsDesc);
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), diagnostics_status,
                     lay.diagnostics_card.left + 16.0f * scale_,
                     lay.diagnostics_card.top + 40.0f * scale_,
                     lay.diagnostics_card.right - lay.diagnostics_card.left - 32.0f * scale_,
                     42.0f * scale_);
        {
            const D2D1_RECT_F& row = lay.diagnostics_perf;
            const auto& description=l10n::Get(l10n::StringId::SettingsShowPerformanceDesc);
            const auto bounds=SettingsToggleBounds(row,description,scale_,&painter_);
            if (IsHovered(vm, HitTestResult::SettingsToggle, 4)) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), row.left + 4.0f * scale_, row.top,
                                row.right - row.left - 8.0f * scale_, row.bottom - row.top,
                                4.0f * scale_);
            }
            DrawIconText(bounds.icon.left,bounds.icon.top,24*scale_,24*scale_,L"\xE946",L"",theme.text_secondary,0.85f);
            const auto performance_title=FitEndEllipsis(l10n::Get(l10n::StringId::SettingsShowPerformance),
                bounds.title.right-bounds.title.left,[&](const std::wstring& value) {
                    return MeasureTextWidth(compositor_->DwriteFactory(),compositor_->TextFormat(),value);
                });
            painter_.DrawText(performance_title,bounds.title,compositor_->TextFormat(),theme.text);
            painter_.DrawWrappedCaption(description,D2D1::Point2F(bounds.description.left,bounds.description.top),
                bounds.description.right-bounds.description.left,theme.text_secondary);
            fluent::ControlState st{};
            st.checked = vm.settings_show_performance;
            st.hovered = IsHovered(vm, HitTestResult::SettingsToggle, 4);
            painter_.DrawSwitch(D2D1::RectF(row.right - 60.0f * scale_,
                                            (row.top + row.bottom - 32.0f * scale_) * 0.5f,
                                            row.right - 16.0f * scale_,
                                            (row.top + row.bottom + 32.0f * scale_) * 0.5f),
                                L"", st);
        }
        static constexpr pulse::l10n::StringId kDiagnosticsActions[] = {
            pulse::l10n::StringId::OpenDiagnostics,
            pulse::l10n::StringId::ClearDiagnostics,
            pulse::l10n::StringId::ExportDiagnostics,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::ControlState state{};
            state.enabled = !vm.settings_diagnostics_exporting;
            state.hovered = state.enabled &&
                IsHovered(vm, HitTestResult::SettingsDiagnosticsAction, i);
            painter_.DrawButton({lay.diagnostics_action[i],
                pulse::l10n::Get(kDiagnosticsActions[i]), {},
                fluent::ButtonKind::Standard, state});
        }

        if (!vm.settings_index_error.empty()) {
            MakeBrush(dc, theme.danger, brDanger_);
            DrawTextRect(dc, compositor_->SmallFormat(), brDanger_.get(),
                         vm.settings_index_error,
                         lay.content.left + pad, lay.diagnostics_card.bottom + 8.0f * scale_,
                         lay.content.right - lay.content.left - pad * 2, 36.0f * scale_);
        }

    } else if (vm.settings_page == 4) {
        using I = pulse::l10n::StringId;
        auto draw_card = [&](const D2D1_RECT_F& card) {
            if (card.right <= card.left || card.bottom <= card.top) return;
            MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), card.left, card.top,
                            card.right - card.left, card.bottom - card.top, 8.0f * scale_);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f * scale_, 8.0f * scale_),
                                     brStrokeCard_.get(), 1.0f);
        };
        auto divider = [&](const D2D1_RECT_F& row) {
            if (row.bottom <= row.top) return;
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            FillRect(dc, brStrokeDivider_.get(), row.left + 16 * scale_, row.bottom,
                row.right - row.left - 32 * scale_, 1);
        };
        auto label = [&](const D2D1_RECT_F& row, const D2D1_RECT_F& bounds,
                         I title, const wchar_t* icon) {
            DrawIconText(row.left + 16 * scale_, row.top + 17 * scale_, 24 * scale_,
                24 * scale_, icon, L"", theme.text_secondary, 0.85f);
            painter_.DrawText(pulse::l10n::Get(title), bounds, compositor_->TextFormat(), theme.text);
        };
        auto caption = [&](std::wstring_view value, const D2D1_RECT_F& bounds) {
            if (bounds.bottom <= bounds.top) return;
            painter_.DrawWrappedCaption(value, D2D1::Point2F(bounds.left, bounds.top),
                bounds.right - bounds.left, theme.text_secondary);
        };
        auto button = [&](const D2D1_RECT_F& bounds, std::wstring_view value,
                          fluent::ButtonKind kind, const fluent::ControlState& state) {
            fluent::ButtonSpec spec{bounds, value, {}, kind, state};
            spec.wrap_text = true;
            painter_.DrawButton(spec);
        };
        draw_card(lay.duplicate_options);
        label(lay.dup_scope_row, lay.dup_scope_label, I::DupScopeLabel, kIconSearch);
        caption(vm.dup_hint, lay.dup_hint);
        divider(lay.dup_scope_row);
        if (lay.dup_target_row.bottom > lay.dup_target_row.top) {
            label(lay.dup_target_row, lay.dup_target_label, I::DupTargetLabel, L"\xED25");
            divider(lay.dup_target_row);
        }
        label(lay.dup_min_row, lay.dup_min_label, I::DupMinSize, L"\xE8A4");
        caption(pulse::l10n::Get(I::DupMinSizeDesc), lay.dup_min_description);
        divider(lay.dup_min_row);
        static constexpr pulse::l10n::StringId kScope[] = {
            pulse::l10n::StringId::DupScopeFolder,
            pulse::l10n::StringId::DupScopeDrive,
            pulse::l10n::StringId::DupScopeAll,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec segment;
            segment.bounds = lay.dup_scope[i];
            if (i == 0 && !lay.dup_scope_stacked)
                painter_.DrawSegmentedTrack(D2D1::RectF(segment.bounds.left,
                    segment.bounds.top, lay.dup_scope[2].right, lay.dup_scope[2].bottom));
            segment.shared_track = !lay.dup_scope_stacked;
            segment.text = pulse::l10n::Get(kScope[i]);
            segment.position = lay.dup_scope_stacked ? fluent::SegmentPosition::Single
                             : i == 0 ? fluent::SegmentPosition::First
                             : i == 2 ? fluent::SegmentPosition::Last
                                      : fluent::SegmentPosition::Middle;
            segment.state.selected = vm.dup_scope == i;
            segment.state.enabled = !vm.dup_scanning;
            segment.state.hovered = segment.state.enabled &&
                IsHovered(vm, HitTestResult::SettingsDupScope, i);
            painter_.DrawSegmentedItem(segment);
        }
        if (vm.dup_scope == 0) {
            MakeBrush(dc, theme.text, brText_);
            const std::wstring folder = vm.dup_folder.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::DupFolderPlaceholder)
                : vm.dup_folder;
            DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(), brText_.get(), folder,
                         lay.dup_target_summary.left, lay.dup_target_summary.top,
                         lay.dup_target_summary.right - lay.dup_target_summary.left,
                         lay.dup_target_summary.bottom - lay.dup_target_summary.top);
            fluent::ControlState browse{};
            browse.enabled = !vm.dup_scanning;
            browse.hovered = browse.enabled && IsHovered(vm, HitTestResult::SettingsDupBrowse);
            button(lay.dup_browse, pulse::l10n::Get(I::DupBrowse),
                fluent::ButtonKind::Standard, browse);
        } else if (vm.dup_scope == 1) {
            for (size_t i = 0; i < vm.dup_drives.size() && i < lay.dup_drives.size(); ++i) {
                fluent::ControlState chip{};
                chip.checked = vm.dup_drives[i].selected;
                chip.selected = vm.dup_drives[i].selected;
                chip.enabled = !vm.dup_scanning;
                chip.hovered = chip.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDupDrive, static_cast<int>(i));
                button(lay.dup_drives[i], vm.dup_drives[i].label,
                    fluent::ButtonKind::Toggle, chip);
            }
        }
        static constexpr pulse::l10n::StringId kMin[] = {
            pulse::l10n::StringId::DupMinSize1KB,
            pulse::l10n::StringId::DupMinSize1MB,
            pulse::l10n::StringId::DupMinSize10MB,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec segment;
            segment.bounds = lay.dup_min_size[i];
            if (i == 0) painter_.DrawSegmentedTrack(D2D1::RectF(segment.bounds.left,
                segment.bounds.top, lay.dup_min_size[2].right, lay.dup_min_size[2].bottom));
            segment.shared_track = true;
            segment.text = pulse::l10n::Get(kMin[i]);
            segment.position = i == 0 ? fluent::SegmentPosition::First
                             : i == 2 ? fluent::SegmentPosition::Last
                                      : fluent::SegmentPosition::Middle;
            segment.state.selected = vm.dup_min_size == i;
            segment.state.enabled = !vm.dup_scanning;
            segment.state.hovered = segment.state.enabled &&
                IsHovered(vm, HitTestResult::SettingsDupMinSize, i);
            painter_.DrawSegmentedItem(segment);
        }
        fluent::ControlState scan{};
        scan.enabled = vm.dup_can_scan && !vm.dup_scanning;
        scan.hovered = scan.enabled && IsHovered(vm, HitTestResult::SettingsDupScan);
        button(lay.dup_scan, pulse::l10n::Get(I::DupScan), fluent::ButtonKind::Primary, scan);
        fluent::ControlState cancel{};
        cancel.enabled = vm.dup_scanning;
        cancel.hovered = cancel.enabled && IsHovered(vm, HitTestResult::SettingsDupCancel);
        button(lay.dup_cancel, pulse::l10n::Get(I::Cancel), fluent::ButtonKind::Standard, cancel);
        if (vm.dup_show_progress) {
            draw_card(lay.dup_progress);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), vm.dup_status,
                         lay.dup_progress.left + 16.0f * scale_, lay.dup_progress.top + 10.0f * scale_,
                         lay.dup_progress.right - lay.dup_progress.left - 32.0f * scale_,
                         18.0f * scale_);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.dup_speed,
                         lay.dup_progress.left + 16.0f * scale_, lay.dup_progress.top + 28.0f * scale_,
                         lay.dup_progress.right - lay.dup_progress.left - 32.0f * scale_,
                         16.0f * scale_);
            fluent::ProgressSpec bar;
            bar.bounds = D2D1::RectF(lay.dup_progress.left + 16.0f * scale_,
                                     lay.dup_progress.bottom - 18.0f * scale_,
                                     lay.dup_progress.right - 16.0f * scale_,
                                     lay.dup_progress.bottom - 10.0f * scale_);
            bar.indeterminate = vm.dup_progress_indeterminate;
            bar.value = vm.dup_progress_value;
            bar.animation_progress = vm.dup_animation;
            painter_.DrawProgressBar(bar);
        }
        caption(vm.dup_empty, lay.dup_empty);
        size_t visible_file = 0;
        for (size_t g = 0; g < vm.dup_groups.size() && g < lay.dup_group_cards.size(); ++g) {
            const auto& card = lay.dup_group_cards[g];
            if (!VisibleInContent(card, lay.content)) continue;
            draw_card(card);
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), vm.dup_groups[g].title,
                         card.left + 16.0f * scale_, card.top + 12.0f * scale_,
                         card.right - card.left - 32.0f * scale_, 24.0f * scale_);
            while (visible_file < lay.dup_keep.size() &&
                   lay.dup_keep_group[visible_file] < static_cast<int>(g)) ++visible_file;
            while (visible_file < lay.dup_keep.size() &&
                   lay.dup_keep_group[visible_file] == static_cast<int>(g)) {
                const size_t item = visible_file++;
                const int f = lay.dup_keep_file[item];
                const auto& keep_rc = lay.dup_keep[item];
                const auto& open_rc = lay.dup_open[item];
                if (!VisibleInContent(open_rc, lay.content) &&
                    !VisibleInContent(keep_rc, lay.content)) continue;
                const auto& file = vm.dup_groups[g].files[f];
                fluent::ControlState radio{};
                radio.checked = file.keep;
                radio.enabled = !vm.dup_scanning;
                radio.hovered = radio.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDupKeep, static_cast<int>(g),
                              static_cast<int>(f));
                painter_.DrawRadioButton(keep_rc,
                    pulse::l10n::Get(pulse::l10n::StringId::DupKeep), radio);
                MakeBrush(dc, theme.text, brText_);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(),
                    brText_.get(), file.name, open_rc.left, open_rc.top,
                    open_rc.right - open_rc.left, 18 * scale_);
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(),
                    brTextSecondary_.get(), file.detail, open_rc.left, open_rc.top + 18 * scale_,
                    open_rc.right - open_rc.left, 18 * scale_);
            }
            if (g < lay.dup_group_delete.size() &&
                VisibleInContent(lay.dup_group_delete[g], lay.content)) {
                fluent::ControlState del{};
                del.enabled = !vm.dup_scanning;
                del.hovered = del.enabled &&
                    IsHovered(vm, HitTestResult::SettingsDupGroupDelete, static_cast<int>(g));
                button(lay.dup_group_delete[g], pulse::l10n::Get(I::DupDeleteExtras),
                    fluent::ButtonKind::Danger, del);
            }
        }
        if (vm.dup_show_delete_all) {
            fluent::ControlState all{};
            all.enabled = !vm.dup_scanning;
            all.hovered = all.enabled && IsHovered(vm, HitTestResult::SettingsDupDeleteAll);
            button(lay.dup_delete_all, vm.dup_delete_all.empty()
                ? std::wstring_view(pulse::l10n::Get(I::DupDeleteAllExtras))
                : std::wstring_view(vm.dup_delete_all), fluent::ButtonKind::Danger, all);
        }
    }
    dc->PopAxisAlignedClip();

    const float view = lay.content.bottom - lay.content.top;
    if (lay.content_h > view + 1.0f) {
        fluent::ScrollbarSpec bar;
        bar.viewport = D2D1::RectF(lay.content.right - 18.0f * scale_, lay.content.top,
                                   lay.content.right - 10.0f * scale_, lay.content.bottom);
        bar.offset = vm.settings_scroll;
        bar.viewport_extent = view;
        bar.content_extent = lay.content_h;
        bar.expand_progress = 1.0f;
        painter_.DrawScrollbar(bar);
    }
}

D2D1_RECT_F MainRenderer::SettingsDropdownBounds(const WindowViewModel& vm, int index, float window_w, float window_h) const {
    const auto l=MakeSettingsLayout(vm,D2D1::RectF(0,0,window_w,window_h),scale_,title_bar_height_,status_height_,&painter_);
    return index==0 ? l.effect_choice : l.language_choice;
}

float MainRenderer::SettingsDestinationOffset(const WindowViewModel& vm, int setting_id, float window_w, float window_h) const {
    const auto l=MakeSettingsLayout(vm,D2D1::RectF(0,0,window_w,window_h),scale_,title_bar_height_,status_height_,&painter_);
    using I=l10n::StringId;
    D2D1_RECT_F target{};
    switch(static_cast<I>(setting_id)) {
    case I::SettingsTheme: target=l.theme_row;break;
    case I::SettingsThemeColor: target=l.accent_card;break;
    case I::SettingsWindowEffect: target=l.effect_card;break;
    case I::SettingsLanguage: target=l.language_card;break;
    case I::SettingsLaunch: target=l.startup_row[0];break;
    case I::SettingsKeepRunning: target=l.startup_row[1];break;
    case I::SettingsDefaultManager: target=l.startup_row[2];break;
    case I::SettingsOpenFolders: target=l.default_manager_rows[0];break;
    case I::SettingsTakeoverWinE: target=l.default_manager_rows[1];break;
    case I::ThisPc: target=l.default_manager_rows[2];break;
    case I::SettingsExplorerTakeover: target=l.default_manager_rows[3];break;
    case I::StartToTray: target=l.startup_tray_row;break;
    case I::CloseLastTabWindow: target=l.last_tab_row;break;
    case I::SettingsRowHeight: target=l.density_card;break;
    case I::SettingsShowPerformance: target=l.performance_row;break;
    case I::ListSmartDate: target=l.list_style_row[0];break;
    case I::ListZebraRows: target=l.list_style_row[1];break;
    case I::ListSizeBar: target=l.list_style_row[2];break;
    case I::SettingsFolderSort: target=l.folder_sort_card;break;
    case I::SettingsWallpaper: target=l.wallpaper_card;break;
    case I::SettingsWallpaperLook: target=l.wallpaper_look_card;break;
    case I::SettingsWallpaperBlur: target=l.wallpaper_blur_card;break;
    case I::SettingsTrayIcon: target=l.tray_icon_card;break;
    case I::SettingsShowHidden: target=l.hidden_files_row;break;
    case I::SettingsShowProtected: target=l.protected_files_row;break;
    case I::PinnedNames: target=l.pinned_names_row;break;
    case I::SettingsBlankClickBack: target=l.blank_click_row;break;
    case I::SettingsNewTabHome: target=l.new_tab_row;break;
    case I::SettingsChangeTracking: target=l.change_tracking_row;break;
    case I::GlobalSearch: target=l.global_search_row;break;
    case I::GlobalSearchHotkey: target=l.global_search_hotkey_row;break;
    case I::SearchPinyin: target=l.search_pinyin_row;break;
    case I::ContentIndexManage: target=l.content_header;break;
    case I::ConfigurationLocation: target=l.configuration_path;break;
    case I::IndexLocation: target=l.index_path;break;
    case I::LocalDrives: if(!l.index_volume_rows.empty()) target=l.index_volume_rows.front();break;
    case I::Exclusions: target=l.index_exclude_action;break;
    case I::ServerFolders: target=l.network_action[0];break;
    default: return 0;
    }
    return (std::max)(0.0f,target.top-l.content_origin-20*scale_);
}

float MainRenderer::SettingsMaxScroll(const WindowViewModel& vm, float window_w, float window_h) const {
    const D2D1_RECT_F rect = D2D1::RectF(0, 0, window_w, window_h);
    const SettingsLayout lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_,
                                                  status_height_, &painter_);
    const float view = (std::max)(0.0f, lay.content.bottom - lay.content.top);
    return (std::max)(0.0f, lay.content_h - view);
}

bool MainRenderer::SettingsScrollbarGeometry(const WindowViewModel& vm, float window_w,
                                            float window_h, D2D1_RECT_F& track,
                                            D2D1_RECT_F& thumb, float& maximum) const {
    const auto lay = MakeSettingsLayout(vm, D2D1::RectF(0, 0, window_w, window_h), scale_,
                                       title_bar_height_, status_height_, &painter_);
    fluent::ScrollbarSpec bar;
    bar.viewport = D2D1::RectF(lay.content.right - 18.0f * scale_, lay.content.top,
                              lay.content.right - 10.0f * scale_, lay.content.bottom);
    bar.offset = vm.settings_scroll;
    bar.viewport_extent = lay.content.bottom - lay.content.top;
    bar.content_extent = lay.content_h;
    bar.expand_progress = 1.0f;
    track = bar.viewport;
    thumb = fluent::ScrollbarThumbRect(bar, scale_);
    maximum = (std::max)(0.0f, bar.content_extent - bar.viewport_extent);
    return maximum > 1.0f;
}

} // namespace pulse::ui
