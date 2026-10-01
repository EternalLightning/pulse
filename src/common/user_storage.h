#pragma once
#include <string>

namespace pulse::storage {
enum class Kind { Configuration, Index };
std::wstring DefaultRoot();
std::wstring ConfigurationRoot();
std::wstring UserIndexRoot();
std::wstring Pending(Kind kind);
std::wstring LastError(Kind kind);
void Refresh();
bool Schedule(Kind kind, const std::wstring& target, std::wstring& error);
bool ApplyConfiguration(std::wstring& error);
bool ApplyUserIndex(std::wstring& error);
bool Commit(Kind kind, const std::wstring& target, std::wstring& error);
bool ClearPending(Kind kind, std::wstring& error);
void RecordError(Kind kind, const std::wstring& error);
// Tests must set an isolated directory before calling any storage API.
void OverrideDefaultRootForTesting(const std::wstring& root);
}
