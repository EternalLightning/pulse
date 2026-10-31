// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <string>
#include <functional>

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
// Returning false aborts after a verified copy; fixtures must use an isolated root.
void SetConfigurationCopyHookForTesting(std::function<bool(const std::wstring&)> hook);
// Tests use file locators unless an isolated HKCU key is supplied; never use the real user key.
void OverrideDefaultRootForTesting(const std::wstring& root, const std::wstring& locator_registry_key = {});
}
