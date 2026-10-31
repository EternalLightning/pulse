// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "office_ooxml.h"
#include "document_literal_match.h"
#include "document_protocol.h"

#include <msopc.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <new>
#include <string_view>
#include <vector>

namespace pulse::index {
namespace {

using Microsoft::WRL::ComPtr;
constexpr ULONGLONG kMaxBytes = 64ull * 1024 * 1024;
constexpr size_t kMaxChars = 8u * 1024 * 1024;
constexpr HRESULT kInvalid = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
constexpr HRESULT kLimit = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);

enum class Kind { Word, Slide, PptComments, Shared, Sheet, Workbook, XComments };
struct Part { ComPtr<IOpcPart> value; Kind kind; bool main = false; };

// XmlLite only needs a sequential stream. Count actual bytes as well as checking
// the part's declared size, so oversized compressed content cannot bypass limits.
class BudgetStream final : public ISequentialStream {
public:
    BudgetStream(IStream* source, ULONGLONG& remaining, DocumentReadMetrics& metrics)
        : source_(source), remaining_(remaining), metrics_(metrics) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_ISequentialStream) return E_NOINTERFACE;
        *result = static_cast<ISequentialStream*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG count, ULONG* read) override {
        if (read) *read = 0;
        if (!count) return S_OK;
        DocumentMetricScope timing(metrics_.stream_read_us);
        ++metrics_.stream_reads;
        if (!remaining_) {
            BYTE probe = 0;
            ULONG actual = 0;
            const HRESULT hr = source_->Read(&probe, 1, &actual);
            return actual ? kLimit : hr;
        }
        const ULONG allowed = static_cast<ULONG>((std::min)(remaining_, static_cast<ULONGLONG>(count)));
        ULONG actual = 0;
        const HRESULT hr = source_->Read(data, allowed, &actual);
        metrics_.stream_bytes += actual;
        if (actual > remaining_) return kLimit;
        remaining_ -= actual;
        if (read) *read = actual;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
private:
    ULONG refs_ = 1;
    ComPtr<IStream> source_;
    ULONGLONG& remaining_;
    DocumentReadMetrics& metrics_;
};

bool Append(std::wstring& dest, std::wstring_view text) {
    if (text.size() > kMaxChars - dest.size()) return false;
    dest.append(text);
    return true;
}

bool Separator(std::wstring& dest) {
    return dest.empty() || dest.back() == L'\n' || Append(dest, L"\n");
}

bool Namespace(std::wstring_view ns, std::wstring_view family) {
    constexpr std::wstring_view transitional = L"http://schemas.openxmlformats.org/";
    constexpr std::wstring_view strict = L"http://purl.oclc.org/ooxml/";
    std::wstring_view suffix;
    if (ns.starts_with(transitional)) {
        ns.remove_prefix(transitional.size());
        suffix = L"/2006/main";
    } else if (ns.starts_with(strict)) {
        ns.remove_prefix(strict.size());
        suffix = L"/main";
    } else return false;
    if (!ns.starts_with(family)) return false;
    ns.remove_prefix(family.size());
    return ns == suffix;
}

std::wstring Attribute(IXmlReader* reader, const wchar_t* name) {
    std::wstring result;
    if (reader->MoveToAttributeByName(name, nullptr) == S_OK) {
        const wchar_t* text = nullptr;
        UINT length = 0;
        if (SUCCEEDED(reader->GetValue(&text, &length))) result.assign(text, length);
        reader->MoveToElement();
    }
    return result;
}

bool Classify(std::wstring_view type, Part& part) {
    constexpr std::wstring_view prefix = L"application/vnd.openxmlformats-officedocument.";
    if (!type.starts_with(prefix)) return false;
    type.remove_prefix(prefix.size());
    if (type == L"wordprocessingml.document.main+xml") { part.kind = Kind::Word; part.main = true; }
    else if (type == L"wordprocessingml.header+xml" || type == L"wordprocessingml.footer+xml" ||
             type == L"wordprocessingml.comments+xml" || type == L"wordprocessingml.footnotes+xml" ||
             type == L"wordprocessingml.endnotes+xml") part.kind = Kind::Word;
    else if (type == L"presentationml.presentation.main+xml") { part.kind = Kind::Slide; part.main = true; }
    else if (type == L"presentationml.slide+xml" || type == L"presentationml.notesSlide+xml") part.kind = Kind::Slide;
    else if (type == L"presentationml.comments+xml") part.kind = Kind::PptComments;
    else if (type == L"spreadsheetml.sheet.main+xml") { part.kind = Kind::Workbook; part.main = true; }
    else if (type == L"spreadsheetml.worksheet+xml") part.kind = Kind::Sheet;
    else if (type == L"spreadsheetml.sharedStrings+xml") part.kind = Kind::Shared;
    else if (type == L"spreadsheetml.comments+xml") part.kind = Kind::XComments;
    else return false;
    return true;
}

struct Element { std::wstring name; bool word = false; bool drawing = false; bool sheet = false; bool ppt = false; };

HRESULT ReadPart(const Part& part, ULONGLONG& remaining, std::vector<std::wstring>& shared,
                 size_t& shared_chars, std::wstring& output, DocumentLiteralMatch& match, DocumentReadMetrics& metrics) {
    DocumentMetricScope timing(metrics.parts_inclusive_us);
    DocumentMetricScope opening(metrics.part_open_us);
    ComPtr<IStream> source;
    HRESULT hr = part.value->GetContentStream(&source);
    if (FAILED(hr)) return hr;
    STATSTG stat{};
    if (FAILED(hr = source->Stat(&stat, STATFLAG_NONAME))) return hr;
    if (stat.cbSize.QuadPart > remaining) return kLimit;
    ComPtr<ISequentialStream> budget;
    budget.Attach(new (std::nothrow) BudgetStream(source.Get(), remaining, metrics));
    if (!budget) return E_OUTOFMEMORY;
    opening.Finish();
    ComPtr<IXmlReader> reader;
    if (FAILED(hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr))) return hr;
    if (FAILED(hr = reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit))) return hr;
    if (FAILED(hr = reader->SetProperty(XmlReaderProperty_MaxElementDepth, 128))) return hr;
    if (FAILED(hr = reader->SetInput(budget.Get()))) return hr;

    std::vector<Element> stack;
    std::wstring cell, cell_type, item;
    bool root_seen = false;
    bool in_cell = false;
    bool in_item = false;
    unsigned phonetic_depth = 0;
    auto end_element = [&](const Element& element) -> HRESULT {
        if (element.sheet && element.name == L"rPh") --phonetic_depth;
        if (part.kind == Kind::Shared && element.sheet && element.name == L"si") {
            if (shared.size() >= 1024 * 1024 || item.size() > kMaxChars - shared_chars) return kLimit;
            shared_chars += item.size();
            shared.push_back(std::move(item));
            item.clear();
            in_item = false;
        }
        if (part.kind == Kind::Sheet && element.sheet && element.name == L"c") {
            if (cell_type == L"s") {
                if (cell.empty()) return kInvalid;
                size_t index = 0;
                for (const wchar_t ch : cell) {
                    if (ch < L'0' || ch > L'9' || index > ((std::numeric_limits<size_t>::max)() - 9) / 10) return kInvalid;
                    index = index * 10 + static_cast<size_t>(ch - L'0');
                }
                if (index >= shared.size()) return kInvalid;
                if (!Append(output, shared[index])) return kLimit;
            } else if (!Append(output, cell)) return kLimit;
            if (!Separator(output)) return kLimit;
            in_cell = false;
        }
        if ((element.word && element.name == L"p") || (element.drawing && element.name == L"p") ||
            (part.kind == Kind::PptComments && element.ppt && element.name == L"text") ||
            (part.kind == Kind::XComments && element.sheet && element.name == L"comment")) {
            if (!Separator(output)) return kLimit;
        }
        return S_OK;
    };

    XmlNodeType node{};
    while ((hr = reader->Read(&node)) == S_OK) {
        if (match.Found(output)) return S_FALSE;
        if (node == XmlNodeType_Element) {
            const wchar_t* name = nullptr;
            const wchar_t* ns = nullptr;
            UINT name_length = 0, ns_length = 0;
            if (FAILED(hr = reader->GetLocalName(&name, &name_length))) return hr;
            if (FAILED(hr = reader->GetNamespaceUri(&ns, &ns_length))) return hr;
            const std::wstring_view uri(ns, ns_length);
            Element element{std::wstring(name, name_length), Namespace(uri, L"wordprocessingml"),
                            Namespace(uri, L"drawingml"), Namespace(uri, L"spreadsheetml"), Namespace(uri, L"presentationml")};
            if (!root_seen) {
                root_seen = true;
                const bool valid = part.kind == Kind::Word ? element.word :
                    (part.kind == Kind::Slide || part.kind == Kind::PptComments) ? element.ppt : element.sheet;
                if (!valid) return kInvalid;
                if (part.main && !((part.kind == Kind::Word && element.name == L"document") ||
                    (part.kind == Kind::Slide && element.name == L"presentation") ||
                    (part.kind == Kind::Workbook && element.name == L"workbook"))) return kInvalid;
            }
            if (element.sheet && element.name == L"rPh") ++phonetic_depth;
            if (part.kind == Kind::Shared && element.sheet && element.name == L"si") {
                if (in_item) return kInvalid;
                item.clear();
                in_item = true;
            }
            if (part.kind == Kind::Sheet && element.sheet && element.name == L"c") {
                if (in_cell) return kInvalid;
                in_cell = true;
                cell.clear();
                cell_type = Attribute(reader.Get(), L"t");
            }
            if (part.kind == Kind::Workbook && element.sheet && element.name == L"sheet") {
                if (!Append(output, Attribute(reader.Get(), L"name")) || !Separator(output)) return kLimit;
            }
            if ((element.word && (element.name == L"tab" || element.name == L"br" || element.name == L"cr")) ||
                (element.drawing && element.name == L"br")) {
                if (!Append(output, L"\n")) return kLimit;
            }
            if (reader->IsEmptyElement()) {
                if (FAILED(hr = end_element(element))) return hr;
            } else stack.push_back(std::move(element));
        } else if (node == XmlNodeType_EndElement) {
            if (stack.empty()) return kInvalid;
            if (FAILED(hr = end_element(stack.back()))) return hr;
            stack.pop_back();
        } else if (node == XmlNodeType_Text || node == XmlNodeType_CDATA || node == XmlNodeType_Whitespace) {
            if (stack.empty()) continue;
            const Element& element = stack.back();
            const wchar_t* value = nullptr;
            UINT length = 0;
            if (FAILED(hr = reader->GetValue(&value, &length))) return hr;
            const std::wstring_view text(value, length);
            if (part.kind == Kind::Shared && in_item && !phonetic_depth && element.sheet && element.name == L"t") {
                if (!Append(item, text)) return kLimit;
            } else if (part.kind == Kind::Sheet && in_cell && !phonetic_depth && element.sheet &&
                       (element.name == L"v" || (cell_type == L"inlineStr" && element.name == L"t"))) {
                if (!Append(cell, text)) return kLimit;
            } else if ((part.kind == Kind::Word && element.word && element.name == L"t") ||
                       (part.kind == Kind::Slide && element.drawing && element.name == L"t") ||
                       (part.kind == Kind::PptComments && element.ppt && element.name == L"text") ||
                       (part.kind == Kind::XComments && !phonetic_depth && element.sheet && element.name == L"t")) {
                if (!Append(output, text)) return kLimit;
            }
        }
    }
    if (FAILED(hr)) return hr;
    if (!root_seen || !stack.empty() || in_cell || in_item) return kInvalid;
    return Separator(output) ? S_OK : kLimit;
}

HRESULT Extract(const std::wstring& path, std::wstring& output, DocumentLiteralMatch& match, DocumentReadMetrics& metrics) {
    DocumentMetricScope package_timing(metrics.package_us);
    ComPtr<IOpcFactory> factory;
    HRESULT hr = CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IStream> stream;
    if (FAILED(hr = factory->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_READ, nullptr, FILE_ATTRIBUTE_NORMAL, &stream))) return hr;
    STATSTG stat{};
    if (FAILED(hr = stream->Stat(&stat, STATFLAG_NONAME))) return hr;
    // Pictures can dominate a package while its searchable XML stays small.
    // Keep the extracted-part budget separate from the on-disk document size.
    if (stat.cbSize.QuadPart > document::kMaximumFileBytes) return kLimit;
    ComPtr<IOpcPackage> package;
    if (FAILED(hr = factory->ReadPackageFromStream(stream.Get(), OPC_READ_DEFAULT, &package))) return hr;
    package_timing.Finish();
    DocumentMetricScope enumerate_timing(metrics.enumerate_us);
    ComPtr<IOpcPartSet> set;
    if (FAILED(hr = package->GetPartSet(&set))) return hr;
    ComPtr<IOpcPartEnumerator> enumerator;
    if (FAILED(hr = set->GetEnumerator(&enumerator))) return hr;
    std::vector<Part> parts;
    size_t main_count = 0, shared_count = 0, part_count = 0;
    BOOL next = FALSE;
    while (SUCCEEDED(hr = enumerator->MoveNext(&next)) && next) {
        if (++part_count > 65536) return kLimit;
        Part part;
        if (FAILED(hr = enumerator->GetCurrent(&part.value))) return hr;
        LPWSTR type = nullptr;
        if (FAILED(hr = part.value->GetContentType(&type))) return hr;
        const bool relevant = Classify(type, part);
        CoTaskMemFree(type);
        if (!relevant) continue;
        if (part.main) ++main_count;
        if (part.kind == Kind::Shared) ++shared_count;
        parts.push_back(std::move(part));
    }
    if (FAILED(hr)) return hr;
    if (main_count != 1 || shared_count > 1) return kInvalid;
    std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
        const auto rank = [](const Part& p) { return p.kind == Kind::Shared ? 0 : p.main ? 1 : 2; };
        return rank(a) < rank(b);
    });
    enumerate_timing.Finish();
    ULONGLONG remaining = kMaxBytes;
    std::vector<std::wstring> shared;
    size_t shared_chars = 0;
    for (const Part& part : parts) {
        hr = ReadPart(part, remaining, shared, shared_chars, output, match, metrics);
        if (FAILED(hr)) return hr;
        if (hr == S_FALSE || match.Found(output)) return S_OK;
    }
    return S_OK;
}

}

HRESULT ExtractOfficeOpenXml(const std::wstring& path, std::wstring& output,
                            std::wstring_view stop_needle, bool case_sensitive, DocumentReadMetrics* metrics) {
    DocumentReadMetrics local_metrics;
    if (metrics) *metrics = {};
    output.clear();
    try {
        std::wstring result;
        DocumentLiteralMatch match(stop_needle, case_sensitive);
        const HRESULT hr = Extract(path, result, match, metrics ? *metrics : local_metrics);
        if (SUCCEEDED(hr)) output.swap(result);
        return hr;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

}
