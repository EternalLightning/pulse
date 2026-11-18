// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "ui_renderer.h"
#include "ui_renderer_internal.h"

namespace pulse::ui {

D2D1_RECT_F MainRenderer::SettingsContentBounds(const WindowViewModel& vm, const D2D1_RECT_F& rect) const {
    const auto layout = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_,
        EffectiveSidebarWidth(rect.right));
    auto content = layout.content;
    if (vm.settings_page == 5 && layout.sysctx.pending_bar.bottom > layout.sysctx.pending_bar.top)
        content.bottom = (std::min)(content.bottom, layout.sysctx.pending_bar.top);
    return content;
}

std::vector<HitTestResult> MainRenderer::SettingsControls(const WindowViewModel& vm, const D2D1_RECT_F& rect) const {
    using H = HitTestResult;
    std::vector<H> controls;
    if (!vm.settings_open) return controls;
    const auto l = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_,
        EffectiveSidebarWidth(rect.right), true);
    auto add = [&](D2D1_RECT_F bounds, H::Region region, int index = -1, int sub_index = -1,
                   bool enabled = true, const std::wstring& identity = std::wstring()) {
        if (!enabled || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
        H target;
        target.region = region;
        target.index = index;
        target.sub_index = sub_index;
        target.control_bounds = bounds;
        target.path = identity;
        controls.push_back(std::move(target));
    };
    for (const int page : kSettingsNavOrder) add(l.nav_row[page], H::SettingsNav, page);
    add(l.nav_row[3], H::SettingsNav, 3);
    const size_t navigation_count = controls.size();
    auto toggle = [&](D2D1_RECT_F bounds, int index, bool enabled = true) {
        add(bounds, H::SettingsToggle, index, -1, enabled);
    };
    auto segments = [&](const auto& bounds, H::Region region) {
        for (size_t i = 0; i < std::size(bounds); ++i) add(bounds[i], region, static_cast<int>(i));
    };

    if (IsSettingsCategory(vm.settings_page)) {
        const int theme_values[] = {1, 2, 0};
        for (int i = 0; i < 3; ++i) add(l.theme_tile[i], H::SettingsTheme, theme_values[i]);
        add(l.accent_picker, H::SettingsAccent, 0);
        add(l.accent_system, H::SettingsAccent, 1);
        segments(l.effect_row, H::SettingsEffect);
        add(l.effect_choice, H::SettingsDropdown, 0);
        add(l.language_choice, H::SettingsDropdown, 1);
        add(l.external_open_choice, H::SettingsDropdown, 2);
        segments(l.language_segment, H::SettingsLanguage);
        segments(l.density_row, H::SettingsDensity);
        segments(l.text_render_row, H::SettingsTextRender);
        segments(l.folder_sort_row, H::SettingsFolderSort);
        segments(l.wallpaper_blur_row, H::SettingsWallpaperBlur);
        add(l.wallpaper_choose, H::SettingsWallpaper, 0);
        add(l.wallpaper_clear, H::SettingsWallpaper, 1, -1, !vm.background_image.empty());
        const auto image_size = material_.LoadedSourceSize(vm.background_image);
        add(l.wallpaper_fit, H::SettingsWallpaper, 2, -1,
            !vm.settings_lock_window_size && image_size.cx > 0 && image_size.cy > 0);
        add(l.wallpaper_look_value, H::SettingsWallpaperLook, 0);
        toggle(l.window_size_lock_row, 34);
        toggle(l.new_tab_row, 20);
        toggle(l.performance_row, 4);
        toggle(l.auto_folder_size_row, 35);
        toggle(l.staging_tray_row, 36);
        toggle(l.text_compare_row, 33);
        for (int i = 0; i < 3; ++i) toggle(l.list_style_row[i], 17 + i);
        toggle(l.hidden_files_row, 5);
        toggle(l.protected_files_row, 16);
        toggle(l.pinned_names_row, 6);
        toggle(l.blank_click_row, 7);
        toggle(l.change_tracking_row, 8);
        segments(l.change_days, H::SettingsChangeDays);
        toggle(l.startup_tray_row, 31);
        toggle(l.explorer_takeover_row, 24);
        toggle(l.file_dialog_bridge_row, 38, !vm.settings_system_pending);
        toggle(l.file_dialog_takeover_row, 37);
        toggle(l.last_tab_row, 32);
        toggle(l.startup_row[2], 21, !vm.settings_system_pending);
        for (int i = 0; i < 2; ++i) toggle(l.startup_row[i], i + 1);
        for (int i = 0; i < 2; ++i) add(l.configuration_action[i], H::SettingsConfigurationAction,
            i, -1, i == 0 || !vm.settings_storage_pending);
        for (int i = 0; i < 2 + static_cast<int>(vm.settings_transfer_pending); ++i)
            add(l.transfer_action[i], H::SettingsDiagnosticsAction, i + 3, -1, !vm.settings_transfer_busy);
    } else if (vm.settings_page == 11) {
        add(l.home_access_add, H::SettingsHomeQuickAccess, HomeAccessAddFolder);
        add(l.home_access_reset, H::SettingsHomeQuickAccess, HomeAccessReset);
        for (int bit = 0; bit < model::kHomeQuickAccessItemCount; ++bit)
            add(l.home_access_builtin[bit], H::SettingsHomeQuickAccess, HomeAccessToggle, bit);
        for (size_t i = 0; i < l.home_access_folders.size() && i < vm.settings_home_access_folders.size(); ++i) {
            const auto& row = l.home_access_folders[i];
            const auto& path = vm.settings_home_access_folders[i].path;
            add(row.up, H::SettingsHomeQuickAccess, HomeAccessMoveUp, static_cast<int>(i), i > 0, path);
            add(row.down, H::SettingsHomeQuickAccess, HomeAccessMoveDown, static_cast<int>(i), i + 1 < vm.settings_home_access_folders.size(), path);
            add(row.remove, H::SettingsHomeQuickAccess, HomeAccessRemove, static_cast<int>(i), true, path);
        }
    } else if (vm.settings_page == 10) {
        add(l.shortcut_search, H::SettingsShortcut, ShortcutFilter);
        add(l.shortcut_clear, H::SettingsShortcut, ShortcutClearFilter, -1, !vm.settings_shortcut_filter.empty());
        add(l.shortcut_reset_all, H::SettingsShortcut, ShortcutResetAll);
        add(l.global_search_hotkey_button, H::SettingsGlobalSearchHotkey);
        for (size_t i = 0; i < l.shortcut_rows.size() && i < vm.settings_shortcuts.size(); ++i) {
            const auto& row = vm.settings_shortcuts[i];
            const auto& geometry = l.shortcut_rows[i];
            add(geometry.binding, H::SettingsShortcut, ShortcutEdit, row.command);
            add(geometry.remove, H::SettingsShortcut, ShortcutRemove, row.command, !row.binding.empty());
            add(geometry.reset, H::SettingsShortcut, ShortcutReset, row.command, row.customized);
        }
    } else if (vm.settings_page == 1) {
        toggle(l.global_search_row, 15);
        toggle(l.search_pinyin_row, 9);
        add(l.content_index_row, H::SettingsContentIndex);
        for (int i = 0; i < 2; ++i) add(l.disclosure[i], H::SettingsDisclosure, i);
        add(l.content_pause, H::SettingsContentAction, 1, -1,
            !vm.settings_content_instant && !vm.settings_content_folders.empty());
        add(l.content_options, H::SettingsContentAction, 2);
        add(l.content_rebuild, H::SettingsContentAction, 3, -1,
            !vm.settings_content_instant && !vm.settings_content_folders.empty());
        for (int i = 0; i < ((vm.settings_index_service || vm.settings_index_installed) ? 3 : 4); ++i)
            add(l.index_action[i], H::SettingsIndexAction, i, -1,
                (i != 0 || vm.settings_index_service) && (!vm.settings_index_migrating || i == 1));
        for (size_t i = 0; i < l.index_volume_rows.size() && i < vm.settings_index_volumes.size(); ++i) {
            const auto& volume = vm.settings_index_volumes[i];
            add(l.index_volume_rows[i], H::SettingsIndexVolume, static_cast<int>(i), -1,
                volume.enabled && !volume.pending, volume.id);
        }
        add(l.index_exclude_action, H::SettingsIndexExcludeAction, 0, -1, vm.settings_index_service);
        for (size_t i = 0; i < l.index_exclude_remove.size() && i < vm.settings_index_excluded_paths.size(); ++i)
            add(l.index_exclude_remove[i], H::SettingsIndexExcludeRemove, static_cast<int>(i), -1,
                vm.settings_index_service, vm.settings_index_excluded_paths[i]);
        for (int i = 0; i < 2; ++i) add(l.network_action[i], H::SettingsNetworkAction, i, -1,
            i == 0 || !vm.settings_network_roots.empty());
        for (size_t i = 0; i < l.network_remove.size() && i < vm.settings_network_roots.size(); ++i)
            add(l.network_remove[i], H::SettingsNetworkRemove, static_cast<int>(i), -1,
                true, vm.settings_network_roots[i].path);
    } else if (vm.settings_page == 2) {
        add(l.ctx_layer[1], H::SettingsCtxPivot, 5);
        // Presets and tabs use the legacy SettingsToggle action indices.
        for (int i = 0; i < 3; ++i) {
            toggle(l.pulse_preset[i], 60 + i);
            toggle(l.pulse_tab[i], 64 + i);
        }
        bool can_restore = vm.settings_builtin_hidden != 0;
        for (int i = 0; i < static_cast<int>(model::BuiltinMenuSurface::Count); ++i)
            can_restore |= PulseMenuOrderCustom(vm, static_cast<model::BuiltinMenuSurface>(i));
        toggle(l.pulse_restore, 67, can_restore);
        toggle(l.pulse_order_reset, kSettingsMenuOrderReset);
        for (size_t i = 0; i < l.context_rows.size() && i < vm.settings_items.size(); ++i)
            add(l.context_rows[i], H::SettingsToggle, 100 + static_cast<int>(i), -1,
                true, vm.settings_items[i].key);
        auto still = vm;
        still.settings_menu_drag = -1;
        for (const auto& row : LayoutPulsePreview(still, PulsePreviewBox(l.pulse_preview, scale_), scale_).rows)
            if (row.row.fixed == model::BuiltinFixedRow::None)
                toggle(row.rect, PulseMenuRowHit(row.row));
    } else if (vm.settings_page == 3) {
        segments(l.about_action, H::SettingsAboutAction);
        toggle(l.diagnostics_perf, 4);
        for (int i = 0; i < 3; ++i) add(l.diagnostics_action[i], H::SettingsDiagnosticsAction,
            i, -1, !vm.settings_diagnostics_exporting);
    } else if (vm.settings_page == 4) {
        for (int i = 0; i < 3; ++i) {
            add(l.dup_scope[i], H::SettingsDupScope, i, -1, !vm.dup_scanning);
            add(l.dup_min_size[i], H::SettingsDupMinSize, i, -1, !vm.dup_scanning && vm.dup_skip_small_files);
        }
        add(l.dup_browse, H::SettingsDupBrowse, -1, -1, !vm.dup_scanning);
        for (size_t i = 0; i < l.dup_drives.size() && i < vm.dup_drives.size(); ++i)
            add(l.dup_drives[i], H::SettingsDupDrive, static_cast<int>(i), -1,
                !vm.dup_scanning, vm.dup_drives[i].root);
        add(l.dup_skip_small_row, H::SettingsDupSkipSmallFiles, -1, -1, !vm.dup_scanning);
        add(l.dup_scan, H::SettingsDupScan, -1, -1, vm.dup_can_scan && !vm.dup_scanning);
        add(l.dup_cancel, H::SettingsDupCancel, -1, -1, vm.dup_scanning);
        for (size_t i = 0; i < l.dup_keep.size(); ++i) {
            const int group = l.dup_keep_group[i], file = l.dup_keep_file[i];
            const auto& path = vm.dup_groups[static_cast<size_t>(group)].files[static_cast<size_t>(file)].path;
            add(l.dup_keep[i], H::SettingsDupKeep, group, file, !vm.dup_scanning, path);
            add(l.dup_open[i], H::SettingsDupOpen, group, file, true, path);
        }
        std::wstring all_groups;
        for (size_t i = 0; i < vm.dup_groups.size(); ++i) {
            std::wstring identity;
            for (const auto& file : vm.dup_groups[i].files) identity += file.path + L'\n';
            all_groups += identity;
            add(l.dup_group_ignore[i], H::SettingsDupGroupIgnore, static_cast<int>(i), -1,
                !vm.dup_scanning, identity);
            add(l.dup_group_delete[i], H::SettingsDupGroupDelete, static_cast<int>(i), -1,
                !vm.dup_scanning, identity);
        }
        add(l.dup_delete_all, H::SettingsDupDeleteAll, -1, -1,
            vm.dup_show_delete_all && !vm.dup_scanning, all_groups);
    } else if (vm.settings_page == 5) {
        const auto& o = l.sysctx;
        const auto* v = vm.settings_sysctx.get();
        auto sys = [&](D2D1_RECT_F bounds, int action, bool enabled = true,
                       const std::wstring& identity = std::wstring()) {
            add(bounds, H::SettingsSysCtx, action, -1, enabled, identity);
        };
        sys(o.local_card, kSysCtxLocalLayer);
        if (v) {
            const bool ready = v->state == SysCtxView::State::Ready;
            sys(o.undo, kSysCtxUndo, ready && v->undo_steps > 0);
            sys(o.log, kSysCtxLog);
            sys(o.restore, kSysCtxRestoreAll, ready && v->undo_steps > 0);
            sys(o.overview_row, kSysCtxOverview);
            for (size_t i = 0; i < o.scenes.size(); ++i)
                sys(o.scenes[i], kSysCtxSceneBase + static_cast<int>(i));
            sys(o.refresh, kSysCtxRefresh, ready);
            sys(o.clean, kSysCtxClean, ready);
            for (size_t i = 0; i < o.filters.size(); ++i)
                sys(o.filters[i], kSysCtxFilterBase + static_cast<int>(i));
            for (size_t i = 0; i < o.subs.size() && i < v->subs.size(); ++i)
                sys(o.subs[i], kSysCtxSubBase + static_cast<int>(i), true, v->subs[i]);
            for (size_t i = 0; i < o.rows.size() && i < v->rows.size(); ++i) {
                const auto& row = v->rows[i];
                auto selection = o.rows[i];
                selection.bottom = selection.top + kSysCtxRowDip * scale_;
                selection.right = o.more[i].left;
                if (row.deletable) selection.right = o.removes[i].left;
                else if (SysCtxSwitchable(row)) selection.right = o.switches[i].left;
                sys(selection, kSysCtxSelectBase + static_cast<int>(i), true, row.registry);
                sys(o.more[i], kSysCtxMoreBase + static_cast<int>(i), true, row.registry);
                sys(o.removes[i], kSysCtxRemoveBase + static_cast<int>(i), ready, row.registry);
                sys(o.switches[i], kSysCtxRowBase + static_cast<int>(i),
                    ready && SysCtxSwitchable(row) && !row.deletable, row.registry);
            }
            sys(o.tab_preview, kSysCtxTabPreview);
            sys(o.tab_detail, kSysCtxTabDetail);
            if (v->selected >= 0 && v->selected < static_cast<int>(v->rows.size()) &&
                (!o.wide || v->side_tab == 1)) {
                const auto& identity = v->rows[static_cast<size_t>(v->selected)].registry;
                sys(o.detail.toggle, kSysCtxDetailSwitch, ready, identity);
                sys(o.detail.pulse_toggle, kSysCtxDetailPulse, ready, identity);
                sys(o.detail.copy, kSysCtxDetailCopy, true, identity);
                sys(o.detail.folder, kSysCtxDetailFolder, true, identity);
                sys(o.detail.remove, kSysCtxDetailDelete, ready, identity);
            }
            if (o.wide && v->side_tab == 0 && !v->overview.shown) {
                const auto preview = LayoutSysCtxPreview(*v, o.side_body, o.scale, false);
                for (size_t i = 0; i < preview.items.size(); ++i)
                    sys(preview.items[i], kSysCtxPreviewBase + static_cast<int>(i), ready);
            }
            const auto& ov = o.overview;
            for (size_t i = 0; i < ov.slow_skip.size() && i < v->overview.slow_rows.size(); ++i) {
                const auto& identity = v->overview.slow_rows[i].clsid;
                sys(ov.slow_skip[i], kSysCtxSlowSkipBase + static_cast<int>(i), ready, identity);
                sys(ov.slow_toggle[i], kSysCtxSlowToggleBase + static_cast<int>(i), ready, identity);
            }
            for (size_t i = 0; i < ov.mark_restore.size() && i < v->overview.mark_rows.size(); ++i)
                sys(ov.mark_restore[i], kSysCtxMarkRestoreBase + static_cast<int>(i), ready,
                    v->overview.mark_rows[i].registry);
            sys(o.apply, kSysCtxApply, ready);
            sys(o.discard, kSysCtxDiscard, ready);
        }
    }
    std::stable_sort(controls.begin() + navigation_count, controls.end(), [](const H& a, const H& b) {
        return a.control_bounds.top != b.control_bounds.top
            ? a.control_bounds.top < b.control_bounds.top : a.control_bounds.left < b.control_bounds.left;
    });
    return controls;
}

} // namespace pulse::ui
