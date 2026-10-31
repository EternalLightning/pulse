// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "localization.h"

#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>

namespace pulse::l10n {
namespace {

constexpr UINT kFirstString = IDS_SETTINGS;
// Must stay on the highest allocated string id, otherwise Get() returns empty.
constexpr UINT kLastString = IDS_UNGROUP_FOLDER;
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
              static_cast<UINT>(StringId::OpenNetworkSettings) <= kLastString);

HINSTANCE g_module = nullptr;
std::atomic<Language> g_preference{Language::System};
std::atomic<Language> g_effective{Language::EnUS};
std::mutex g_mutex;
// Published strings stay immutable when the UI language changes. Workers may
// still hold references to the previous language while finishing an operation.
std::array<std::array<std::wstring, kLastString - kFirstString + 1>, 2> g_cache;
std::array<std::array<bool, kLastString - kFirstString + 1>, 2> g_loaded{};
const std::wstring g_empty;

const std::wstring& RuntimeMessage(UINT id, Language language) {
    switch (id) {
#define PULSE_LOCALIZED_MESSAGE(message_id, zh, en) \
    case message_id: { \
        static const std::wstring chinese = L##zh; \
        static const std::wstring english = L##en; \
        return language == Language::ZhCN ? chinese : english; \
    }
#include "runtime_messages.inc"
#undef PULSE_LOCALIZED_MESSAGE
    default: return g_empty;
    }
}

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
    const UINT value = static_cast<UINT>(id);
    if (value < kFirstString || value > kLastString) return g_empty;
    const size_t index = value - kFirstString;
    std::lock_guard lock(g_mutex);
    const Language language = g_effective.load();
    if (!g_module) return RuntimeMessage(value, language);
    const size_t locale = language == Language::ZhCN ? 0 : 1;
    if (!g_loaded[locale][index]) {
        g_cache[locale][index] = LoadStringResource(value, ResourceLanguage(language));
        if (g_cache[locale][index].empty() && language != Language::EnUS) {
            g_cache[locale][index] = LoadStringResource(value, ResourceLanguage(Language::EnUS));
        }
        if (g_cache[locale][index].empty())
            g_cache[locale][index] = RuntimeMessage(value, language);
        g_loaded[locale][index] = true;
    }
    return g_cache[locale][index];
}

} // namespace pulse::l10n
