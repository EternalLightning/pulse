#pragma once
#include <windows.h>
#include <string>

namespace pulse::app {
bool PromptNewItemName(HWND owner, const std::wstring& title, bool dark,
                       COLORREF accent, std::wstring& result, size_t max_length = 255);
}
