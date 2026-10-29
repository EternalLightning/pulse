#include "network_recent.h"

#include "network_locations.h"
#include "server_address.h"
#include "../common/json_utils.h"

#include <algorithm>
#include <cwchar>

namespace pulse::app {

bool NetworkRecentList::Touch(const std::wstring& path, uint64_t now) {
    const std::wstring root = NetworkRecentRoot(path);
    if (root.empty()) return false;
    auto it = std::find_if(items_.begin(), items_.end(),
                           [&](const NetworkRecentItem& item) { return SameNetworkLocation(item.path, root); });
    if (it != items_.end()) items_.erase(it);
    items_.insert(items_.begin(), NetworkRecentItem{root, now});
    if (items_.size() > kCap) items_.resize(kCap);
    return true;
}

bool NetworkRecentList::Remove(const std::wstring& path) {
    const size_t before = items_.size();
    std::erase_if(items_, [&](const NetworkRecentItem& item) { return SameNetworkLocation(item.path, path); });
    return items_.size() != before;
}

std::wstring NetworkRecentList::ToJson() const {
    std::wstring out = L"{\"version\":1,\"items\":[";
    for (size_t i = 0; i < items_.size(); ++i) {
        if (i) out += L',';
        out += L"{\"path\":\"";
        json::Escape(items_[i].path, out);
        out += L"\",\"t\":\"" + std::to_wstring(items_[i].last_visit) + L"\"}";
    }
    out += L"]}";
    return out;
}

bool NetworkRecentList::FromJson(const std::wstring& text) {
    if (text.size() > 1024 * 1024) return false;
    const json::Document document(text);
    const auto* array = document.Find(L"items");
    if (!document.Is(json::Kind::Object) || !array || array->kind != json::Kind::Array) return false;
    std::vector<NetworkRecentItem> loaded;
    for (const auto& value : array->children) {
        if (loaded.size() >= kCap) break;
        if (value.kind != json::Kind::Object) continue;
        const std::wstring root = NetworkRecentRoot(json::String(value.Find(L"path")));
        if (root.empty()) continue;
        const bool duplicate = std::any_of(loaded.begin(), loaded.end(),
            [&](const NetworkRecentItem& item) { return SameNetworkLocation(item.path, root); });
        if (duplicate) continue;
        const std::wstring stamp = json::String(value.Find(L"t"));
        uint64_t time = 0;
        if (!stamp.empty() && !json::ParseUnsigned(stamp, time)) continue;
        loaded.push_back({root, time});
    }
    items_ = std::move(loaded);
    return true;
}

} // namespace pulse::app
