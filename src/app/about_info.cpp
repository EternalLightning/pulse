#include "about_info.h"

#include "../common/localization.h"
#include "pulse_version.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace pulse::app {
namespace {

std::wstring BuildTimeText() {
    // PULSE_BUILD_ID is "YYYYMMDDTHHMMSSZ-<hash>" in UTC; show local time.
    const std::wstring id = PULSE_BUILD_ID;
    SYSTEMTIME utc{};
    if (id.size() < 16 || id[8] != L'T' || id[15] != L'Z' ||
        swscanf_s(id.c_str(), L"%4hu%2hu%2huT%2hu%2hu%2hu", &utc.wYear, &utc.wMonth, &utc.wDay,
                  &utc.wHour, &utc.wMinute, &utc.wSecond) != 6)
        return {};
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t text[32]{};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute);
    return text;
}

const std::wstring& WindowsText() {
    static const std::wstring text = [] {
        wchar_t product[128]{}, display[64]{}, build[32]{};
        DWORD ubr = 0;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                          KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            DWORD size = sizeof(product);
            RegGetValueW(key, nullptr, L"ProductName", RRF_RT_REG_SZ, nullptr, product, &size);
            size = sizeof(display);
            if (RegGetValueW(key, nullptr, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &size) != ERROR_SUCCESS) {
                size = sizeof(display);
                RegGetValueW(key, nullptr, L"ReleaseId", RRF_RT_REG_SZ, nullptr, display, &size);
            }
            size = sizeof(build);
            RegGetValueW(key, nullptr, L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, build, &size);
            size = sizeof(ubr);
            RegGetValueW(key, nullptr, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
            RegCloseKey(key);
        }
        std::wstring name = *product ? product : L"Windows";
        // ProductName still says "Windows 10" on Windows 11.
        if (wcstoul(build, nullptr, 10) >= 22000) {
            const size_t at = name.find(L"Windows 10");
            if (at != std::wstring::npos) name.replace(at, 10, L"Windows 11");
        }
        if (*display) name += L" " + std::wstring(display);
        if (*build) {
            name += L" (" + std::wstring(build);
            if (ubr) name += L"." + std::to_wstring(ubr);
            name += L")";
        }
        return name;
    }();
    return text;
}

std::wstring InstallDirectory() {
    wchar_t path[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return {};
    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? dir : dir.substr(0, slash);
}

constexpr const wchar_t* kArchitecture =
#if defined(_M_ARM64)
    L"ARM64";
#elif defined(_M_X64)
    L"x64";
#else
    L"x86";
#endif

} // namespace

std::vector<AboutRow> BuildAboutRows(bool index_service, bool index_installed, float scale) {
    using I = l10n::StringId;
    std::vector<AboutRow> rows;
    rows.emplace_back(l10n::Get(I::AboutVersion),
                      std::wstring(PULSE_VERSION_STRING) + L" \u00B7 " + kArchitecture);
    const std::wstring built = BuildTimeText();
    if (!built.empty()) rows.emplace_back(l10n::Get(I::AboutBuilt), built);
    rows.emplace_back(l10n::Get(I::AboutBuildId), PULSE_BUILD_ID);
    rows.emplace_back(l10n::Get(I::AboutOs), WindowsText());
    rows.emplace_back(l10n::Get(I::AboutLocation), InstallDirectory());
    rows.emplace_back(l10n::Get(I::AboutIndex),
                      l10n::Get(index_service ? I::AboutIndexService
                                : index_installed ? I::AboutIndexWaiting : I::AboutIndexUser));
    wchar_t display[128]{};
    swprintf_s(display, l10n::Get(I::AboutDisplayFormat).c_str(),
               static_cast<int>(std::lround(scale * 100.0f)));
    rows.emplace_back(l10n::Get(I::AboutDisplay), display);
    return rows;
}

std::wstring AboutRowsText(const std::vector<AboutRow>& rows) {
    std::wstring text = L"Pulse\r\n";
    for (const auto& [label, value] : rows) text += label + L": " + value + L"\r\n";
    return text;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(memory)) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(memory);
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            if (!ok) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}

} // namespace pulse::app
