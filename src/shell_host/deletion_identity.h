#pragma once
#include <windows.h>
#include <string_view>
#include <memory>
#include "../common/file_identity.h"

namespace pulse::shell {
bool IsSameDeletionItem(std::wstring_view requested, std::wstring_view resolved,
    decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx);

class DeletionReference {
public:
    bool Capture(const std::wstring& path, const FileIdentity& expected,
        decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx);
    bool Matches(const std::wstring& path,
        decltype(&GetFileInformationByHandleEx) query = GetFileInformationByHandleEx) const;
private:
    std::shared_ptr<void> handle_;
};
}
