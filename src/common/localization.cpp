// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "localization.h"

#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace pulse::l10n {
namespace {

constexpr UINT kFirstString = IDS_SETTINGS;
// Must stay on the highest allocated string id, otherwise Get() returns empty.
constexpr UINT kLastString = IDS_LOCALIZATION_LAST;
static_assert(static_cast<UINT>(StringId::SettingsChangeTracking) >= kFirstString &&
              static_cast<UINT>(StringId::ChangeDisabled) <= kLastString &&
              static_cast<UINT>(StringId::FolderSortMixed) <= kLastString &&
              static_cast<UINT>(StringId::WallpaperBlurStrong) <= kLastString &&
              static_cast<UINT>(StringId::CreateItemNamePrompt) <= kLastString &&
              static_cast<UINT>(StringId::CreateItemAction) <= kLastString &&
              static_cast<UINT>(StringId::DeleteReasonNetwork) <= kLastString &&
              static_cast<UINT>(StringId::OpRemainingSize) <= kLastString &&
              static_cast<UINT>(StringId::MatchWindowSizeHint) <= kLastString &&
              static_cast<UINT>(StringId::ArchivePasswordOrDataError) <= kLastString &&
              static_cast<UINT>(StringId::SettingsDefaultManagerPartial) <= kLastString &&
              static_cast<UINT>(StringId::ArchivePasswordRetryHint) <= kLastString &&
              static_cast<UINT>(StringId::OperationCenterRecycle) <= kLastString &&
              static_cast<UINT>(StringId::SettingsExplorerTakeoverDesc) <= kLastString &&
              static_cast<UINT>(StringId::IndexRemovableExcluded) <= kLastString &&
              static_cast<UINT>(StringId::IndexDeviceUnavailable) <= kLastString &&
              static_cast<UINT>(StringId::TextRenderSmooth) <= kLastString &&
              static_cast<UINT>(StringId::OpenNetworkSettings) <= kLastString &&
              static_cast<UINT>(StringId::DupSkipSmallFiles) <= kLastString &&
              static_cast<UINT>(StringId::ClearFilter) <= kLastString &&
              static_cast<UINT>(StringId::ShellApplications) <= kLastString);

HINSTANCE g_module = nullptr;
std::atomic<Language> g_preference{Language::System};
std::atomic<Language> g_effective{Language::EnUS};
std::mutex g_mutex;
// Published strings stay immutable when the UI language changes. Workers may
// still hold references to the previous language while finishing an operation.
// Resource IDs are sparse. Allocate only strings requested by this process.
std::array<std::unordered_map<UINT, std::wstring>, 2> g_cache;
const std::wstring g_empty;

#include "runtime_messages.generated.h"

Language SystemLanguage() noexcept {
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_CHINESE ? Language::ZhCN : Language::EnUS;
}

LANGID ResourceLanguage(Language language) noexcept {
    return language == Language::ZhCN
        ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
        : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

std::wstring LoadStringResource(UINT id, LANGID language) {
    if (!g_module) return {};
    const UINT block = id / 16 + 1;
    const UINT index = id % 16;
    HRSRC resource = FindResourceExW(g_module, RT_STRING, MAKEINTRESOURCEW(block), language);
    if (!resource) return {};
    HGLOBAL data = LoadResource(g_module, resource);
    const auto* cursor = static_cast<const wchar_t*>(LockResource(data));
    if (!cursor) return {};
    for (UINT i = 0; i < index; ++i) cursor += 1 + static_cast<UINT>(*cursor);
    const UINT length = static_cast<UINT>(*cursor++);
    return std::wstring(cursor, length);
}

void ApplyLanguage() {
    const Language selected = g_preference.load();
    g_effective = selected == Language::System ? SystemLanguage() : selected;
    SetThreadUILanguage(ResourceLanguage(g_effective));
}

} // namespace

bool IsLanguageId(std::wstring_view id) noexcept {
    return id == L"system" || id == L"zh-CN" || id == L"en-US";
}

Language LanguageFromId(std::wstring_view id) noexcept {
    if (id == L"zh-CN") return Language::ZhCN;
    if (id == L"en-US") return Language::EnUS;
    return Language::System;
}

const wchar_t* LanguageId(Language language) noexcept {
    switch (language) {
    case Language::ZhCN: return L"zh-CN";
    case Language::EnUS: return L"en-US";
    default: return L"system";
    }
}

void Initialize(HINSTANCE module, std::wstring_view preference) {
    std::lock_guard lock(g_mutex);
    g_module = module;
    g_preference = LanguageFromId(preference);
    ApplyLanguage();
}

void SetLanguage(std::wstring_view preference) {
    const Language next = LanguageFromId(preference);
    if (next == g_preference) return;
    g_preference = next;
    ApplyLanguage();
}

Language preference() noexcept { return g_preference; }
Language effective_language() noexcept { return g_effective; }

const wchar_t* LocaleName() noexcept {
    return g_effective == Language::ZhCN ? L"zh-CN" : L"en-US";
}

const std::wstring& Get(StringId id) {
    return Get(id, g_effective.load());
}
const std::wstring& Get(StringId id, Language language) {
    const UINT value = static_cast<UINT>(id);
    if (value < kFirstString || value > kLastString) return g_empty;
    std::lock_guard lock(g_mutex);
    if (language == Language::System) language = SystemLanguage();
    if (!g_module) return RuntimeMessage(value, language);
    const size_t locale = language == Language::ZhCN ? 0 : 1;
    auto& cache = g_cache[locale];
    if (const auto found = cache.find(value); found != cache.end()) return found->second;
    auto text = LoadStringResource(value, ResourceLanguage(language));
    if (text.empty() && language != Language::EnUS)
        text = LoadStringResource(value, ResourceLanguage(Language::EnUS));
    if (text.empty()) text = RuntimeMessage(value, language);
    return cache.emplace(value, std::move(text)).first->second;
}

} // namespace pulse::l10n
