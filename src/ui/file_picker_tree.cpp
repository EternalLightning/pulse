// SPDX-License-Identifier: Apache-2.0
#include "file_picker_tree.h"

#include "animation_clock.h"
#include "ui_compositor.h"
#include "../common/localization.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr float kRowDip = 30.0f;
constexpr float kGroupGapDip = 12.0f;
constexpr float kDisclosureDip = 20.0f;
constexpr float kScrollbarDip = 10.0f;

bool Contains(D2D1_RECT_F rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

bool IsWithin(std::wstring_view path, std::wstring_view root) {
    return !root.empty() && path.size() >= root.size() &&
        (path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\') &&
        CompareStringOrdinal(path.data(), static_cast<int>(root.size()),
            root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

FilePickerTree::~FilePickerTree() {
    for (const auto* node : pending_) node->listing->cancelled.store(true, std::memory_order_relaxed);
    if (places_request_) places_request_->cancelled.store(true, std::memory_order_relaxed);
}

FilePickerTree::Node& FilePickerTree::AddRoot(Group group, PickerPlace place) {
    auto node = std::make_unique<Node>();
    node->label = std::move(place.label);
    node->glyph = std::move(place.glyph);
    node->target.directory = std::move(place.directory);
    return *roots_[group].emplace_back(std::move(node));
}

void FilePickerTree::ApplyPlaces(PickerPlaces places) {
    for (auto& place : places.onedrive) AddRoot(OneDrive, std::move(place));
    for (auto& place : places.quick_access) AddRoot(QuickAccess, std::move(place)).pinned = true;
    if (pulse_folder_) {
        AddRoot(QuickAccess, {l10n::Get(l10n::StringId::PickerPulseFolder), {}, L"\xE8B7"}).target.pulse_folder = true;
    }
    for (auto& place : places.network) AddRoot(Devices, std::move(place));
}

void FilePickerTree::Initialize(HWND window, bool show_hidden, bool pulse_folder, const PickerPlacesLoader& load_places) {
    window_ = window;
    show_hidden_ = show_hidden;
    pulse_folder_ = pulse_folder;
    computer_ = &AddRoot(Devices, {l10n::Get(l10n::StringId::ThisPc), {}, L"\xE977"});
    computer_->expanded = true;
    Fetch(*computer_);
    if (load_places) {
        auto request = std::make_shared<ListingRequest>();
        request->window = window_;
        request->load_places = load_places;
        if (SubmitPickerListing(request)) places_request_ = std::move(request);
    }
    if (!places_request_) ApplyPlaces({});
    RebuildRows(false);
}

void FilePickerTree::Layout(D2D1_RECT_F bounds, float scale, bool animate) {
    if (scale_ != scale) {
        offset_ *= scale / scale_;
        scroll_.Reset();
    }
    scale_ = scale;
    bounds_ = bounds;
    Tick(AnimationTimeMs(), animate);
}

void FilePickerTree::Navigate(std::wstring_view directory, bool animate) {
    directory_ = directory;
    navigated_ = true;
    SyncPath(animate);
}

void FilePickerTree::Fetch(Node& node) {
    if (node.loaded || node.listing || node.failed || !node.Expandable()) return;
    auto request = std::make_shared<ListingRequest>();
    request->window = window_;
    request->directory = node.target.directory;
    request->mode = PickerMode::Folder;
    request->show_hidden = show_hidden_;
    node.failed = !SubmitPickerListing(request);
    if (node.failed) return;
    node.listing = std::move(request);
    pending_.push_back(&node);
}

bool FilePickerTree::ApplyListing(UINT_PTR token, bool animate) {
    const bool places_ready = places_request_ && (!token || places_request_->token == token) &&
        places_request_->ready.load(std::memory_order_acquire);
    if (places_ready) {
        ApplyPlaces(std::move(places_request_->places));
        places_request_.reset();
    }
    bool changed = false;
    std::erase_if(pending_, [&](Node* node) {
        if ((token && node->listing->token != token) ||
            !node->listing->ready.load(std::memory_order_acquire)) return false;
        auto request = std::move(node->listing);
        changed = true;
        node->loaded = request->succeeded;
        node->failed = !node->loaded;
        if (node->loaded) {
            node->target.directory = std::move(request->directory);
            const auto& directory = node->target.directory;
            const auto prefix = directory.empty() || directory.back() == L'\\' ? directory : directory + L'\\';
            for (auto& entry : request->entries) {
                auto child = std::make_unique<Node>();
                child->label = std::move(entry.name);
                child->target.directory = prefix + child->label;
                child->parent = node;
                child->glyph = node == computer_ ? L"\xEDA2" : L"\xE8B7";
                node->children.push_back(std::move(child));
            }
        }
        return true;
    });
    if (changed || places_ready) {
        if (reveal_ || (places_ready && navigated_)) SyncPath(animate);
        else RebuildRows(animate);
    }
    return changed || places_ready;
}

void FilePickerTree::SyncPath(bool animate) {
    if (!computer_) return;
    auto* node = computer_;
    for (const auto& group : roots_) for (const auto& root : group) {
        const auto& target = root->target.directory;
        if (target.size() > node->target.directory.size() &&
            (!root->pinned || target.size() == directory_.size()) && IsWithin(directory_, target)) node = root.get();
    }
    while (node && node->target.directory.size() != directory_.size()) {
        node->expanded = true;
        Fetch(*node);
        if (node->listing) break;
        const auto child = std::find_if(node->children.begin(), node->children.end(),
            [&](const auto& item) { return IsWithin(directory_, item->target.directory); });
        if (child == node->children.end()) { node = nullptr; break; }
        node = child->get();
    }
    selected_ = node && node->target.directory.size() == directory_.size() ? node : nullptr;
    reveal_ = !selected_ && node && node->listing;
    RebuildRows(animate);
    if (selected_) {
        cursor_ = selected_;
        Reveal(cursor_, animate);
    }
}

void FilePickerTree::AppendRows(Node& node) {
    node.top = content_height_;
    content_height_ += kRowDip;
    rows_.push_back(&node);
    if (node.expanded) for (const auto& child : node.children) AppendRows(*child);
}

void FilePickerTree::RebuildRows(bool animate) {
    rows_.clear();
    content_height_ = 0.0f;
    for (const auto& group : roots_) {
        if (group.empty()) continue;
        group.front()->divider = !rows_.empty();
        if (group.front()->divider) content_height_ += kGroupGapDip;
        for (const auto& root : group) AppendRows(*root);
    }
    while (cursor_ && std::find(rows_.begin(), rows_.end(), cursor_) == rows_.end()) cursor_ = cursor_->parent;
    Tick(AnimationTimeMs(), animate);
}

void FilePickerTree::Toggle(Node& node, bool animate) {
    if (!node.Expandable()) return;
    node.expanded = !node.expanded;
    if (node.expanded) {
        node.failed = false;
        Fetch(node);
    } else reveal_ = false;
    RebuildRows(animate);
    Reveal(cursor_, animate);
}

float FilePickerTree::Viewport() const { return std::max(0.0f, bounds_.bottom - bounds_.top); }

float FilePickerTree::Maximum() const {
    return std::max(0.0f, content_height_ * scale_ - Viewport());
}

size_t FilePickerTree::FirstRowEndingAfter(float y) const {
    const float dip = (y - bounds_.top + offset_) / scale_;
    const auto found = std::upper_bound(rows_.begin(), rows_.end(), dip,
        [](float height, const Node* node) { return height < node->top + kRowDip; });
    return static_cast<size_t>(found - rows_.begin());
}

D2D1_RECT_F FilePickerTree::RowBounds(size_t row) const {
    const float height = kRowDip * scale_;
    const float top = bounds_.top + rows_[row]->top * scale_ - offset_;
    return D2D1::RectF(bounds_.left, top, bounds_.right - kScrollbarDip * scale_, top + height);
}

D2D1_RECT_F FilePickerTree::DisclosureBounds(const Node& node, size_t row) const {
    auto bounds = RowBounds(row);
    int depth = 0;
    for (auto* parent = node.parent; parent; parent = parent->parent) ++depth;
    const float indent = std::max(0.0f, std::min(14.0f * static_cast<float>(depth) * scale_,
        bounds.right - bounds.left - 100.0f * scale_));
    bounds.left += indent;
    bounds.right = bounds.left + kDisclosureDip * scale_;
    return bounds;
}

fluent::ScrollbarSpec FilePickerTree::Scrollbar() const {
    return {bounds_, offset_, Viewport(), content_height_ * scale_,
        hover_scrollbar_ || dragging_ ? 1.0f : 0.0f, 1.0f, true};
}

void FilePickerTree::Draw(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                          bool enabled, bool focused, bool high_contrast) const {
    auto* dc = compositor.Dc();
    dc->PushAxisAlignedClip(bounds_, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t row = FirstRowEndingAfter(bounds_.top); row < rows_.size(); ++row) {
        auto rect = RowBounds(row);
        const auto& node = *rows_[row];
        if (node.divider) {
            const float y = std::round(rect.top - kGroupGapDip * scale_ * 0.5f);
            painter.FillRoundedRect(D2D1::RectF(rect.left + 9.0f * scale_, y,
                rect.right - 5.0f * scale_, y + std::max(1.0f, scale_)), 0.0f, theme.stroke_divider);
        }
        if (rect.top >= bounds_.bottom) break;
        const auto disclosure = DisclosureBounds(node, row);
        rect.left = disclosure.right - 3.0f * scale_;
        fluent::SidebarItemSpec item{};
        item.bounds = rect;
        item.text = node.label;
        item.glyph = node.expanded && node.glyph == L"\xE8B7" ? std::wstring_view{L"\xE838"} : node.glyph;
        item.trailing_reserve = node.pinned ? 20.0f * scale_ : 0.0f;
        item.state.enabled = enabled;
        item.state.selected = &node == selected_;
        item.state.hovered = &node == hover_;
        item.state.pressed = &node == pressed_ && !pressed_disclosure_;
        item.state.keyboard_focus = focused && &node == cursor_;
        painter.DrawSidebarItem(item);
        if (item.state.keyboard_focus) painter.DrawFocusRing(rect, theme.radius_flyout * scale_);
        if (node.pinned) {
            const bool highlighted = high_contrast && enabled && (item.state.selected || item.state.hovered || item.state.pressed);
            const auto color = !enabled ? theme.text_disabled : highlighted ? theme.text_selected : theme.text_secondary;
            painter.DrawGlyph(L"\xE718", D2D1::RectF(rect.right - 22.0f * scale_, rect.top + 8.0f * scale_,
                rect.right - 8.0f * scale_, rect.bottom - 8.0f * scale_), color);
        }
        if (node.Expandable()) {
            const auto glyph = node.expanded ? L"\xE70D" : L"\xE76C";
            auto color = !enabled || node.listing ? theme.text_disabled : node.failed ? theme.danger : theme.text_secondary;
            painter.DrawGlyph(glyph, D2D1::RectF(disclosure.left + 4 * scale_, disclosure.top + 7 * scale_,
                disclosure.right - 4 * scale_, disclosure.bottom - 7 * scale_), color);
        }
    }
    painter.DrawScrollbar(Scrollbar());
    dc->PopAxisAlignedClip();
}

void FilePickerTree::Reveal(Node* node, bool animate) {
    const auto row = std::find(rows_.begin(), rows_.end(), node);
    if (row == rows_.end()) return;
    const auto now = AnimationTimeMs();
    Tick(now, animate);
    scroll_.Reset();
    const auto rect = RowBounds(static_cast<size_t>(row - rows_.begin()));
    const float delta = rect.top < bounds_.top ? rect.top - bounds_.top
        : std::max(0.0f, rect.bottom - bounds_.bottom);
    const float viewport = Viewport();
    // A distant target must land in view even when the scroll model caps travel.
    scroll_.ScrollBy(offset_, delta, Maximum(), viewport, now, animate && std::abs(delta) <= viewport);
}

void FilePickerTree::Focus(bool animate) {
    if (!cursor_ && !rows_.empty()) cursor_ = rows_.front();
    Reveal(cursor_, animate);
}

std::optional<FilePickerTree::Target> FilePickerTree::Key(WPARAM key, bool animate) {
    if (rows_.empty()) return {};
    if (!cursor_) cursor_ = rows_.front();
    const int current = static_cast<int>(std::find(rows_.begin(), rows_.end(), cursor_) - rows_.begin());
    const int page = std::max(1, static_cast<int>(Viewport() / (kRowDip * scale_)) - 1);
    int next = current;
    switch (key) {
    case VK_RETURN: case VK_SPACE: return cursor_->target;
    case VK_LEFT:
        if (cursor_->expanded) Toggle(*cursor_, animate);
        else if (cursor_->parent) cursor_ = cursor_->parent;
        break;
    case VK_RIGHT:
        if (!cursor_->expanded) Toggle(*cursor_, animate);
        else if (!cursor_->children.empty()) cursor_ = cursor_->children.front().get();
        break;
    case VK_UP: --next; break;
    case VK_DOWN: ++next; break;
    case VK_PRIOR: next -= page; break;
    case VK_NEXT: next += page; break;
    case VK_HOME: next = 0; break;
    case VK_END: next = static_cast<int>(rows_.size()) - 1; break;
    default: return {};
    }
    if (next != current) cursor_ = rows_[static_cast<size_t>(std::clamp(next, 0, static_cast<int>(rows_.size()) - 1))];
    Reveal(cursor_, animate);
    return {};
}

bool FilePickerTree::OnScrollbar(float x, float y) const {
    return Maximum() > 0 && x >= bounds_.right - kScrollbarDip * scale_ && Contains(bounds_, x, y);
}

FilePickerTree::Node* FilePickerTree::Hit(float x, float y, bool* disclosure) const {
    if (!Contains(bounds_, x, y) || OnScrollbar(x, y)) return nullptr;
    const auto row = FirstRowEndingAfter(y);
    if (row >= rows_.size() || !Contains(RowBounds(row), x, y)) return nullptr;
    auto* node = rows_[row];
    if (disclosure) *disclosure = node->Expandable() && Contains(DisclosureBounds(*node, row), x, y);
    return node;
}

void FilePickerTree::DragScrollbar(float y, bool high_contrast) {
    const auto thumb = fluent::ScrollbarThumbRect(Scrollbar(), scale_, high_contrast);
    const float travel = Viewport() - (thumb.bottom - thumb.top);
    if (travel > 0.0f) offset_ = Maximum() * std::clamp((y - bounds_.top - drag_offset_) / travel, 0.0f, 1.0f);
}

void FilePickerTree::MouseDown(float x, float y, bool high_contrast) {
    scroll_.Reset();
    pressed_ = Hit(x, y, &pressed_disclosure_);
    if (pressed_) cursor_ = pressed_;
    if (OnScrollbar(x, y)) {
        dragging_ = true;
        const auto thumb = fluent::ScrollbarThumbRect(Scrollbar(), scale_, high_contrast);
        drag_offset_ = y >= thumb.top && y < thumb.bottom ? y - thumb.top : (thumb.bottom - thumb.top) * 0.5f;
        DragScrollbar(y, high_contrast);
    }
}

void FilePickerTree::MouseMove(float x, float y, bool high_contrast) {
    hover_ = Hit(x, y);
    hover_scrollbar_ = OnScrollbar(x, y);
    if (dragging_) DragScrollbar(y, high_contrast);
}

std::optional<FilePickerTree::Target> FilePickerTree::MouseUp(float x, float y, bool animate) {
    auto* node = pressed_;
    bool disclosure = false;
    const bool match = node && node == Hit(x, y, &disclosure) && disclosure == pressed_disclosure_;
    CancelPointer();
    if (!match) return {};
    if (disclosure) { Toggle(*node, animate); return {}; }
    return node->target;
}

void FilePickerTree::MouseLeave() { hover_ = nullptr; hover_scrollbar_ = false; }

void FilePickerTree::CancelPointer() {
    pressed_ = nullptr;
    pressed_disclosure_ = false;
    dragging_ = false;
}

void FilePickerTree::Scroll(float wheel_steps, UINT lines, bool animate) {
    const float step = lines == WHEEL_PAGESCROLL ? Viewport() : kRowDip * scale_ * static_cast<float>(lines);
    if (step > 0.0f) scroll_.ScrollBy(offset_, wheel_steps * step, Maximum(), Viewport(), AnimationTimeMs(), animate);
}

bool FilePickerTree::Tick(uint64_t now, bool animate) { return scroll_.Tick(offset_, Maximum(), now, animate); }

} // namespace pulse::ui
