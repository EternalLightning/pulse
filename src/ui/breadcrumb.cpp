// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "breadcrumb_layout.h"
#include "device_crumbs.h"
#include "dialog_text_fit.h"
#include "typography.h"
#include "../common/localization.h"
#include "../common/known_folder_labels.h"
#include "../common/path_utils.h"
#include "../model/places.h"

namespace pulse::ui {
namespace {
DeviceCrumbResolver device_resolver = nullptr;
constexpr float kBreadcrumbPaddingDip = 8.0f;
constexpr float kBreadcrumbChevronDip = 14.0f;
}
void SetDeviceCrumbResolver(DeviceCrumbResolver resolver) { device_resolver = resolver; }
std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path,
    std::wstring_view virtual_title) {
    std::vector<BreadcrumbSegment> out;
    std::wstring p = pulse::path::StripExtendedPathPrefix(path);
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    if (p.empty()) {
        // Empty path = This PC: a single segment that navigates to "".
        BreadcrumbSegment seg;
        seg.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        seg.path = L"";
        out.push_back(seg);
        return out;
    }
    if (p.starts_with(L"pulse:device:")) {
        const auto rest = p.substr(13);
        const auto slash = rest.find(L'\\');
        const auto key = rest.substr(0, slash);
        const DeviceCrumbInfo info = device_resolver ? device_resolver(key) : DeviceCrumbInfo{};
        out.push_back({pulse::l10n::Get(pulse::l10n::StringId::ThisPc), L""});
        std::wstring prefix = L"pulse:device:" + key;
        const auto name = info.name.empty() ? pulse::l10n::Get(pulse::l10n::StringId::DevicePhone) : info.name;
        out.push_back({name, prefix});
        if (slash == std::wstring::npos) return out;
        size_t start = slash + 1;
        bool storage = true;
        while (start < rest.size()) {
            const auto next = rest.find(L'\\', start);
            const auto part = rest.substr(start, next == std::wstring::npos ? next : next - start);
            prefix += L"\\" + part;
            if (!(storage && info.skips_storage)) out.push_back({part, prefix});
            else out.back().path = prefix;
            storage = false;
            if (next == std::wstring::npos) break;
            start = next + 1;
        }
        return out;
    }
    std::wstring pulse_kind, pulse_rest;
    if (model::ParsePulsePath(p, &pulse_kind, &pulse_rest)) {
        if (pulse_kind == L"recycle") {
            BreadcrumbSegment pc;
            pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
            pc.path = L"";
            out.push_back(pc);
            BreadcrumbSegment bin;
            bin.text = virtual_title.empty() ? pulse::l10n::Get(pulse::l10n::StringId::RecycleBin)
                : std::wstring(virtual_title);
            bin.path = L"pulse:recycle";
            out.push_back(bin);
            return out;
        }
        BreadcrumbSegment seg;
        seg.path = path;
        if (pulse_kind == L"home")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Home);
        else if (pulse_kind == L"search" || pulse_kind == L"saved-search")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Search);
        else if (pulse_kind == L"starred")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::StarredItems);
        else if (pulse_kind == L"recent")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Recent);
        else if (pulse_kind == L"shell" || pulse_kind == L"library") {
            if (!virtual_title.empty()) seg.text = virtual_title;
            else if (pulse_kind == L"library" || pulse_rest == L"libraries")
                seg.text = l10n::Get(l10n::StringId::ShellLibraries);
            else if (pulse_rest == L"controlpanel") seg.text = l10n::Get(l10n::StringId::ShellControlPanel);
            else seg.text = l10n::Get(l10n::StringId::ShellApplications);
        }
        else if (pulse_kind == L"settings")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Settings);
        else if (pulse_kind == L"archive") {
            const auto separator = pulse_rest.find(L'|');
            const auto file = pulse_rest.substr(0, separator);
            const auto filename = file.find_last_of(L"/\\");
            seg.text = filename == std::wstring::npos ? file : file.substr(filename + 1);
            if (separator != std::wstring::npos && separator + 1 < pulse_rest.size())
                seg.text += L" / " + pulse_rest.substr(separator + 1);
        } else if (pulse_kind == L"tag")
            seg.text = pulse_rest.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::Tag) : pulse_rest;
        else
            seg.text = pulse_kind;
        if (!virtual_title.empty()) seg.text = virtual_title;
        out.push_back(seg);
        return out;
    }

    std::wstring prefix; // full path of the segments emitted so far
    size_t i = 0;
    if (p.size() >= 2 && p[1] == L':') {
        // Explorer-style: drives live under This PC.
        BreadcrumbSegment pc;
        pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        pc.path = L"";
        out.push_back(pc);
        // Drive root, e.g. "C:\": single segment with the drive icon text.
        prefix = p.substr(0, 2);
        BreadcrumbSegment seg;
        seg.text = prefix;
        seg.path = prefix + L"\\";
        out.push_back(seg);
        i = 2;
        while (i < p.size() && p[i] == L'\\') ++i;
        prefix += L"\\";
    } else if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        // UNC: split the root into a server segment and a share segment.
        auto s3 = p.find(L'\\', 2);           // after server
        std::wstring server = (s3 == std::wstring::npos) ? p.substr(2)
                                                         : p.substr(2, s3 - 2);
        BreadcrumbSegment srv;
        srv.text = server;
        srv.path = L"\\\\" + server;
        out.push_back(srv);
        prefix = srv.path;
        if (s3 == std::wstring::npos) {
            i = p.size();
        } else {
            auto s4 = p.find(L'\\', s3 + 1);  // after share
            std::wstring share = (s4 == std::wstring::npos) ? p.substr(s3 + 1)
                                                            : p.substr(s3 + 1, s4 - s3 - 1);
            if (!share.empty()) {
                BreadcrumbSegment seg;
                seg.text = share;
                seg.path = prefix + L"\\" + share;
                out.push_back(seg);
                prefix = seg.path;
            }
            i = (s4 == std::wstring::npos) ? p.size() : s4 + 1;
        }
    }
    while (i <= p.size() && i < p.size()) {
        auto sep = p.find(L'\\', i);
        std::wstring part = (sep == std::wstring::npos) ? p.substr(i)
                                                        : p.substr(i, sep - i);
        if (!part.empty()) {
            if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
            prefix += part;
            BreadcrumbSegment seg;
            seg.text = pulse::path::KnownFolderDisplayName(prefix, part);
            seg.path = prefix;
            out.push_back(seg);
        }
        if (sep == std::wstring::npos) break;
        i = sep + 1;
    }
    return out;
}

void LayoutBreadcrumbs(Compositor* compositor, const D2D1_RECT_F& bounds, float scale, float margin,
    const std::wstring& path, std::wstring_view title, std::vector<MainRenderer::BreadcrumbPlaced>& out) {
    out.clear();
    const auto segments = SplitBreadcrumb(path, title);
    if (segments.empty()) return;
    const float padding = kBreadcrumbPaddingDip * scale;
    const float chevron = kBreadcrumbChevronDip * scale;
    const float available = (std::max)(0.0f, bounds.right - bounds.left - 2 * margin);
    const auto measure = [&](const std::wstring& text) {
        return typography::MeasureLine(compositor, compositor ? compositor->AddressFormat() : nullptr, text);
    };
    std::vector<float> widths;
    widths.reserve(segments.size());
    float total = chevron * static_cast<float>(segments.size() - 1);
    for (const auto& segment : segments) {
        widths.push_back(measure(segment.text) + 2 * padding);
        total += widths.back();
    }
    size_t first = 0;
    while (total > available && first + 1 < segments.size()) total -= widths[first++] + chevron;
    float x = bounds.left + margin;
    for (size_t i = first; i < segments.size(); ++i) {
        if (i > first) x += chevron;
        const auto& segment = segments[i];
        const float width = (std::min)(widths[i], bounds.right - margin - x);
        if (width <= 0.0f) break;
        const float text_width = width - 2 * padding;
        out.push_back({D2D1::RectF(x, bounds.top + 2 * scale, x + width, bounds.bottom - 2 * scale),
            text_width <= 0.0f ? std::wstring{} : widths[i] <= width + 0.5f ? segment.text :
                FitTextEnd(segment.text, text_width, measure), segment.path, segment.text});
        x += widths[i];
    }
}

int HitBreadcrumbSegment(std::span<const MainRenderer::BreadcrumbPlaced> segments, float x, float y) {
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& rc = segments[i].rc;
        if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom) return static_cast<int>(i);
    }
    return -1;
}

void DrawBreadcrumbSegmentHighlight(fluent::Painter& painter, const Theme& theme,
    const MainRenderer::BreadcrumbPlaced& segment, float opacity, bool pressed) {
    if (opacity <= 0.0f) return;
    auto fill = pressed ? theme.fill_pressed : theme.fill_hover;
    fill.a *= opacity;
    painter.FillRoundedRect(segment.rc, theme.radius_control * painter.Scale(), fill);
}

void DrawBreadcrumbSegmentLabel(fluent::Painter& painter, Compositor& compositor, const Theme& theme,
    const MainRenderer::BreadcrumbPlaced& segment, bool separator, D2D1_COLOR_F ink) {
    const auto& rc = segment.rc;
    if (separator) {
        const float left = rc.left - kBreadcrumbChevronDip * painter.Scale();
        painter.DrawGlyph(L"\xE76C", D2D1::RectF(left, rc.top, rc.left, rc.bottom), theme.text_secondary);
    }
    if (segment.text.empty()) return;
    const float padding = kBreadcrumbPaddingDip * painter.Scale();
    painter.DrawText(segment.text, D2D1::RectF(rc.left + padding, rc.top, rc.right - padding, rc.bottom),
        compositor.AddressFormat(), ink);
}

}
