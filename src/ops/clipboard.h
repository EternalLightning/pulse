// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// clipboard.h — System clipboard interop with Explorer (CF_HDROP + drop effect).
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct IDataObject;
struct IShellItem;
struct IShellItemArray;
struct IFileOperation;

namespace pulse::ops {

// An ambiguous COPY|MOVE preference is a copy, never authorization to cut.
bool PreferredEffectIsCut(uint32_t preferred_effect);

struct ClipboardData {
    std::vector<std::wstring> paths;
    bool cut = false; // CFSTR_PREFERREDDROPEFFECT == DROPEFFECT_MOVE
    uint32_t sequence = 0;
};

// Writes paths as CF_HDROP + CFSTR_PREFERREDDROPEFFECT so Explorer can paste them.
// Callers on a worker supply the owner HWND explicitly.
bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut, HWND owner = nullptr);

// Writes plain text (Ctrl+Shift+C "copy path").
bool WriteClipboardText(const std::wstring& text, HWND owner = nullptr);

// Reads an Explorer copy/cut (CF_HDROP). Returns false if no files present.
bool ReadClipboard(ClipboardData& out);
// Captures the selected clipboard under its lock. S_FALSE means there are no HDROP paths.
HRESULT ReadClipboardForPaste(uint32_t expected_sequence, ClipboardData& out);

// Shell virtual files (for example files copied from RDP, archives or Outlook)
// have no filesystem paths. Shell items use IFileOperation through completion;
// descriptor-only data uses the destination's drop target. Call from an STA
// worker; the shell may perform provider I/O here.
inline bool ClipboardHasShellFiles() {
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Shell IDList Array")) ||
           IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
}

bool PasteShellClipboard(const std::wstring& dir, uint32_t expected_sequence);
HRESULT PasteShellClipboard(const std::wstring& dir, uint32_t expected_sequence,
                            const std::atomic_bool& cancelled, HWND owner,
                            const std::function<bool()>& begin_transfer);
// The broker STA is already OLE-initialized and has resolved the device PIDL.
HRESULT PasteShellClipboardToFolder(IShellItem* folder, HWND owner, std::vector<std::wstring>& completed_names,
    uint32_t expected_sequence);
HRESULT PerformShellOperation(IFileOperation* operation, IShellItem* destination = nullptr,
                              std::vector<std::wstring>* completed_names = nullptr,
                              const std::atomic_bool* cancelled = nullptr);

bool HasShellFileData(IDataObject* data);
// STA worker only. Returns after the transfer completes; cancellation/failure
// never authorizes a move. The requested effect must be COPY or MOVE.
HRESULT PasteShellData(IDataObject* data, const std::wstring& dir, DWORD effect,
                      const std::atomic_bool* cancelled = nullptr);

#ifdef PULSE_CLIPBOARD_TESTING
HRESULT ResolveShellClipboardItemsForTest(IDataObject* data, IShellItemArray** out);
bool PasteShellDataForTest(IDataObject* data, const std::wstring& dir, uint32_t preferred_effect);
#endif

// Clears a completed cut only if the clipboard still contains the exact cut
// captured by ReadClipboard. This never clears a newer clipboard value.
bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths, HWND owner = nullptr);
bool CutPasteMovedAllPaths(const std::vector<std::wstring>& expected_normalized,
                          const std::vector<std::wstring>& moved_normalized);

} // namespace pulse::ops
