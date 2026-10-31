// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// clipboard.h — System clipboard interop with Explorer (CF_HDROP + drop effect).
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct IDataObject;

namespace pulse::ops {

// An ambiguous COPY|MOVE preference is a copy, never authorization to cut.
bool PreferredEffectIsCut(uint32_t preferred_effect);

struct ClipboardData {
    std::vector<std::wstring> paths;
    bool cut = false; // CFSTR_PREFERREDDROPEFFECT == DROPEFFECT_MOVE
    uint32_t sequence = 0;
};

// Writes paths as CF_HDROP + CFSTR_PREFERREDDROPEFFECT so Explorer can paste them.
// Must be called on the UI thread (OpenClipboard affinity).
bool WriteClipboard(const std::vector<std::wstring>& paths, bool cut);

// Writes plain text (Ctrl+Shift+C "copy path"). UI thread only.
bool WriteClipboardText(const std::wstring& text);

// Reads an Explorer copy/cut (CF_HDROP). Returns false if no files present.
bool ReadClipboard(ClipboardData& out);

// Shell virtual files (for example files copied from RDP, archives or Outlook)
// have no filesystem paths. Paste them through the destination's shell drop
// target. Call from an STA worker; the shell may perform provider I/O here.
inline bool ClipboardHasShellFiles() {
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Shell IDList Array")) ||
           IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
}

inline bool ClipboardHasPasteableFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) || ClipboardHasShellFiles();
}

bool PasteShellClipboard(const std::wstring& dir, uint32_t expected_sequence);

#ifdef PULSE_CLIPBOARD_TESTING
bool PasteShellDataForTest(IDataObject* data, const std::wstring& dir, uint32_t preferred_effect);
#endif

// Clears a completed cut only if the clipboard still contains the exact cut
// captured by ReadClipboard. This never clears a newer clipboard value.
bool CompleteCutClipboard(uint32_t sequence, const std::vector<std::wstring>& expected_paths);

} // namespace pulse::ops
