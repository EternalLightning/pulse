// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

bool RunContextMenuRegistryTests() {
    using namespace pulse::shell;
    namespace fs = std::filesystem;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return Report("initialize Shell COM", false);
    struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    struct Fixture {
        fs::path directory;
        std::wstring registry;
        HKEY classes = nullptr;
        bool overridden = false;
        bool created_directory = false;
        ~Fixture() {
            if (overridden) RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
            if (classes) {
                RegCloseKey(classes);
                RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
            }
            if (created_directory) {
                std::error_code error;
                fs::remove_all(directory, error);
            }
        }
    } fixture{fs::absolute(L"bench_data/context-menu-registry") / suffix,
              L"Software\\Pulse\\Tests\\ContextMenuRegistry-" + suffix};
    std::error_code error;
    fixture.created_directory = fs::create_directories(fixture.directory, error);
    if (!fixture.created_directory ||
        RegCreateKeyExW(HKEY_CURRENT_USER, fixture.registry.c_str(), 0, nullptr,
            REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &fixture.classes, nullptr) != ERROR_SUCCESS)
        return Report("create isolated Shell registry fixture", false);

    const auto set = [&](std::wstring_view path, const wchar_t* name, const std::wstring& value) {
        HKEY key = nullptr;
        if (RegCreateKeyExW(fixture.classes, std::wstring(path).c_str(), 0, nullptr,
                REG_OPTION_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        const auto result = RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    };
    wchar_t executable[32768]{};
    const DWORD executable_length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!executable_length || executable_length == ARRAYSIZE(executable))
        return Report("locate command fixture executable", false);
    const auto command = L"\"" + std::wstring(executable) + L"\" --context-menu-registry-invoke \"%1\"";
    const auto add_verb = [&](const std::wstring& key, const wchar_t* label, const wchar_t* model) {
        return set(key, nullptr, label) && set(key, L"MultiSelectModel", model) &&
            set(key + L"\\command", nullptr, command);
    };
    const wchar_t* object_clsid = L"{774d7000-219a-4aa3-b130-925000000001}";
    const wchar_t* background_clsid = L"{774d7000-219a-4aa3-b130-925000000002}";
    if (!add_verb(L"*\\shell\\pulse-registry-player", L"Pulse fixture Player", L"Player") ||
        !add_verb(L"*\\shell\\pulse-registry-single", L"Pulse fixture Single", L"Single") ||
        !add_verb(L"AllFilesystemObjects\\shell\\pulse-registry-document", L"Pulse fixture Document", L"Document") ||
        !set(L"Directory\\shellex\\ContextMenuHandlers\\Pulse object fixture", nullptr, object_clsid) ||
        !set(L"Directory\\Background\\shellex\\ContextMenuHandlers\\Pulse background fixture", nullptr, background_clsid))
        return Report("register fixture verbs and handlers in the private key", false);

    const auto first = fixture.directory / L"first.txt";
    const auto second = fixture.directory / L"second.bin";
    if (!std::ofstream(first).good() || !std::ofstream(second).good())
        return Report("create two files with different extensions", false);
    CtxBind single, mixed;
    if (!BindCtxSelection({first.wstring()}, false, single) ||
        !BindCtxSelection({first.wstring(), second.wstring()}, false, mixed))
        return Report("bind real Shell selections", false);
    fixture.overridden = RegOverridePredefKey(HKEY_CLASSES_ROOT, fixture.classes) == ERROR_SUCCESS;
    if (!fixture.overridden) return Report("override HKCR in the test process only", false);

    const auto object = EnumerateCtxHandlers(false, first.wstring(), {});
    const auto background = EnumerateCtxHandlers(true, fixture.directory.wstring(), {});
    bool passed = Report("object scene keeps its handler and adds common static verbs",
        object.size() == 2 && object.front().clsid_text == object_clsid && object.back().generic_static);
    passed &= Report("background scene queries only the background handler",
        background.size() == 1 && background.front().clsid_text == background_clsid);
    passed &= Report("disabled background handlers stay excluded from enumeration",
        EnumerateCtxHandlers(true, fixture.directory.wstring(), {background_clsid}).empty());
    const auto without_object = EnumerateCtxHandlers(false, first.wstring(), {object_clsid});
    passed &= Report("a disabled object handler cannot return through the native fallback",
        without_object.size() == 1 && without_object.front().generic_static);
    if (!passed) return false;

    const auto query = [&](const CtxBind& bind, bool multi) {
        CtxHandlerSlot slot;
        const auto hr = QueryOneHandler(object.back(), bind, nullptr, 1, 256, CMF_NORMAL, slot);
        std::vector<CtxItemOut> items;
        CollectHandlerItems(slot, false, items);
        const auto find = [&](std::wstring_view label) {
            return std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.text == label; });
        };
        const auto player = find(L"Pulse fixture Player");
        bool ok = Report(multi ? "mixed extensions retain * and AllFilesystemObjects commands" :
                "single selection keeps the same generic static commands",
            SUCCEEDED(hr) && player != items.end() && find(L"Pulse fixture Document") != items.end());
        ok &= Report(multi ? "mixed selection hides the Single verb" : "single selection shows the Single verb",
            (find(L"Pulse fixture Single") != items.end()) != multi);
        if (multi && ok) {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_NOASYNC;
            invoke.lpVerb = MAKEINTRESOURCEA(player->id - slot.id_first);
            invoke.lpVerbW = MAKEINTRESOURCEW(player->id - slot.id_first);
            invoke.nShow = SW_HIDE;
            const auto invoked = slot.menu->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
            const auto marker = [](fs::path path) { return path.concat(L".invoked"); };
            const auto deadline = GetTickCount64() + 3000;
            while (!fs::exists(marker(first)) && !fs::exists(marker(second)) && GetTickCount64() < deadline)
                Sleep(20);
            ok &= Report("invoke the Player command once through the retained mixed Shell menu",
                SUCCEEDED(invoked) && (fs::exists(marker(first)) || fs::exists(marker(second))));
        }
        ReleaseHandlerSlot(slot);
        return ok;
    };
    passed &= query(single, false);
    passed &= query(mixed, true);
    return passed;
}

} // namespace
