// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <windows.h>
#include <imm.h>
#include <string>

namespace pulse::ui {
namespace edit_ime {
inline constexpr wchar_t kTransaction[] = L"Pulse.EditImeTransaction";
struct Transaction {
    std::wstring original, removed;
    DWORD from = 0, to = 0;
    bool committed = false;
};
inline std::wstring Text(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}
inline void Discard(HWND edit) {
    delete reinterpret_cast<Transaction*>(RemovePropW(edit, kTransaction));
}
inline void BeforeDefault(HWND edit, UINT message, LPARAM flags) {
    if (message == WM_NCDESTROY || message == WM_SETTEXT) Discard(edit);
    if (message == WM_IME_STARTCOMPOSITION) {
        Discard(edit);
        DWORD from = 0, to = 0;
        SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        if (from == to) return;
        auto* transaction = new Transaction;
        transaction->original = Text(edit);
        transaction->from = from;
        transaction->to = to;
        transaction->removed = transaction->original;
        transaction->removed.erase(from, to - from);
        if (!SetPropW(edit, kTransaction, transaction)) { delete transaction; return; }
        // Pinyin's next word must start at the committed caret, not jump to
        // the beginning of the old selection (#116, #162, #211).
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    }
    auto* transaction = reinterpret_cast<Transaction*>(GetPropW(edit, kTransaction));
    if (transaction && ((message == WM_IME_COMPOSITION && (flags & GCS_RESULTSTR)) ||
                        message == WM_IME_CHAR)) transaction->committed = true;
}
inline void AfterDefault(HWND edit, UINT message) {
    if (message != WM_IME_ENDCOMPOSITION && message != WM_KILLFOCUS) return;
    auto* transaction = reinterpret_cast<Transaction*>(RemovePropW(edit, kTransaction));
    if (!transaction) return;
    if (!transaction->committed && Text(edit) == transaction->removed) {
        // Undo only our preparatory deletion. If an IME changed the native
        // undo record, restore the snapshot explicitly instead of losing text.
        SendMessageW(edit, EM_UNDO, 0, 0);
        if (Text(edit) != transaction->original) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, EM_REPLACESEL, TRUE,
                reinterpret_cast<LPARAM>(transaction->original.c_str()));
        }
        SendMessageW(edit, EM_SETSEL, transaction->from, transaction->to);
    }
    delete transaction;
}
} // namespace edit_ime
} // namespace pulse::ui
