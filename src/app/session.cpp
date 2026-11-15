// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// session.cpp
#include "session.h"
#include "../ui/panel_metrics.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../common/user_storage.h"
#include <sstream>
#include <algorithm>

namespace pulse::app {

namespace {

// Session file version. Bump when a stored field changes meaning: the loader
// migrates (or discards) values written by older versions.
//   6 -> 7: BuiltinQuickAccess lost its reserved "starred" bit, so the stored
//           quick-access mask is dropped for files written before 7.
constexpr int kSessionVersion = 8;

} // namespace

std::wstring GetPulseDataDir() {
    const auto root = storage::ConfigurationRoot();
    if (!root.empty()) CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

std::wstring SessionPreferencesToJson(const WindowSessionSnapshot& snap) {
    std::wostringstream f;
    f << L"{\n";
    f << L"  \"version\":" << kSessionVersion << L",\n";
    f << L"  \"left\":" << snap.window_rect.left << L",\n";
    f << L"  \"top\":" << snap.window_rect.top << L",\n";
    f << L"  \"right\":" << snap.window_rect.right << L",\n";
    f << L"  \"bottom\":" << snap.window_rect.bottom << L",\n";
    f << L"  \"maximized\":" << (snap.maximized ? L"true" : L"false") << L",\n";
    f << L"  \"dark\":" << (snap.dark ? L"true" : L"false") << L",\n";
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
    f << L"  \"detailsPreview\":" << (snap.details_preview ? L"true" : L"false");
    f << L"\n";
    f << L"}\n";
    return f.str();
}

static bool ParseWindowPreferences(const pulse::json::Value& doc, WindowSessionSnapshot& snap) {
    if (doc.kind != pulse::json::Kind::Object) return false;
    snap = {};

    snap.window_rect.left = pulse::json::Int(doc.Find(L"left"));
    snap.window_rect.top = pulse::json::Int(doc.Find(L"top"));
    snap.window_rect.right = pulse::json::Int(doc.Find(L"right"));
    snap.window_rect.bottom = pulse::json::Int(doc.Find(L"bottom"));
    snap.maximized = pulse::json::Bool(doc.Find(L"maximized"));
    snap.dark = pulse::json::Bool(doc.Find(L"dark"));
    const auto sidebar_mask = (1u << kSidebarSectionCount) - 1u;
    snap.sidebar_collapsed = static_cast<int>(static_cast<uint32_t>(std::max(0,
        pulse::json::Int(doc.Find(L"sidebarCollapsed")))) & sidebar_mask);
    snap.sidebar_hidden = static_cast<int>(static_cast<uint32_t>(std::max(0,
        pulse::json::Int(doc.Find(L"sidebarHidden")))) & sidebar_mask);
    // Section order travels as "0,1,2,6,3,4,5"; anything malformed falls back to
    // the default order (NormalizeSidebarOrder rebuilds a full permutation).
    snap.sidebar_order.clear();
    {
        const std::wstring order_text = pulse::json::String(doc.Find(L"sidebarOrder"));
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
    snap.quick_access_hidden = pulse::json::Int(doc.Find(L"version")) >= 7
        ? pulse::json::Int(doc.Find(L"quickAccessHidden")) : 0;
    snap.quick_access_hidden = static_cast<int>(static_cast<uint32_t>(std::max(0,
        snap.quick_access_hidden)) & ((1u << static_cast<int>(BuiltinQuickAccess::Count)) - 1u));
    snap.starred_expanded = !doc.Find(L"starredExpanded")
        ? true : pulse::json::Bool(doc.Find(L"starredExpanded"));
    snap.details_panel = pulse::json::Int(doc.Find(L"detailsPanel")) != 0;
    snap.details_preview_only = pulse::json::Bool(doc.Find(L"detailsPreviewOnly"), false);
    // Missing in sessions written before the switch existed: keep the old
    // always-on behaviour.
    snap.details_preview = !doc.Find(L"detailsPreview")
        ? true : pulse::json::Bool(doc.Find(L"detailsPreview"), true);
    snap.details_panel_width = pulse::json::Int(doc.Find(L"detailsPanelWidth"));
    // The stored value is the user's intent; the window caps it while drawing.
    if (snap.details_panel_width < static_cast<int>(ui::kDetailsMinWidthDip) ||
        snap.details_panel_width > static_cast<int>(ui::kPanelWidthMaxDip))
        snap.details_panel_width = 340;
    const std::array<int, 3> columnEdges{
        pulse::json::Int(doc.Find(L"detailsColumn0")),
        pulse::json::Int(doc.Find(L"detailsColumn1")),
        pulse::json::Int(doc.Find(L"detailsColumn2")) };
    if (columnEdges[0] > 0 && columnEdges[0] < columnEdges[1] &&
        columnEdges[1] < columnEdges[2] && columnEdges[2] < 10000) {
        for (size_t i = 0; i < columnEdges.size(); ++i)
            snap.details_column_dividers[i] =
                static_cast<float>(columnEdges[i]) / 10000.0f;
    }

    return true;
}

static bool ParseWindowPreferencesJson(const std::wstring& json, WindowSessionSnapshot& snap) {
    const pulse::json::Document doc(json);
    return doc.Is(pulse::json::Kind::Object) && ParseWindowPreferences(doc.Root(), snap);
}

static bool SaveSessionJson(const std::wstring& path, const std::wstring& json) {
    const pulse::json::Document doc(json);
    if (!doc.Is(pulse::json::Kind::Object)) return false;
    std::vector<uint8_t> bytes;
    // Match both the JSON character budget and ReadUtf8File's byte budget.
    if (!EncodeUtf8Bytes(json, bytes) || bytes.size() > 16ull * 1024 * 1024) return false;
    return WriteUtf8FileAtomic(path, json);
}

bool SaveSessionPreferences(const WindowSessionSnapshot& snap) {
    const auto dir = GetPulseDataDir();
    return !dir.empty() && SaveSessionJson(dir + L"\\preferences.json", SessionPreferencesToJson(snap));
}

bool LoadSessionPreferences(WindowSessionSnapshot& snap, bool* saves_enabled) {
    if (saves_enabled) *saves_enabled = false;
    const auto dir = GetPulseDataDir();
    if (dir.empty()) return false;
    std::wstring json;
    const std::wstring preferences = dir + L"\\preferences.json";
    if (GetFileAttributesW(preferences.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const bool loaded = ReadUtf8File(preferences, json) && ParseWindowPreferencesJson(json, snap);
        if (saves_enabled) *saves_enabled = loaded;
        return loaded;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    const auto legacy = dir + L"\\session.json";
    const DWORD attrs = GetFileAttributesW(legacy.c_str());
    const DWORD legacy_error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attrs == INVALID_FILE_ATTRIBUTES &&
        (legacy_error == ERROR_FILE_NOT_FOUND || legacy_error == ERROR_PATH_NOT_FOUND);
    const bool loaded = ReadUtf8File(legacy, json) && ParseWindowPreferencesJson(json, snap);
    if (saves_enabled) *saves_enabled = loaded || missing;
    return loaded;
}

} // namespace pulse::app
