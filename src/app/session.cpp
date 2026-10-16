// session.cpp
#include "session.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/user_storage.h"
#include <commctrl.h>
#include <prsht.h>
#include <shlobj.h>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pulse::app {

namespace {

// Session file version. Bump when a stored field changes meaning: the loader
// migrates (or discards) values written by older versions.
//   6 -> 7: BuiltinQuickAccess lost its reserved "starred" bit, so the stored
//           quick-access mask is dropped for files written before 7.
constexpr int kSessionVersion = 8;
constexpr size_t kMaxSessionWindows = 32;

std::wstring FormatScaled3(const std::array<float, 3>& edges) {
    return std::to_wstring(static_cast<int>(std::lround(edges[0] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[1] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[2] * 10000.0f)));
}

template<size_t Count>
std::array<float, Count> ParseScaledColumns(const std::wstring& value) {
    std::array<float, Count> output{};
    size_t pos = 0;
    for (size_t i = 0; i < Count; ++i) {
        const size_t comma = value.find(L',', pos);
        if ((comma == std::wstring::npos) != (i + 1 == Count)) return {};
        const auto token = std::wstring_view(value).substr(pos,
            comma == std::wstring::npos ? value.size() - pos : comma - pos);
        int scaled = 0;
        if (!pulse::json::ParseInteger(token, scaled) || scaled < 0 || scaled > 40000000) return {};
        output[i] = static_cast<float>(scaled) / 10000.0f;
        pos = comma == std::wstring::npos ? value.size() : comma + 1;
    }
    return output;
}

std::array<float, 3> ParseScaled3(const std::wstring& value) {
    return ParseScaledColumns<3>(value);
}

std::wstring FormatScaled4(const std::array<float, 4>& edges) {
    return std::to_wstring(static_cast<int>(std::lround(edges[0] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[1] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[2] * 10000.0f))) + L","
         + std::to_wstring(static_cast<int>(std::lround(edges[3] * 10000.0f)));
}

std::array<float, 4> ParseScaled4(const std::wstring& value) {
    return ParseScaledColumns<4>(value);
}

std::wstring FormatScaledList(const std::vector<float>& values) {
    std::wstring out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(static_cast<int>(std::lround(values[i] * 10000.0f)));
    }
    return out;
}

std::vector<float> ParseScaledList(const std::wstring& value) {
    std::vector<float> out;
    size_t pos = 0;
    while (pos < value.size()) {
        const size_t comma = value.find(L',', pos);
        const std::wstring token = value.substr(
            pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        int scaled = 0;
        pulse::json::ParseInteger(token, scaled);
        if (scaled > 0 && scaled < 10000)
            out.push_back(static_cast<float>(scaled) / 10000.0f);
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return out;
}

// Column view widths: whole DIP values, 0 = default width for that slot.
std::wstring FormatDipList(const std::vector<float>& values) {
    std::wstring out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += L",";
        out += std::to_wstring(std::max(0, static_cast<int>(std::lround(values[i]))));
    }
    return out;
}

std::vector<float> ParseDipList(const std::wstring& value) {
    std::vector<float> out;
    size_t pos = 0;
    while (pos < value.size() && out.size() < 64) {
        const size_t comma = value.find(L',', pos);
        const std::wstring token = value.substr(
            pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
        int dip = 0;
        pulse::json::ParseInteger(token, dip);
        out.push_back(dip > 0 && dip < 4096 ? static_cast<float>(dip) : 0.0f);
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    while (!out.empty() && out.back() <= 0.0f) out.pop_back();
    return out;
}

} // namespace

std::wstring LayoutTabsToJson(const std::vector<LayoutTabSnapshot>& tabs) {
    std::wstring out = L"[";
    for (size_t i = 0; i < tabs.size(); ++i) {
        if (i) out += L",";
        const LayoutTabSnapshot& tab = tabs[i];
        out += L"{\"pinned\":";
        out += tab.pinned ? L"true" : L"false";
        out += L",\"group\":" + std::to_wstring(tab.group);
        out += L",\"marker_rgb\":" + std::to_wstring(tab.marker_rgb);
        out += L",\"title\":\"";
        std::wstring escaped;
        pulse::json::Escape(tab.title, escaped);
        out += escaped;
        out += L"\",\"layout\":" + std::to_wstring(tab.layout);
        out += L",\"focused\":" + std::to_wstring(tab.focused);
        out += L",\"target\":" + std::to_wstring(tab.target);
        out += L",\"splitRatios\":\"";
        out += FormatScaledList(tab.split_ratios);
        out += L"\",\"panes\":[";
        for (size_t p = 0; p < tab.panes.size(); ++p) {
            if (p) out += L",";
            const PaneFolderSnapshot& pane = tab.panes[p];
            out += L"{\"path\":\"";
            escaped.clear();
            pulse::json::Escape(pane.path, escaped);
            out += escaped;
            out += L"\",\"view\":\"";
            out += ui::ViewModeName(pane.view);
            out += L"\",\"cols\":\"";
            out += FormatScaled3(pane.columns);
            out += L"\",\"searchCols\":\"";
            out += FormatScaled4(pane.search_columns);
            out += L"\",\"colLayout\":";
            out += pane.column_layout ? L"true" : L"false";
            out += L",\"colWidths\":\"";
            out += FormatDipList(pane.column_widths);
            out += L"\"}";
        }
        out += L"]}";
    }
    out += L"]";
    return out;
}

std::wstring TabGroupsToJson(const std::vector<GroupSessionSnapshot>& groups) {
    std::wstring out = L"[";
    for (size_t g = 0; g < groups.size(); ++g) {
        if (g) out += L",";
        const GroupSessionSnapshot& grp = groups[g];
        out += L"{\"id\":" + std::to_wstring(grp.id);
        out += L",\"name\":\"";
        std::wstring escaped;
        pulse::json::Escape(grp.name, escaped);
        out += escaped;
        out += L"\",\"color\":" + std::to_wstring(grp.color_rgb);
        out += L",\"collapsed\":";
        out += grp.collapsed ? L"true" : L"false";
        out += L"}";
    }
    out += L"]";
    return out;
}

// Extract the contents of each top-level {...} object inside an array body
// (without the outer brackets). Handles nested objects/arrays and string
// escapes; returns false on unbalanced input.
static bool SplitTopLevelObjects(const std::wstring& body, std::vector<std::wstring>& out) {
    const std::wstring array = L"[" + body + L"]";
    const pulse::json::Document doc(array);
    if (!doc.Is(pulse::json::Kind::Array)) return false;
    for (const auto& item : doc.Root().children)
        if (item.kind != pulse::json::Kind::Object) return false;
    out = pulse::json::Objects(doc, &doc.Root());
    return true;
}

static bool ExtractJsonArray(const pulse::json::Document& doc, const wchar_t* key,
                             std::wstring& body) {
    const auto raw = pulse::json::ExtractRaw(doc, key, pulse::json::Kind::Array);
    if (raw.empty()) return false;
    body = raw.substr(1, raw.size() - 2);
    return true;
}

bool ParseTabGroups(const std::wstring& array_json,
                    std::vector<GroupSessionSnapshot>& out) {
    out.clear();
    if (array_json.size() < 2 || array_json.front() != L'[' ||
        array_json.back() != L']') {
        return false;
    }
    std::vector<std::wstring> objs;
    if (!SplitTopLevelObjects(array_json.substr(1, array_json.size() - 2), objs))
        return false;
    for (const std::wstring& gj : objs) {
        const pulse::json::Document group_doc(gj);
        GroupSessionSnapshot grp;
        grp.id = pulse::json::ExtractInt(group_doc, L"id");
        grp.name = pulse::json::ExtractString(group_doc, L"name");
        grp.color_rgb = static_cast<uint32_t>(std::max(
            0, pulse::json::ExtractInt(group_doc, L"color")));
        grp.collapsed = pulse::json::ExtractBool(group_doc, L"collapsed");
        out.push_back(std::move(grp));
    }
    return true;
}

bool ParseLayoutTabs(const std::wstring& array_json,
                     std::vector<LayoutTabSnapshot>& out) {
    out.clear();
    if (array_json.size() < 2 || array_json.front() != L'[' ||
        array_json.back() != L']') {
        return false;
    }
    std::vector<std::wstring> tabObjs;
    if (!SplitTopLevelObjects(array_json.substr(1, array_json.size() - 2), tabObjs))
        return false;
    for (const std::wstring& tabJson : tabObjs) {
        const pulse::json::Document tab_doc(tabJson);
        LayoutTabSnapshot tab;
        tab.pinned = pulse::json::ExtractBool(tab_doc, L"pinned");
        tab.group = pulse::json::ExtractInt(tab_doc, L"group");
        tab.title = pulse::json::ExtractString(tab_doc, L"title");
        tab.marker_rgb = static_cast<uint32_t>(std::clamp(
            pulse::json::ExtractInt(tab_doc, L"marker_rgb"), 0, 0xFFFFFF));
        tab.layout = pulse::json::ExtractInt(tab_doc, L"layout");
        tab.focused = pulse::json::ExtractInt(tab_doc, L"focused");
        tab.target = pulse::json::ExtractInt(tab_doc, L"target");
        if (tabJson.find(L"\"target\"") == std::wstring::npos) tab.target = -1;
        tab.split_ratios = ParseScaledList(
            pulse::json::ExtractString(tab_doc, L"splitRatios"));

        std::wstring panesBody;
        if (ExtractJsonArray(tab_doc, L"panes", panesBody)) {
            std::vector<std::wstring> paneObjs;
            if (!SplitTopLevelObjects(panesBody, paneObjs)) return false;
            for (const std::wstring& pj : paneObjs) {
                const pulse::json::Document pane_doc(pj);
                PaneFolderSnapshot pane;
                pane.path = pulse::json::ExtractString(pane_doc, L"path");
                pane.view = ui::ParseViewMode(pulse::json::ExtractString(pane_doc, L"view"));
                pane.columns = ParseScaled3(pulse::json::ExtractString(pane_doc, L"cols"));
                pane.search_columns = ParseScaled4(
                    pulse::json::ExtractString(pane_doc, L"searchCols"));
                pane.column_layout = pulse::json::ExtractBool(pane_doc, L"colLayout");
                pane.column_widths = ParseDipList(pulse::json::ExtractString(pane_doc, L"colWidths"));
                tab.panes.push_back(std::move(pane));
            }
        }
        out.push_back(std::move(tab));
    }
    return true;
}

std::wstring GetPulseDataDir() {
    const auto root = storage::ConfigurationRoot();
    if (!root.empty()) CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

static std::wstring WindowSessionToJson(const WindowSessionSnapshot& snap) {
    std::wstring trayJson;
    snap.tray.ToJson(trayJson);

    std::wostringstream f;
    f << L"{\n";
    f << L"  \"version\":" << kSessionVersion << L",\n";
    f << L"  \"left\":" << snap.window_rect.left << L",\n";
    f << L"  \"top\":" << snap.window_rect.top << L",\n";
    f << L"  \"right\":" << snap.window_rect.right << L",\n";
    f << L"  \"bottom\":" << snap.window_rect.bottom << L",\n";
    f << L"  \"maximized\":" << (snap.maximized ? L"true" : L"false") << L",\n";
    f << L"  \"dark\":" << (snap.dark ? L"true" : L"false") << L",\n";
    f << L"  \"path\":\"";
    std::wstring escaped;
    pulse::json::Escape(snap.active_path, escaped);
    f << escaped << L"\",\n";
    f << L"  \"sidebarCollapsed\":" << snap.sidebar_collapsed << L",\n";
    f << L"  \"sidebarHidden\":" << snap.sidebar_hidden << L",\n";
    f << L"  \"sidebarOrder\":\"";
    for (size_t i = 0; i < snap.sidebar_order.size(); ++i) {
        if (i != 0) f << L",";
        f << snap.sidebar_order[i];
    }
    f << L"\",\n";
    f << L"  \"quickAccessHidden\":" << snap.quick_access_hidden << L",\n";
    f << L"  \"starredExpanded\":" << (snap.starred_expanded ? L"true" : L"false") << L",\n";
    f << L"  \"detailsPanel\":" << (snap.details_panel ? 1 : 0) << L",\n";
    f << L"  \"detailsPanelWidth\":" << std::clamp(snap.details_panel_width,
        static_cast<int>(ui::kDetailsMinWidthDip),
        static_cast<int>(ui::kPanelWidthMaxDip))
      << L",\n";
    f << L"  \"detailsPreviewOnly\":" << (snap.details_preview_only ? L"true" : L"false") << L",\n";
    f << L"  \"detailsPreview\":" << (snap.details_preview ? L"true" : L"false") << L",\n";
    f << L"  \"tray\":" << trayJson << L",\n";
    f << L"  \"undo\":" << (snap.undo_json.empty() ? L"[]" : snap.undo_json) << L",\n";
    f << L"  \"activeTab\":" << snap.active_layout_tab << L",\n";
    f << L"  \"tabGroups\":" << TabGroupsToJson(snap.tab_groups) << L",\n";
    f << L"  \"layoutTabs\":" << LayoutTabsToJson(snap.layout_tabs) << L"\n";
    f << L"}\n";
    return f.str();
}

static bool ParseWindowSessionJson(const std::wstring& json, WindowSessionSnapshot& snap) {
    const pulse::json::Document doc(json);
    if (!doc.Is(pulse::json::Kind::Object)) return false;
    snap = {};

    snap.window_rect.left = pulse::json::ExtractInt(doc, L"left");
    snap.window_rect.top = pulse::json::ExtractInt(doc, L"top");
    snap.window_rect.right = pulse::json::ExtractInt(doc, L"right");
    snap.window_rect.bottom = pulse::json::ExtractInt(doc, L"bottom");
    snap.maximized = pulse::json::ExtractBool(doc, L"maximized");
    snap.dark = pulse::json::ExtractBool(doc, L"dark");
    snap.active_path = pulse::json::ExtractString(doc, L"path");
    snap.sidebar_collapsed = pulse::json::ExtractInt(doc, L"sidebarCollapsed");
    snap.sidebar_hidden = pulse::json::ExtractInt(doc, L"sidebarHidden");
    // Section order travels as "0,1,2,6,3,4,5"; anything malformed falls back to
    // the default order (NormalizeSidebarOrder rebuilds a full permutation).
    snap.sidebar_order.clear();
    {
        const std::wstring order_text = pulse::json::ExtractString(doc, L"sidebarOrder");
        size_t start = 0;
        while (start < order_text.size()) {
            size_t end = order_text.find(L',', start);
            if (end == std::wstring::npos) end = order_text.size();
            uint64_t value = 0;
            if (pulse::json::ParseUnsigned(std::wstring_view(order_text).substr(start, end - start), value) &&
                value < kSidebarSectionCount) snap.sidebar_order.push_back(static_cast<int>(value));
            start = end + 1;
        }
        // Sessions written before the starred section existed lack its id; put it
        // where the default order has it instead of appending it at the very end.
        const int starred_id = static_cast<int>(SidebarSectionId::Starred);
        if (!snap.sidebar_order.empty() &&
            std::find(snap.sidebar_order.begin(), snap.sidebar_order.end(), starred_id) ==
                snap.sidebar_order.end()) {
            const auto cloud = std::find(snap.sidebar_order.begin(), snap.sidebar_order.end(),
                                         static_cast<int>(SidebarSectionId::Cloud));
            snap.sidebar_order.insert(
                cloud == snap.sidebar_order.end() ? snap.sidebar_order.begin() : cloud + 1,
                starred_id);
        }
        snap.sidebar_order = NormalizeSidebarOrder(snap.sidebar_order);
    }
    // Bit positions changed in version 7 (the reserved starred bit is gone), so
    // anything older restarts with every built-in link visible.
    snap.quick_access_hidden = pulse::json::ExtractInt(doc, L"version") >= 7
        ? pulse::json::ExtractInt(doc, L"quickAccessHidden") : 0;
    snap.starred_expanded = !doc.Find(L"starredExpanded")
        ? true : pulse::json::ExtractBool(doc, L"starredExpanded");
    snap.details_panel = pulse::json::ExtractInt(doc, L"detailsPanel") != 0;
    snap.details_preview_only = pulse::json::ExtractBool(doc, L"detailsPreviewOnly", false);
    // Missing in sessions written before the switch existed: keep the old
    // always-on behaviour.
    snap.details_preview = !doc.Find(L"detailsPreview")
        ? true : pulse::json::ExtractBool(doc, L"detailsPreview", true);
    snap.details_panel_width = pulse::json::ExtractInt(doc, L"detailsPanelWidth");
    // The stored value is the user's intent; the window caps it while drawing.
    if (snap.details_panel_width < static_cast<int>(ui::kDetailsMinWidthDip) ||
        snap.details_panel_width > static_cast<int>(ui::kPanelWidthMaxDip))
        snap.details_panel_width = 340;
    const std::array<int, 3> columnEdges{
        pulse::json::ExtractInt(doc, L"detailsColumn0"),
        pulse::json::ExtractInt(doc, L"detailsColumn1"),
        pulse::json::ExtractInt(doc, L"detailsColumn2") };
    if (columnEdges[0] > 0 && columnEdges[0] < columnEdges[1] &&
        columnEdges[1] < columnEdges[2] && columnEdges[2] < 10000) {
        for (size_t i = 0; i < columnEdges.size(); ++i)
            snap.details_column_dividers[i] =
                static_cast<float>(columnEdges[i]) / 10000.0f;
    }

    const auto tray_json = pulse::json::ExtractRaw(doc, L"tray", pulse::json::Kind::Array);
    if (!tray_json.empty()) snap.tray.FromJson(tray_json);
    snap.undo_json = pulse::json::ExtractRaw(doc, L"undo", pulse::json::Kind::Array);

    snap.active_layout_tab = pulse::json::ExtractInt(doc, L"activeTab");
    snap.tab_groups.clear();
    std::wstring groupsBody;
    if (ExtractJsonArray(doc, L"tabGroups", groupsBody)) {
        std::vector<GroupSessionSnapshot> parsed;
        if (ParseTabGroups(L"[" + groupsBody + L"]", parsed))
            snap.tab_groups = std::move(parsed);
    }
    snap.layout_tabs.clear();
    std::wstring layoutBody;
    if (ExtractJsonArray(doc, L"layoutTabs", layoutBody)) {
        std::vector<LayoutTabSnapshot> parsed;
        if (ParseLayoutTabs(L"[" + layoutBody + L"]", parsed))
            snap.layout_tabs = std::move(parsed);
    }
    return true;
}

std::wstring SessionToJson(const SessionSnapshot& snap) {
    const WindowSessionSnapshot& primary = snap.windows.empty()
        ? static_cast<const WindowSessionSnapshot&>(snap) : snap.windows.front();
    auto out = WindowSessionToJson(primary);
    const auto closing = out.find_last_of(L'}');
    out.resize(closing);
    out += L",\n  \"windows\":[";
    if (snap.windows.empty()) {
        out += WindowSessionToJson(primary);
    } else {
        const size_t count = (std::min)(snap.windows.size(), kMaxSessionWindows);
        for (size_t i = 0; i < count; ++i) {
            if (i) out += L",";
            out += WindowSessionToJson(snap.windows[i]);
        }
    }
    out += L"]\n}\n";
    return out;
}

bool ParseSessionJson(const std::wstring& json, SessionSnapshot& snap) {
    SessionSnapshot parsed;
    if (!ParseWindowSessionJson(json, parsed)) return false;
    const pulse::json::Document doc(json);
    const auto* windows = doc.Find(L"windows");
    if (windows && windows->kind == pulse::json::Kind::Array && !windows->children.empty() &&
        std::all_of(windows->children.begin(), windows->children.end(), [](const auto& item) {
            return item.kind == pulse::json::Kind::Object;
        })) {
        for (const auto& item : pulse::json::Objects(doc, windows)) {
            if (parsed.windows.size() == kMaxSessionWindows) break;
            WindowSessionSnapshot window;
            if (!ParseWindowSessionJson(item, window)) return false;
            parsed.windows.push_back(std::move(window));
        }
    }
    if (parsed.windows.empty()) parsed.windows.push_back(parsed);
    static_cast<WindowSessionSnapshot&>(parsed) = parsed.windows.front();
    snap = std::move(parsed);
    return true;
}

bool SaveSession(const SessionSnapshot& snap) {
    const auto dir = GetPulseDataDir();
    return !dir.empty() && WriteUtf8FileAtomic(dir + L"\\session.json", SessionToJson(snap));
}

bool LoadSession(SessionSnapshot& snap) {
    const auto dir = GetPulseDataDir();
    std::wstring json;
    return !dir.empty() && ReadUtf8File(dir + L"\\session.json", json) && ParseSessionJson(json, snap);
}

} // namespace pulse::app
