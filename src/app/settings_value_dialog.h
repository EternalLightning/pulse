#pragma once
#include <windows.h>
#include <string>

namespace pulse::app {
bool PromptSettingsValue(HWND owner, const std::wstring& title,
                         const std::wstring& hint, const std::wstring& initial,
                         std::wstring& result, size_t max_length = 32,
                         const std::wstring& confirm_label = {});
}
