#pragma once

#include "../ipc/protocol.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

namespace pulse::shell {

struct CtxHandlerDesc {
    CLSID clsid{};
    std::wstring clsid_text;
    std::wstring name;
};

struct CtxItemOut {
    uint32_t id = 0;
    bool enabled = true;
    bool separator_after = false;
    bool has_children = false;
    bool child = false;
    std::wstring verb;
    std::wstring text;
    std::wstring clsid;
    std::wstring handler;
};

struct CtxHandlerSlot {
    IContextMenu* menu = nullptr;
    IContextMenu2* menu2 = nullptr;
    IContextMenu3* menu3 = nullptr;
    HMENU hmenu = nullptr;
    UINT id_first = 0;
    UINT id_last = 0;
    std::wstring clsid;
    std::wstring name;
    bool preserve_builtin = false;
};

struct CtxBind {
    IDataObject* data = nullptr;
    PIDLIST_ABSOLUTE folder = nullptr;
    HKEY assoc = nullptr;
    ~CtxBind();
    CtxBind() = default;
    CtxBind(const CtxBind&) = delete;
    CtxBind& operator=(const CtxBind&) = delete;
};

struct CtxParsedSelection {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    ~CtxParsedSelection();
    CtxParsedSelection() = default;
    CtxParsedSelection(const CtxParsedSelection&) = delete;
    CtxParsedSelection& operator=(const CtxParsedSelection&) = delete;
};

struct CtxWorkerHandles {
    HANDLE done_event = nullptr;
    HANDLE exit_event = nullptr;
    HANDLE thread = nullptr;

    CtxWorkerHandles() = default;
    ~CtxWorkerHandles() { Stop(); }
    CtxWorkerHandles(const CtxWorkerHandles&) = delete;
    CtxWorkerHandles& operator=(const CtxWorkerHandles&) = delete;
    bool Start(LPTHREAD_START_ROUTINE entry, void* context,
               decltype(&CreateEventW) create_event = CreateEventW,
               decltype(&CreateThread) create_thread = CreateThread);
    void Stop() noexcept;
};

void ReleaseHandlerSlot(CtxHandlerSlot& slot) noexcept;

std::vector<CtxHandlerDesc> EnumerateCtxHandlers(
    bool background, const std::wstring& path,
    const std::vector<std::wstring>& disabled_clsids);

bool ParseCtxSelection(const std::vector<std::wstring>& paths, bool background,
                       CtxParsedSelection& out);

bool BindCtxSelection(const std::vector<std::wstring>& paths, bool background,
                      CtxBind& out, const CtxParsedSelection* parsed = nullptr);

HRESULT QueryOneHandler(const CtxHandlerDesc& handler, const CtxBind& bind,
                        HWND owner, UINT id_first, UINT id_last, UINT qcm_flags,
                        CtxHandlerSlot& slot);

void CollectHandlerItems(const CtxHandlerSlot& slot, bool background,
                         std::vector<CtxItemOut>& out);

uint32_t FindItemId(const std::vector<CtxItemOut>& items, uint32_t id,
                    const std::wstring& verb, const std::wstring& text,
                    uint32_t id_first = 1, uint32_t id_last = 0x7FFF);

} // namespace pulse::shell
