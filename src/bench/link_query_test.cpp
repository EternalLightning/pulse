#include "../app/link_resolve.h"
#include "../ipc/shell_query_client.h"
#include <iostream>

namespace {
pulse::ipc::ShellQueryResult reply;
bool transport_ok = true;
bool cancel_after_reply = false;
int query_count = 0;
int failures = 0;
std::wstring queried_path;

void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}

pulse::fs::DirEntry Original() {
    pulse::fs::DirEntry entry;
    entry.name = L"example.lnk";
    entry.size = 17;
    entry.mtime = {19, 23};
    entry.attrs = FILE_ATTRIBUTE_ARCHIVE;
    entry.link_target = L"previous";
    entry.link_target_size = 31;
    entry.link_target_mtime = {37, 41};
    entry.link_target_is_dir = true;
    return entry;
}

bool Unchanged(const pulse::fs::DirEntry& entry) {
    return entry.name == L"example.lnk" && entry.size == 17 &&
        entry.mtime.dwLowDateTime == 19 && entry.mtime.dwHighDateTime == 23 &&
        entry.attrs == FILE_ATTRIBUTE_ARCHIVE && entry.link_target == L"previous" &&
        entry.link_target_size == 31 && entry.link_target_mtime.dwLowDateTime == 37 &&
        entry.link_target_mtime.dwHighDateTime == 41 && entry.link_target_is_dir;
}
}

namespace pulse::ipc {
bool QueryShell(const ShellQueryRequest& request, ShellQueryResult& result,
                const std::function<bool()>& cancelled, DWORD) {
    if (cancelled && cancelled()) return false;
    ++query_count;
    queried_path = request.path;
    Check(request.kind == ShellQueryKind::Shortcut && !request.use_attributes && !request.attributes,
          "shortcut query requests no Shell tracking or attributes override");
    result = reply;
    cancel_after_reply = true;
    return transport_ok;
}
}

int main() {
    using namespace pulse;
    reply.error = S_OK;
    reply.text = L"\\\\server\\share\\target.txt";
    reply.target_size = 99;
    reply.target_mtime = {101, 103};
    auto entry = Original();
    Check(app::ResolveLink(L"C:\\fixture\\file.lnk", entry) &&
          entry.link_target == fs::NormalizePath(reply.text) && entry.link_target_size == 99 &&
          entry.link_target_mtime.dwLowDateTime == 101 && !entry.link_target_is_dir &&
          entry.name == L"example.lnk" && entry.size == 17 && entry.attrs == FILE_ATTRIBUTE_ARCHIVE,
          "successful query fills target metadata while preserving shortcut metadata");

    entry = Original();
    transport_ok = false;
    Check(!app::ResolveLink(L"unreadable.lnk", entry) && Unchanged(entry), "transport failure leaves entry untouched");
    transport_ok = true;
    reply.error = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    Check(!app::ResolveLink(L"missing.lnk", entry) && Unchanged(entry), "missing target leaves entry untouched");
    reply.error = S_OK;
    reply.text.clear();
    Check(!app::ResolveLink(L"empty.lnk", entry) && Unchanged(entry), "empty target leaves entry untouched");
    reply.text = L"C:\\target";
    cancel_after_reply = false;
    Check(!app::ResolveLink(L"cancelled.lnk", entry, [] { return cancel_after_reply; }) && Unchanged(entry),
          "cancellation after response leaves entry untouched");

    std::vector<fs::DirEntry> entries(3);
    for (auto& item : entries) item.name = L"item.lnk";
    query_count = 0;
    cancel_after_reply = false;
    app::ResolveLinksInPlace(L"C:\\fixture", entries, [] { return query_count == 1; });
    Check(query_count == 1 && entries[0].link_target.empty() && entries[1].link_target.empty(),
          "enumeration cancellation is checked for each shortcut and before applying a response");

    entries.resize(1);
    entries[0].full_path = L"\\\\?\\C:\\" + std::wstring(280, L'x') + L"\\item.lnk";
    query_count = 0;
    app::ResolveLinksInPlace(L"pulse:recent", entries, {});
    Check(query_count == 1 && queried_path == entries[0].full_path,
          "virtual entries forward their explicit long path unchanged");
    return failures ? 1 : 0;
}
