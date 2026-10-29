// link_resolve.cpp — See link_resolve.h.
#include "link_resolve.h"
#include "../fs/cloud_state.h"
#include "../ipc/shell_query_client.h"
#include <cwctype>

namespace pulse::app {
namespace {

bool HasLnkSuffix(const std::wstring& name) {
    if (name.size() < 4) return false;
    const std::wstring tail = name.substr(name.size() - 4);
    return _wcsicmp(tail.c_str(), L".lnk") == 0;
}

} // namespace

bool ResolveLink(const std::wstring& lnk_path, fs::DirEntry& e,
                 const std::function<bool()>& cancel) {
    if (fs::IsCloudPlaceholder(e.attrs)) return false;
    ipc::ShellQueryRequest request;
    request.kind = ipc::ShellQueryKind::Shortcut;
    request.path = lnk_path;
    ipc::ShellQueryResult result;
    if (!ipc::QueryShell(request, result, cancel) || FAILED(result.error) ||
        result.text.empty() || (cancel && cancel())) return false;

    const std::wstring target = fs::NormalizePath(result.text);
    if (target.empty()) return false;
    e.link_target = target;
    e.link_target_is_dir = result.target_is_dir;
    e.link_target_size = result.target_size;
    e.link_target_mtime = result.target_mtime;
    return true;
}

void ResolveLinksInPlace(const std::wstring& parent_path,
                         std::vector<fs::DirEntry>& entries,
                         const std::function<bool()>& cancel) {
    bool any = false;
    for (const auto& e : entries) {
        if (!e.is_dir && HasLnkSuffix(e.name)) { any = true; break; }
    }
    if (!any) return;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (cancel && cancel()) return;
        fs::DirEntry& e = entries[i];
        if (e.is_dir || !HasLnkSuffix(e.name)) continue;
        std::wstring full = e.full_path;
        if (full.empty()) {
            if (parent_path.empty() || fs::IsVirtualPath(parent_path)) continue;
            full = parent_path;
            if (full.back() != L'\\') full += L'\\';
            full += e.name;
        }
        ResolveLink(full, e, cancel);
    }
}

} // namespace pulse::app
