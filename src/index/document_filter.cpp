// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "document_filter.h"
#include "document_protocol.h"
#include "document_literal_match.h"
#include <filter.h>
#include <filterr.h>
#include <wrl/client.h>
#include <shlwapi.h>

namespace pulse::index {
namespace {
HRESULT LoadStreamFilter(const std::wstring& path, Microsoft::WRL::ComPtr<IFilter>& filter,
                         Microsoft::WRL::ComPtr<IStream>& stream) {
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    wchar_t handler[40]{}; DWORD bytes = sizeof(handler);
    auto key = path.substr(dot) + L"\\PersistentHandler";
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, handler, &bytes) != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    key = L"CLSID\\" + std::wstring(handler) + L"\\PersistentAddinsRegistered\\{89BCB740-6119-101A-BCB7-00DD010655AF}";
    wchar_t registered[40]{}; bytes = sizeof(registered);
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, registered, &bytes) != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    CLSID clsid{};
    HRESULT hr = CLSIDFromString(registered, &clsid);
    if (FAILED(hr)) return hr;
    filter.Reset();
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filter));
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IPersistStream> persist;
    hr = filter.As(&persist);
    if (FAILED(hr)) return hr;
    hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, FILE_ATTRIBUTE_NORMAL,
        FALSE, nullptr, &stream);
    return FAILED(hr) ? hr : persist->Load(stream.Get());
}
}
HRESULT ExtractOfficeFilter(const std::wstring& path, std::wstring& output,
                           std::wstring_view stop_needle, bool case_sensitive) {
    output.clear();
    // Resolve the OS loader only inside the disposable extraction process.
    // Machines without Windows Search or a registered format handler remain
    // usable; that document is reported as unsupported rather than indexed raw.
    HMODULE module = LoadLibraryExW(L"query.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    struct Module { HMODULE value; ~Module() { FreeLibrary(value); } } owner{module};
    using Load = HRESULT (WINAPI*)(const wchar_t*, IUnknown*, void**);
    const auto load = reinterpret_cast<Load>(GetProcAddress(module, "LoadIFilter"));
    if (!load) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    Microsoft::WRL::ComPtr<IFilter> filter;
    Microsoft::WRL::ComPtr<IStream> stream;
    HRESULT hr = load(path.c_str(), nullptr, reinterpret_cast<void**>(filter.GetAddressOf()));
    // Windows' PDF filter implements IPersistStream but not IPersistFile,
    // which the legacy LoadIFilter entry point attempts to use.
    if (hr == E_NOINTERFACE) hr = LoadStreamFilter(path, filter, stream);
    if (FAILED(hr)) return hr;
    if (!filter) return E_UNEXPECTED;
    ULONG flags = 0;
    // With explicit normalization flags, Office's filter otherwise succeeds
    // but emits no chunks. Request indexable content explicitly.
    hr = filter->Init(IFILTER_INIT_CANON_PARAGRAPHS | IFILTER_INIT_HARD_LINE_BREAKS |
        IFILTER_INIT_CANON_HYPHENS | IFILTER_INIT_CANON_SPACES | IFILTER_INIT_DISABLE_EMBEDDED |
        IFILTER_INIT_APPLY_INDEX_ATTRIBUTES,
        0, nullptr, &flags);
    if (FAILED(hr)) return hr;
    std::wstring body;
    DocumentLiteralMatch match(stop_needle, case_sensitive);
    STAT_CHUNK chunk{};
    for (uint32_t chunks = 0; chunks < 1000000; ++chunks) {
        hr = filter->GetChunk(&chunk);
        if (hr == FILTER_E_END_OF_CHUNKS) { output = std::move(body); return S_OK; }
        if (FAILED(hr)) return hr;
        if (!(chunk.flags & CHUNK_TEXT)) continue;
        if (!body.empty() && chunk.breakType != CHUNK_NO_BREAK) {
            if (body.size() >= document::kMaximumTextChars) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            body += chunk.breakType == CHUNK_EOW ? L' ' : L'\n';
        }
        for (;;) {
            wchar_t buffer[4096]{};
            ULONG count = ARRAYSIZE(buffer);
            hr = filter->GetText(&count, buffer);
            if (hr == FILTER_E_NO_MORE_TEXT || hr == FILTER_E_NO_TEXT) break;
            if (FAILED(hr)) return hr;
            if (count > ARRAYSIZE(buffer)) return E_UNEXPECTED;
            if (count > document::kMaximumTextChars - body.size()) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
            body.append(buffer, count);
            if (match.Found(body)) { output = std::move(body); return S_OK; }
            if (hr == FILTER_S_LAST_TEXT) break;
            if (!count) return E_UNEXPECTED;
        }
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
}
}
