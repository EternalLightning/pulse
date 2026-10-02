#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string>

namespace pulse::app {
bool PromptSettingsValue(HWND owner, const std::wstring& title,
                         const std::wstring& hint, const std::wstring& initial,
                         std::wstring& result, size_t max_length = 32,
                         const std::wstring& confirm_label = {}, bool dark = false,
                         D2D1_COLOR_F accent = {0.0f, 0.4706f, 0.8314f, 1.0f});
}
