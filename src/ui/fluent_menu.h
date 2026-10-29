// fluent_menu.h — Fluent context menu / dropdown (ui.md §4 右键菜单/下拉, §6 动效).
//
// Rendering: a WS_EX_LAYERED popup with a sampled acrylic plate and an opaque
// fallback. Painter draws the surface and items into an
// offscreen D2D bitmap using the main compositor's device.
//
// Behavior: TrackPopup runs a nested modal loop (like TrackPopupMenu):
// hover tracking, keyboard nav (Up/Down/Enter/Esc), click-outside dismiss,
// 200ms OutCubic fade-in + 8px slide, 140ms InQuad fade-out, matching
// the operation center and respecting Windows' reduced-motion preference.
// The same component serves the right-click menu and the toolbar "新建▾"
// dropdown.
//
// FluentMenuModel is the windowless layout/hit-test half; the console
// self-test drives it directly with a standalone DWrite factory.
#pragma once
#include "menu_backdrop.h"
#include "fluent_components.h"
#include "smooth_scroll.h"

#include <dwrite_3.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

struct FluentMenuSwatch {
    int command = 0;
    D2D1_COLOR_F color{};
    bool checked = false;
    bool mixed = false;
    // Non-empty => icon button (Segoe Fluent Icons codepoint) instead of a
    // tag color dot: wider hit target, hover pill, theme text color.
    std::wstring glyph;
};

struct FluentMenuItem {
    int command = 0;              // app-defined verb id; 0 is never returned
    std::wstring text;
    std::wstring glyph;           // Segoe Fluent Icons codepoint string
    std::wstring shortcut;        // right-aligned caption, e.g. L"Ctrl+Shift+C"
    std::wstring badge_text;      // right-aligned Fluent badge (takes priority over shortcut)
    bool enabled = true;
    bool live_update = false;     // Async section or flyout; may refresh its children in place.
    bool separator_after = false;
    bool has_swatch = false;
    bool checked = false;
    bool mixed = false;
    bool toggle = false;
    bool secondary = false;
    bool radio = false;            // selected single-choice item: filled dot, never a checkmark
    bool radio_group = false;      // reserve a leading selection-dot column (left of the icon)
    fluent::MenuPictogram pictogram = fluent::MenuPictogram::None;
    D2D1_COLOR_F swatch_color{};
    std::vector<FluentMenuSwatch> quick_swatches;
    // Non-empty => a flyout header. Hover/click/→ opens its children;
    // the header itself never returns a command. Children may nest.
    std::vector<FluentMenuItem> children;
    float glyph_scale = 1.0f;      // Per-item visual scale inside the fixed icon slot.
    bool shortcut_inline = false; // Search paths follow a shared, compact filename column.
    int trailing_command = 0;     // Optional hover-revealed close action at the right edge.
    std::wstring tooltip;
};

// Windowless menu layout + hit-testing (theme.row_menu = 36 DIP rows).
class FluentMenuModel {
public:
    void SetItems(std::vector<FluentMenuItem> items);
    // Measures text; dwrite may be nullptr (falls back to an estimate).
    void Layout(IDWriteFactory2* dwrite, float scale, float min_width_px = 0,
                Compositor* compositor = nullptr);

    int WidthPx() const { return width_; }
    int HeightPx() const { return height_; }
    float InlineLabelWidthPx() const { return inline_label_width_; }
    float RowHeightPx() const { return row_h_; }
    int Count() const { return (int)items_.size(); }
    const FluentMenuItem* At(int i) const;
    bool LabelTruncated(int i) const;
    std::wstring TooltipText(int i) const;
    // Copy command ids and enabled states when text/structure matches. No layout.
    bool PatchCommands(const std::vector<FluentMenuItem>& src);
    bool ReplaceLiveItems(const std::vector<FluentMenuItem>& src);
    // Refresh one live command without moving rows or resizing an open popup.
    bool UpdateCommandState(int command, const std::wstring& text, bool enabled);
    // Content y (px) of row i's top edge.
    float RowTopPx(int i) const;
    int HitTestRow(float y_px) const;                 // -1 = not on a row
    int NextEnabled(int from, int dir) const;         // wraps; -1 if none
    int FirstEnabled() const { return NextEnabled(-1, 1); }

private:
    std::vector<FluentMenuItem> items_;
    std::vector<bool> label_truncated_;
    float scale_ = 1.0f;
    float row_h_ = 32.0f;
    float pad_v_ = 4.0f;      // surface inner padding (DIP*scale)
    float sep_h_ = 5.0f;      // separator slot height (line + gaps)
    float inline_label_width_ = 0.0f;
    int width_ = 0;
    int height_ = 0;
};

// The popup window. One instance per app, reused across invocations.
class FluentMenu {
    friend struct FluentMenuTestPeer;
public:
    FluentMenu() = default;
    ~FluentMenu();
    FluentMenu(const FluentMenu&) = delete;
    FluentMenu& operator=(const FluentMenu&) = delete;

    // owner: main window. compositor supplies the D2D device + DWrite factory
    // (drawing happens on the UI thread between its own BeginDraw/EndDraw).
    bool Create(HWND owner, Compositor* compositor, float scale);
    void SetTheme(bool dark, D2D1_COLOR_F accent);

    // Modal; returns the invoked command id, or 0 when dismissed.
    // If filter is set, typing rebuilds the item list (Ctrl+K command palette).
    using FilterFn = std::function<std::vector<FluentMenuItem>(const std::wstring& query)>;
    // Return true after handling a command to keep the same popup open.
    using CommandFn = std::function<bool(int command)>;
    int TrackPopup(POINT screen_pt, std::vector<FluentMenuItem> items,
                   FilterFn filter = nullptr, bool top_center = false, CommandFn command = nullptr);
    int TrackToolbarPopup(POINT screen_pt, std::vector<FluentMenuItem> items);
    int TrackDropdown(RECT control_rect, std::vector<FluentMenuItem> items);

    // Renders the menu to a PNG without showing it (GUI verification helper).
    bool SaveDebugSnapshot(const wchar_t* png_path, std::vector<FluentMenuItem> items,
                           int hover_row = -1);

    bool IsOpen() const { return open_; }
    void Dismiss(); // immediate (no animation); safe anytime
    void RequestDismiss(); // normal user dismissal; retains the surface through its exit animation
    void DismissForDeactivation(HWND activated);
    // Safe from the owner window while TrackPopup's modal loop is running
    // (e.g. async index hits arrived). No-op if the menu is closed.
    void RequestFilterRefresh();
    // Replace filtered results in place, preserving editor, animation and scroll.
    bool ReplaceFilterItems(std::vector<FluentMenuItem> items);
    // Swap command ids while the menu is open if the visible structure matches.
    // Stable built-in rows keep their identity; async sections and flyout children
    // can refresh without closing the selected parent. Returns true on success.
    bool ReplaceItems(std::vector<FluentMenuItem> items);
    bool UpdateCommandState(int command, const std::wstring& text, bool enabled);
    void SetFilterPlaceholder(std::wstring text) { filter_placeholder_ = std::move(text); }
    const std::wstring& LastFilterQuery() const { return filter_query_; }
    // True when the last TrackPopup closed because Enter was pressed with no
    // highlighted command (omnibar: commit the typed query).
    bool LastFilterCommitted() const { return filter_committed_; }
    // Prefill the filter edit on the next TrackPopup (used as a rename field).
    void SetInitialFilterText(std::wstring text) { initial_filter_text_ = std::move(text); }
    void SetSelectAllOnOpen(bool select_all) { select_all_on_open_ = select_all; }
    void SetHoverFirstOnOpen(bool hover_first) { hover_first_on_open_ = hover_first; }
    // Align the next TrackPopup to this screen rect (address bar). The filter
    // field covers the rect; results hang below. Cleared after the popup.
    void SetAnchorRect(RECT screen_rc) { anchor_rect_ = screen_rc; anchor_to_rect_ = true; }
    // Align an ordinary selection menu below its control; cleared after use.
    void SetDropdownRect(RECT screen_rc) { dropdown_rect_ = screen_rc; dropdown_ = true; }
    // Consume a repeat left press on this popup's originating control without
    // forwarding it back to the owner and immediately reopening the menu.
    void SetToggleRect(RECT screen_rc) { toggle_rect_ = screen_rc; }
    // Override the filter-mode minimum width (dips) for the next TrackPopup;
    // <= 0 restores the palette default. Reset after each popup.
    void SetFilterMinWidth(float dips) { filter_min_width_ = dips; }
    // Limit the next popup's body to this many rows; wheel/keys reveal the rest.
    // Non-positive values preserve the ordinary unconstrained menu behavior.
    void SetMaxVisibleRows(int rows) { max_visible_rows_ = rows > 0 ? rows : 0; }

    // Borrow the search editor; show only results below its screen rect.
    void SetExternalFilterEdit(HWND edit) { external_edit_ = edit; }
    // Owner hosts a topmost overlay (system preview handler) over the area the
    // menu opens into: place the popup in the topmost band so it stays visible,
    // and dismiss on clicks that land in windows of other threads, which the
    // modal loop cannot otherwise observe. Sticky until changed.
    void SetTopmost(bool topmost) { topmost_ = topmost; }

    static constexpr int kShadowMargin = 20; // px of transparent border around the card

private:
    // ULW staging buffer (GDI DIB); the main menu and every flyout own one.
    struct Surface {
        MenuBackdrop backdrop;
        HDC mem_dc = nullptr;
        HBITMAP dib = nullptr;
        void* bits = nullptr;
        int w = 0;
        int h = 0;
    };

    bool EnsureWindow();
    bool EnsureFilterEdit();
    void PlaceFilterEdit(int y_offset_px);
    void HideFilterEdit();
    void SyncFilterFromEdit();
    bool IsPaletteHwnd(HWND hwnd) const;
    bool RenderSurface(const FluentMenuModel& model, int hover_row, int hover_swatch,
                       float header_px, bool draw_filter_field, Surface& s);
    bool Render();                 // model_ -> dib_ (pixels), current hover state
    void Present(BYTE alpha, int y_offset_px);
    void PresentCurrent();
    void PositionSubmenu(bool show = true, int depth = 0);
    bool EnsureSubWindow(int depth);
    void OpenSubmenu(int row, int depth = 0);
    void CloseSubmenu(int depth = 0);
    void OnSubMouse(POINT client_pt, bool button_up, int depth = 0);
    bool RenderSub(int depth = 0);
    void PresentSub(BYTE alpha, int depth = 0);
    void HideSubWindow(int depth = 0);
    int SubmenuDepth(HWND window) const;
    void ScheduleSubmenu(int row, int depth);
    bool HandleMenuKey(WPARAM key);
    void RevealSubRow(int depth);
    void RefreshSubmenus();
    void LayoutWindow(POINT screen_pt);
    void RefreshFilter();
    int RunModalLoop();
    bool AnimationsEnabled() const;
    void BeginDismiss();
    bool TickAnimation();
    bool TickScroll(uint64_t now);
    void StopScroll();
    void OnMouse(POINT client_pt, bool button_up);
    void UpdateHover(int row, int swatch = -1);
    float BodyHeightPx() const;
    void UpdateTooltip(int row, int depth = -1);
    int HitTestSwatch(int row, float client_x) const;
    int HitTestSwatch(const FluentMenuModel& model, int row, float client_x) const;
    int InvokeRow(int row);        // returns command or 0
    void InvokeCommand(int command);
    int InvokeAt(int row, float client_x) const;
    int InvokeAt(const FluentMenuModel& model, int row, float client_x) const;
    float FilterHeaderPx() const;
    bool AppendFilterChar(wchar_t ch);
    bool HandleFilterKey(UINT msg, WPARAM wParam);
    void PasteFilterText();

    static LRESULT CALLBACK MenuWndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ExternalFilterEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK FilterEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    Compositor* compositor_ = nullptr;
    fluent::Painter painter_;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool snapshot_ = false;
    MenuBackdropSource backdrop_source_;
    D2D1_COLOR_F accent_{};

    FluentMenuModel model_;
    FilterFn filter_fn_;
    CommandFn command_fn_;
    std::wstring filter_query_;
    std::wstring filter_placeholder_;
    std::wstring initial_filter_text_;   // prefilled into the edit on open
    float filter_min_width_ = 0.0f;      // dips; <= 0 = palette default (440)
    bool select_all_on_open_ = true;
    bool hover_first_on_open_ = true;
    bool filter_committed_ = false;
    bool filter_composing_ = false;
    bool forward_tab_key_ = false;
    RECT anchor_rect_{};
    bool anchor_to_rect_ = false;
    RECT dropdown_rect_{};
    bool dropdown_ = false;
    RECT toggle_rect_{};
    POINT popup_pt_{};
    bool top_center_ = false;
    bool topmost_ = false;
    HWND external_edit_ = nullptr; // borrowed; never moved, hidden or destroyed
    HWND edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    int present_offset_ = 0;
    BYTE present_alpha_ = 0;
    uint64_t present_count_ = 0;
    int hover_row_ = -1;
    int hover_swatch_ = -1;
    int max_visible_rows_ = 0;
    float scroll_y_ = 0.0f;
    SmoothScroll scroll_motion_;
    float body_limit_px_ = 0.0f;
    HWND tooltip_ = nullptr;
    std::wstring tooltip_text_;
    bool open_ = false;
    bool animating_out_ = false;
    bool retract_on_dismiss_ = false;
    int result_ = 0;
    int base_x_ = 0;               // window position before slide animation
    int base_y_ = 0;
    uint64_t anim_start_ = 0;
    BYTE dismiss_alpha_ = 0;
    int dismiss_offset_ = 0;
    int motion_override_ = -1; // Isolated fixture override; production follows Windows.
    std::vector<MSG> deferred_clicks_;

    Surface surf_;                 // main menu staging

    struct Flyout {
        ~Flyout();
        HWND hwnd = nullptr;
        Surface surface;
        MenuBackdropSource backdrop_source;
        FluentMenuModel model;
        int parent_row = -1;
        std::wstring parent_text;
        int hover = -1;
        int hover_swatch = -1;
        float scroll_y = 0.0f;
        SmoothScroll scroll_motion;
        float body_height = 0.0f;
        int x = 0;
        int y = 0;
    };
    // Stable allocations: opening a descendant must not invalidate its parent.
    std::vector<std::unique_ptr<Flyout>> flyouts_;
    int open_flyouts_ = 0;
    int sub_pending_row_ = -1;     // hover-to-open delay target
    int sub_pending_depth_ = 0;
    std::chrono::steady_clock::time_point sub_pending_since_;
};

} // namespace pulse::ui
