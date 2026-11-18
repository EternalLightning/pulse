// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <string>
namespace pulse {
struct AppState;
inline constexpr UINT WM_PATH_ALIASES_CHANGED = WM_APP + 0x2E0;
void InitializePathAliases(AppState& state);
void ShowPathAliases(AppState& state);
void SetPathAlias(AppState& state, const std::wstring& path);
bool ResolvePathAlias(AppState& state, const std::wstring& input, std::wstring& resolved);
void HandlePathAliasesChanged(AppState& state);
}
