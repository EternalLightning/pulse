// app_hosted_edit.h — Address/filter/rename overlay editors.
#pragma once
#include "app_runtime.h"
#include <commctrl.h>

namespace pulse {
std::wstring FormatAddressPath(const std::wstring& text);
void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
                            int left_margin_dip, int right_margin_dip);
HWND CreateHostedEdit(AppState& s, SUBCLASSPROC proc);
void LayoutAddressEditor(AppState& s);
void EnsureEditVisuals(AppState& s);
void ShowAddressEditor(AppState& s);
void HideAddressEditor(AppState& s, bool navigate);
void ShowAddressSearch(AppState& s);
bool IsAddressSearchResults(const app::Tab* tab);
void SaveAddressSearchDraft(AppState& s);
void ExitAddressSearch(AppState& s);
void FillAddressSearchView(AppState& s, ui::WindowViewModel& vm);
void ShowAddressSearchScope(AppState& s);
void ShowSearchOptions(AppState& s, bool manage = false);
void SwitchAddressSearchMode(AppState& s, bool content);
void StartIndexedContentSearch(AppState& s, app::Tab& tab, const std::wstring& rest, bool scan = false);
std::wstring ContentIndexStatusText(AppState& s);
void ShowAddressSearchHistory(AppState& s);
void RecordSearchHistory(AppState& s, const std::wstring& path);
void SubmitAddressSearch(AppState& s, bool live = false);
void QueueAddressSearch(AppState& s);
void FlushAddressSearch(AppState& s);
// Combined business tick retained for isolated callers without the UI scheduler.
bool TickAddressSearch(AppState& s, ULONGLONG now);
bool TickAddressSearchMaintenance(AppState& s, ULONGLONG now);
bool TickAddressSearchDeadlines(AppState& s, ULONGLONG now);
bool TickAddressSearchAnimation(AppState& s, uint64_t now);
void LayoutFilterEditor(AppState& s);
void ShowFilterEditor(AppState& s, bool select_mode = false);
void ShowWildcardSelect(AppState& s);
void HideFilterEditor(AppState& s, bool commit);
void ClearPaneFilter(AppState& s);
void SyncFilterEditor(AppState& s);
void LayoutRenameOverlay(AppState& s);
void ShowRenameOverlay(AppState& s);
void HideRenameOverlay(AppState& s, bool commit);
bool TagRenameCell(AppState& s, const app::TagId& tag_id, D2D1_RECT_F& cell);
void LayoutTagRenameOverlay(AppState& s);
void ShowTagRenameOverlay(AppState& s, const app::TagId& tag_id);
void HideTagRenameOverlay(AppState& s, bool commit);
bool HandleHostedEditShortcut(AppState& s, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
} // namespace pulse
