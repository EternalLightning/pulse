#pragma once
#include <string_view>

namespace pulse::shell {
bool IsSameDeletionItem(std::wstring_view requested, std::wstring_view resolved);
}
