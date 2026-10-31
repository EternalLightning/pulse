// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "index/office_ooxml.h"
#include "index/document_protocol.h"
#include <winioctl.h>

#include <msopc.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
struct XmlPart { const wchar_t* path; const wchar_t* type; std::string xml; };
constexpr const wchar_t* kWord = L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
constexpr const wchar_t* kHeader = L"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml";
constexpr const wchar_t* kWorkbook = L"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml";
constexpr const wchar_t* kSheet = L"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml";
constexpr const wchar_t* kShared = L"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml";
constexpr const wchar_t* kPresentation = L"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml";
constexpr const char* kWordNs = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr const char* kSheetNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

HRESULT Package(const std::filesystem::path& path, const std::vector<XmlPart>& parts) {
    ComPtr<IOpcFactory> factory;
    HRESULT hr = CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IOpcPackage> package;
    if (FAILED(hr = factory->CreatePackage(&package))) return hr;
    ComPtr<IOpcPartSet> set;
    if (FAILED(hr = package->GetPartSet(&set))) return hr;
    for (const auto& entry : parts) {
        ComPtr<IOpcPartUri> uri;
        if (FAILED(hr = factory->CreatePartUri(entry.path, &uri))) return hr;
        ComPtr<IOpcPart> part;
        if (FAILED(hr = set->CreatePart(uri.Get(), entry.type, OPC_COMPRESSION_NORMAL, &part))) return hr;
        ComPtr<IStream> content;
        if (FAILED(hr = part->GetContentStream(&content))) return hr;
        ULONG written = 0;
        if (FAILED(hr = content->Write(entry.xml.data(), static_cast<ULONG>(entry.xml.size()), &written))) return hr;
        if (written != entry.xml.size()) return E_FAIL;
    }
    ComPtr<IStream> stream;
    if (FAILED(hr = factory->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_WRITE, nullptr, FILE_ATTRIBUTE_NORMAL, &stream))) return hr;
    return factory->WritePackageToStream(package.Get(), OPC_WRITE_DEFAULT, stream.Get());
}

std::string Word(std::string body) {
    return "<w:document xmlns:w=\"" + std::string(kWordNs) + "\"><w:body>" + body + "</w:body></w:document>";
}
std::string Sheet(std::string body) {
    return "<worksheet xmlns=\"" + std::string(kSheetNs) + "\"><sheetData><row>" + body + "</row></sheetData></worksheet>";
}
std::string Workbook() {
    return "<workbook xmlns=\"" + std::string(kSheetNs) + "\"><sheets><sheet name=\"季度报表\"/></sheets></workbook>";
}

}

int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 2;
    const auto directory = std::filesystem::path(L"bench_data") / L"office_ooxml_test" /
        (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    int failed = 0;
    auto check = [&](const char* name, const std::vector<XmlPart>& parts,
                     const std::vector<std::wstring>& contains, const std::vector<std::wstring>& absent,
                     bool success = true) {
        const auto path = directory / (std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(failed) + L".zip");
        const HRESULT created = Package(path, parts);
        std::wstring output = L"previous output";
        pulse::index::DocumentReadMetrics metrics;
        metrics.filter_us = 123; // The OOXML call must clear stale data from a previous document.
        const HRESULT hr = SUCCEEDED(created) ? pulse::index::ExtractOfficeOpenXml(path.native(), output, {}, false, &metrics) : created;
        bool pass = SUCCEEDED(created) && (success ? SUCCEEDED(hr) : FAILED(hr) && output.empty());
        pass = pass && metrics.filter_us == 0;
        if (success) pass = pass && metrics.stream_reads > 0 && metrics.stream_bytes > 0 &&
            metrics.parts_inclusive_us >= metrics.stream_read_us;
        for (const auto& text : contains) pass = pass && output.find(text) != std::wstring::npos;
        for (const auto& text : absent) pass = pass && output.find(text) == std::wstring::npos;
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << name << " HRESULT=0x" << std::hex
                  << static_cast<unsigned long>(hr) << std::dec << '\n';
        if (!pass) ++failed;
        std::filesystem::remove(path);
    };

    check("DOCX Chinese runs, table, header and excluded instructions", {
        {L"/word/document.xml", kWord, Word("<w:p><w:r><w:t>中文</w:t></w:r><w:r><w:t>检索</w:t></w:r><w:r><w:instrText>HIDDEN_FIELD</w:instrText></w:r></w:p><w:tbl><w:tr><w:tc><w:p><w:r><w:t>表格内容</w:t></w:r></w:p></w:tc></w:tr></w:tbl>")},
        {L"/word/header1.xml", kHeader, "<w:hdr xmlns:w=\"" + std::string(kWordNs) + "\"><w:p><w:r><w:t>页眉文字</w:t></w:r></w:p></w:hdr>"},
        {L"/word/styles.xml", L"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml", "<ignored>STYLE_SECRET</ignored>"}
    }, {L"中文检索\n表格内容", L"页眉文字"}, {L"HIDDEN_FIELD", L"STYLE_SECRET"});

    check("strict DOCX namespace", {{L"/word/document.xml", kWord,
        "<w:document xmlns:w=\"http://purl.oclc.org/ooxml/wordprocessingml/main\"><w:body><w:p><w:r><w:t>严格格式</w:t></w:r></w:p></w:body></w:document>"}}, {L"严格格式"}, {});

    check("XLSX real cells, rich shared and inline strings, cached formulas, comments", {
        {L"/xl/workbook.xml", kWorkbook, Workbook()},
        {L"/xl/sharedStrings.xml", kShared, "<sst xmlns=\"" + std::string(kSheetNs) + "\"><si><r><t>共享</t></r><r><t>文字</t></r><rPh><t>PHONETIC_ONLY</t></rPh></si><si><t>UNUSED_SECRET</t></si></sst>"},
        {L"/xl/worksheets/sheet1.xml", kSheet, Sheet("<c t=\"s\"><v>0</v></c><c t=\"inlineStr\"><is><r><t>内联</t></r><r><t>文本</t></r></is></c><c><v>123.5</v></c><c><f>FORMULA_SECRET</f><v>42</v></c><c t=\"str\"><f>IGNORED</f><v>公式结果</v></c>")},
        {L"/xl/comments1.xml", L"application/vnd.openxmlformats-officedocument.spreadsheetml.comments+xml", "<comments xmlns=\"" + std::string(kSheetNs) + "\"><authors><author>AUTHOR_SECRET</author></authors><commentList><comment><text><r><t>单元格</t></r><r><t>批注</t></r></text></comment></commentList></comments>"}
    }, {L"季度报表", L"共享文字\n内联文本\n123.5\n42\n公式结果", L"单元格批注"}, {L"UNUSED_SECRET", L"PHONETIC_ONLY", L"FORMULA_SECRET", L"AUTHOR_SECRET"});

    check("PPTX slide, table, notes and comments", {
        {L"/ppt/presentation.xml", kPresentation, "<p:presentation xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"/>"},
        {L"/ppt/slides/slide1.xml", L"application/vnd.openxmlformats-officedocument.presentationml.slide+xml", "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:p><a:r><a:t>幻灯</a:t></a:r><a:r><a:t>文字</a:t></a:r></a:p><a:tbl><a:tr><a:tc><a:p><a:r><a:t>演示表格</a:t></a:r></a:p></a:tc></a:tr></a:tbl></p:sld>"},
        {L"/ppt/notesSlides/notesSlide1.xml", L"application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml", "<p:notes xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:p><a:r><a:t>演讲备注</a:t></a:r></a:p></p:notes>"},
        {L"/ppt/comments/comment1.xml", L"application/vnd.openxmlformats-officedocument.presentationml.comments+xml", "<p:cmLst xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cm><p:text>演示批注</p:text></p:cm></p:cmLst>"}
    }, {L"幻灯文字\n演示表格", L"演讲备注", L"演示批注"}, {});

    check("malformed XML discards earlier text", {
        {L"/word/document.xml", kWord, Word("<w:p><w:r><w:t>已读取</w:t></w:r></w:p>")},
        {L"/word/header1.xml", kHeader, "<w:hdr xmlns:w=\"" + std::string(kWordNs) + "\"><w:p>"}
    }, {}, {}, false);
    check("DTD prohibited", {{L"/word/document.xml", kWord, "<!DOCTYPE document [<!ENTITY test 'EXPANDED'>]>" + Word("<w:p><w:r><w:t>&test;</w:t></w:r></w:p>")}}, {}, {}, false);
    check("invalid shared string index", {{L"/xl/workbook.xml", kWorkbook, Workbook()},
        {L"/xl/worksheets/sheet1.xml", kSheet, Sheet("<c t=\"s\"><v>999</v></c>")}}, {}, {}, false);
    check("output character limit", {{L"/word/document.xml", kWord,
        Word("<w:p><w:r><w:t>" + std::string(8u * 1024 * 1024 + 1, 'x') + "</w:t></w:r></w:p>")}}, {}, {}, false);
    check("decompressed part limit", {{L"/word/document.xml", kWord,
        Word("<w:ignored>" + std::string(64u * 1024 * 1024, 'x') + "</w:ignored>")}}, {}, {}, false);
    check("cumulative decompressed parts limit", {
        {L"/word/document.xml", kWord, Word("<w:ignored>" + std::string(33u * 1024 * 1024, 'x') + "</w:ignored>")},
        {L"/word/header1.xml", kHeader, "<w:hdr xmlns:w=\"" + std::string(kWordNs) + "\"><w:ignored>" +
            std::string(33u * 1024 * 1024, 'x') + "</w:ignored></w:hdr>"}
    }, {}, {}, false);

    const auto compound = directory / L"encrypted.docx";
    HANDLE compound_file = CreateFileW(compound.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const BYTE compound_header[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    DWORD compound_written = 0;
    const bool made_compound = compound_file != INVALID_HANDLE_VALUE &&
        WriteFile(compound_file, compound_header, sizeof(compound_header), &compound_written, nullptr) &&
        compound_written == sizeof(compound_header);
    if (compound_file != INVALID_HANDLE_VALUE) CloseHandle(compound_file);
    std::wstring compound_output = L"previous output";
    const HRESULT compound_hr = pulse::index::ExtractOfficeOpenXml(compound.native(), compound_output);
    const bool compound_pass = made_compound && FAILED(compound_hr) && compound_output.empty();
    std::cout << (compound_pass ? "[PASS] " : "[FAIL] ") << "encrypted Office compound-container signature rejected\n";
    if (!compound_pass) ++failed;

    const auto truncated = directory / L"truncated.docx";
    const HRESULT created_truncated = Package(truncated, {{L"/word/document.xml", kWord, Word("<w:p/>")}});
    if (SUCCEEDED(created_truncated)) std::filesystem::resize_file(truncated, 20);
    std::wstring truncated_output = L"previous output";
    const HRESULT truncated_hr = pulse::index::ExtractOfficeOpenXml(truncated.native(), truncated_output);
    const bool truncated_pass = SUCCEEDED(created_truncated) && FAILED(truncated_hr) && truncated_output.empty();
    std::cout << (truncated_pass ? "[PASS] " : "[FAIL] ") << "truncated ZIP rejected\n";
    if (!truncated_pass) ++failed;

    const auto large = directory / L"large.docx";
    HANDLE file = CreateFileW(large.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    size.QuadPart = pulse::index::document::kMaximumFileBytes + 1;
    DWORD returned = 0;
    const bool sparse = file != INVALID_HANDLE_VALUE &&
        DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    const bool made_large = sparse && SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    std::wstring output = L"previous output";
    const HRESULT large_hr = pulse::index::ExtractOfficeOpenXml(large.native(), output);
    const bool large_pass = made_large && large_hr == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE) && output.empty();
    std::cout << (large_pass ? "[PASS] " : "[FAIL] ") << "package input limit\n";
    if (!large_pass) ++failed;
    std::filesystem::remove_all(directory);
    CoUninitialize();
    return failed ? 1 : 0;
}
