// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "binary_transition.h"
#include "file_picker_listing.h"
#include "fluent_components.h"
#include "smooth_scroll.h"

#include <array>
#include <string_view>

namespace pulse::ui {

class MainRenderer;

class FilePickerTree {
public:
    struct Target {
        std::wstring directory;
        bool pulse_folder = false;
        bool native_folder = false;
    };

    ~FilePickerTree();
    void Initialize(HWND window, model::FileVisibility visibility, bool pulse_folder, const PickerPlacesLoader& load_places);
    void Layout(D2D1_RECT_F bounds, float scale, bool animate);
    void MarkInitialDirectory(std::wstring_view directory, bool animate);
    bool ApplyListing(UINT_PTR token, bool animate);
    const std::shared_ptr<const model::PlacesCatalog>& Catalog() const { return catalog_; }
    bool Draw(Compositor& compositor, MainRenderer& renderer, fluent::Painter& painter, const Theme& theme,
              bool enabled, bool focused, bool high_contrast,
              std::optional<D2D1_POINT_2F> pointer, uint64_t now, bool animate);
    void Focus(bool animate);
    std::optional<Target> Key(WPARAM key, bool animate);
    void MouseDown(float x, float y, bool high_contrast);
    void MouseMove(float y, bool high_contrast);
    std::optional<Target> MouseUp(float x, float y, bool animate);
    void CancelPointer();
    void Scroll(float wheel_steps, UINT lines, bool animate);
    bool Tick(uint64_t now, bool animate);
    bool Pending() const { return scroll_.Pending(); }
    void Stop() { scroll_.Reset(); scrollbar_hover_ = {}; ++draw_frame_; CancelPointer(); }

private:
    enum Group { OneDrive, OpenTabs, QuickAccess, Devices, GroupCount };

    struct Node {
        std::wstring label;
        std::wstring glyph;
        D2D1_COLOR_F icon_color{};
        bool use_path_icon = false;
        Target target;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
        Listing listing;
        BinaryTransition hover, disclosure;
        uint64_t draw_frame = 0;
        float top = 0.0f;
        bool divider = false;
        bool pinned = false;
        bool shortcut = false;
        bool expanded = false;
        bool loaded = false;
        bool failed = false;

        bool Expandable() const { return !target.pulse_folder && !target.native_folder && !pinned && !shortcut && (!loaded || !children.empty()); }
    };

    Node& AddRoot(Group group, PickerPlace place);
    void ApplyPlaces(PickerPlaces places);
    float Viewport() const;
    float Maximum() const;
    size_t FirstRowEndingAfter(float y) const;
    D2D1_RECT_F RowBounds(size_t row) const;
    D2D1_RECT_F DisclosureBounds(const Node& node, size_t row) const;
    fluent::ScrollbarSpec Scrollbar() const;
    Node* Hit(float x, float y, bool* disclosure = nullptr) const;
    bool OnScrollbar(float x, float y) const;
    void DragScrollbar(float y, bool high_contrast);
    void Fetch(Node& node);
    void Toggle(Node& node, bool animate);
    Target Activate(Node& node);
    void RebuildRows(bool animate);
    void AppendRows(Node& node);
    void SyncPath(bool animate);
    void Reveal(Node* node, bool animate);

    std::array<std::vector<std::unique_ptr<Node>>, GroupCount> roots_;
    std::vector<Node*> rows_;
    std::vector<Node*> pending_;
    Listing places_request_;
    std::shared_ptr<const model::PlacesCatalog> catalog_;
    Node* computer_ = nullptr;
    Node* selected_ = nullptr;
    Node* cursor_ = nullptr;
    Node* pressed_ = nullptr;
    HWND window_ = nullptr;
    D2D1_RECT_F bounds_{};
    SmoothScroll scroll_;
    BinaryTransition scrollbar_hover_;
    uint64_t draw_frame_ = 0;
    std::wstring directory_;
    float scale_ = 1.0f, offset_ = 0.0f, drag_offset_ = 0.0f, content_height_ = 0.0f;
    float last_draw_offset_ = 0.0f;
    model::FileVisibility visibility_;
    bool pulse_folder_ = false, mark_initial_directory_ = false, resolving_initial_directory_ = false;
    bool pressed_disclosure_ = false;
    bool dragging_ = false;
};

} // namespace pulse::ui
