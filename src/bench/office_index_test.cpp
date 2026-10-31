// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/content_index.h"
#include "../index/document_reader.h"
#include "../index/document_literal_match.h"
#include "../index/content_search_protocol.h"
#include <msopc.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cwctype>

namespace {
using namespace pulse;
using Microsoft::WRL::ComPtr;
int failures = 0;
void Check(bool pass, const char* message) {
    std::cout << (pass ? "[PASS] " : "[FAIL] ") << message << '\n';
    if (!pass) ++failures;
}
struct Part { const wchar_t* path; const wchar_t* type; std::string xml; OPC_COMPRESSION_OPTIONS compression = OPC_COMPRESSION_NORMAL; };
bool Package(const std::filesystem::path& path, const std::vector<Part>& parts) {
    ComPtr<IOpcFactory> factory; ComPtr<IOpcPackage> package; ComPtr<IOpcPartSet> set;
    if (FAILED(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreatePackage(&package)) || FAILED(package->GetPartSet(&set))) return false;
    for (const auto& item : parts) {
        ComPtr<IOpcPartUri> uri; ComPtr<IOpcPart> part; ComPtr<IStream> stream;
        ULONG n = 0;
        if (FAILED(factory->CreatePartUri(item.path, &uri)) ||
            FAILED(set->CreatePart(uri.Get(), item.type, item.compression, &part)) ||
            FAILED(part->GetContentStream(&stream)) ||
            FAILED(stream->Write(item.xml.data(), static_cast<ULONG>(item.xml.size()), &n)) || n != item.xml.size()) return false;
    }
    ComPtr<IStream> stream;
    return SUCCEEDED(factory->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_WRITE, nullptr, FILE_ATTRIBUTE_NORMAL, &stream)) &&
        SUCCEEDED(factory->WritePackageToStream(package.Get(), OPC_WRITE_DEFAULT, stream.Get()));
}
bool Word(const std::filesystem::path& path, const std::string& text) {
    return Package(path, {{L"/word/document.xml", L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>" +
        text + "</w:t></w:r></w:p></w:body></w:document>"}});
}
bool Sheet(const std::filesystem::path& path, const std::string& text) {
    return Package(path, {
        {L"/xl/workbook.xml", L"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml",
            "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>"},
        {L"/xl/worksheets/sheet1.xml", L"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml",
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row><c t=\"inlineStr\"><is><t>" +
        text + "</t></is></c></row></sheetData></worksheet>"}});
}
DWORD WorkerPid() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID == GetCurrentProcessId() && _wcsicmp(entry.szExeFile, L"Pulse.Document.exe") == 0) {
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) found = entry.th32ProcessID;
            if (process) CloseHandle(process);
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}
bool Pdf(const std::filesystem::path& path, const std::string& marker, bool text_layer = true) {
    const std::string content = text_layer ? "BT /F1 16 Tf 50 750 Td (" + marker +
        ") Tj 0 -25 Td /F2 16 Tf <5408540C91D1989D> Tj ET\n" : "0.5 g 50 50 100 100 re f\n";
    const std::string cmap = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Identity-UCS def /CMapType 2 def\n"
        "1 begincodespacerange <0000> <FFFF> endcodespacerange\n"
        "1 beginbfrange <0000> <FFFF> <0000> endbfrange\n"
        "endcmap CMapName currentdict /CMap defineresource pop end end\n";
    const std::vector<std::string> objects{
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R /F2 6 0 R >> >> /Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream",
        "<< /Type /Font /Subtype /Type0 /BaseFont /STSong-Light /Encoding /UniGB-UCS2-H /DescendantFonts [7 0 R] /ToUnicode 8 0 R >>",
        "<< /Type /Font /Subtype /CIDFontType0 /BaseFont /STSong-Light /CIDSystemInfo << /Registry (Adobe) /Ordering (GB1) /Supplement 4 >> /DW 1000 >>",
        "<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "endstream"};
    std::ostringstream out; out << "%PDF-1.4\n";
    std::vector<std::streamoff> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.tellp()); out << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
    }
    const auto xref = out.tellp();
    out << "xref\n0 " << objects.size() + 1 << "\n0000000000 65535 f \n";
    for (const auto offset : offsets) out << std::setfill('0') << std::setw(10) << offset << " 00000 n \n";
    out << "trailer\n<< /Size " << objects.size() + 1 << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
    std::ofstream file(path, std::ios::binary); file << out.str(); return file.good();
}
std::vector<index::ContentHit> Search(index::ContentIndex& db, const std::wstring& needle) {
    index::ContentSearchRequest request; request.needle = needle;
    std::atomic<bool> cancelled{false}; std::vector<index::ContentHit> hits;
    const bool ok = db.Search(request, cancelled, [&](const auto& status, auto batch) {
        Check(status.error == 0, "cached query status");
        hits.insert(hits.end(), batch.begin(), batch.end()); return true;
    });
    Check(ok, "cached query executes"); return hits;
}
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--literal-speed") {
        std::wstring body(16u*1024*1024,L'x');body.replace(body.size()-6,6,L" 3D3S ");
        const std::wstring needle=L"3d3s";const auto expected=body.size()-5;
        size_t legacy_positions[3]{},current_positions[3]{};
        auto begin=std::chrono::steady_clock::now();
        for(int i=0;i<3;++i) {
            const auto found=std::search(body.begin(),body.end(),needle.begin(),needle.end(),
                [](wchar_t a,wchar_t b){return towlower(a)==towlower(b);});
            legacy_positions[i]=found==body.end() ? std::wstring::npos:static_cast<size_t>(found-body.begin());
        }
        const double legacy_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        index::ContentSearchRequest request;request.needle=needle;
        begin=std::chrono::steady_clock::now();
        for(int i=0;i<3;++i) current_positions[i]=index::MatchCachedContent(body,request);
        const double current_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        bool equal=true;for(int i=0;i<3;++i) equal &= legacy_positions[i]==expected && current_positions[i]==legacy_positions[i];
        std::cout<<"literal ascii_bytes="<<body.size()<<" rounds=3 legacy_ms="<<legacy_ms<<" current_ms="<<current_ms<<'\n';
        Check(equal,"optimized literal search equals old towlower comparison in all three full-body scans");
        request.whole_word=true;
        Check(index::MatchCachedContent(body,request)==expected,"whole-word ASCII literal matches at separated tail");
        auto embedded=body;embedded.replace(embedded.size()-6,6,L"a3D3Sb");
        Check(index::MatchCachedContent(embedded,request)==std::wstring::npos,"whole-word mode rejects embedded literal");
        request.whole_word=false;request.case_sensitive=true;
        Check(index::MatchCachedContent(body,request)==std::wstring::npos,"case-sensitive mode rejects different letter case");
        request.needle=L"3D3S";
        Check(index::MatchCachedContent(body,request)==expected,"case-sensitive mode retains exact ASCII match");
        request.case_sensitive=false;request.needle=needle;request.excluded_needles={needle};
        Check(index::MatchCachedContent(body,request)==std::wstring::npos,"excluded literal rejects otherwise matching body");
        request.excluded_needles={L"AbsentExcludedMarker"};
        Check(index::MatchCachedContent(body,request)==expected,"absent exclusion preserves positive literal match");
        request.excluded_needles.clear();std::atomic<bool> cancelled{true};
        Check(index::MatchCachedContent(body,request,&cancelled)==std::wstring::npos,"cancelled literal scan does not return a match");
        cancelled=false;
        Check(index::MatchCachedContent(body,request,&cancelled)==expected,"subsequent uncancelled literal scan remains correct");
        CoUninitialize();return failures ? 1:0;
    }
    if (argc == 4 && (std::wstring_view(argv[1]) == L"--probe" || std::wstring_view(argv[1]) == L"--probe-match" ||
        std::wstring_view(argv[1]) == L"--probe-early")) {
        index::DocumentReadSession task;
        std::wstring body; uint64_t bytes = 0; DWORD error = 0;
        const auto maximum = std::wstring_view(argv[1]) == L"--probe" ? 64ull * 1024 * 1024 : 512ull * 1024 * 1024;
        const auto begin=std::chrono::steady_clock::now();
        const bool ok = index::ReadSearchableDocument(argv[2], maximum, body, bytes, &error,text::Encoding::Auto,{},
            std::wstring_view(argv[1]) == L"--probe-early" ? std::wstring_view(argv[3]) : std::wstring_view{});
        const double elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
        index::ContentSearchRequest request; request.needle = argv[3];
        const auto found = std::wstring_view(argv[1]) != L"--probe"
            ? index::MatchCachedContent(body, request) : body.find(argv[3]);
        Check(ok && found != std::wstring::npos, "document contains expected text");
        std::cout << "error=" << error << " input_bytes=" << bytes << " text_chars=" << body.size() << " elapsed_ms=" << elapsed_ms << '\n';
        return failures ? 1 : 0;
    }
    const auto base = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"office-index-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto root = base / L"files"; std::filesystem::create_directories(root);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--early-match") {
        const std::wstring split=std::wstring(4093,L'x')+L"BoundaryNeedle"+std::wstring(4096,L'x');
        index::DocumentLiteralMatch incremental(L"boundaryneedle",false);
        Check(!incremental.Found(std::wstring_view(split).substr(0,4096)) && incremental.Found(split),
            "incremental literal matcher recognizes a positive spanning the 4096 boundary");
        index::DocumentLiteralMatch absent(L"AbsentBoundaryNeedle",false);
        Check(!absent.Found(std::wstring_view(split).substr(0,4096)) && !absent.Found(split),
            "incremental literal matcher retains no-match across successive fragments");
        const auto word=root/L"runs.docx",rtf=root/L"boundary.rtf",pdf=root/L"early.pdf";
        const std::string tail(512u*1024,'x');
        Check(Package(word,{{L"/word/document.xml",L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p>"
            "<w:r><w:t>Early</w:t></w:r><w:r><w:t>Match</w:t></w:r><w:r><w:t>"+tail+
            "TailSentinel</w:t></w:r></w:p></w:body></w:document>"}}),"create DOCX cross-run match before long trailing content");
        {
            std::ofstream file(rtf);file<<"{\\rtf1\\ansi "<<std::string(4093,'x')<<"BoundaryNeedle"<<tail<<"RtfTailSentinel}";
            Check(file.good(),"create RTF match spanning a 4096-character filter read boundary");
        }
        Check(Pdf(pdf,"EarlyPdfMarker"),"create searchable PDF for early matching");
        std::wstring body; uint64_t bytes=0; DWORD error=0;
        {
            index::DocumentReadSession task;
            auto read=[&](const std::filesystem::path& path,std::wstring_view needle={},bool sensitive=false) {
                const auto begin=std::chrono::steady_clock::now();
                const bool ok=index::ReadSearchableDocument(path.wstring(),64ull*1024*1024,body,bytes,&error,text::Encoding::Auto,{},needle,sensitive);
                const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
                std::cout<<"early_read chars="<<body.size()<<" error="<<error<<" elapsed_ms="<<elapsed<<'\n';
                return ok;
            };
            Check(read(word) && body.find(L"TailSentinel")!=std::wstring::npos && body.size()>tail.size(),
                "default DOCX extraction retains entire long tail");
            const auto word_full=body;
            Check(read(word,L"earlymatch") && body.find(L"EarlyMatch")!=std::wstring::npos && body.size()<word_full.size() &&
                body.find(L"TailSentinel")==std::wstring::npos,"case-insensitive early match crosses XML runs and stops before tail");
            Check(read(word,L"EarlyMatch",true) && body.find(L"EarlyMatch")!=std::wstring::npos && body.size()<word_full.size(),
                "exact case-sensitive literal also terminates DOCX extraction early");
            Check(read(word,L"earlymatch",true) && body==word_full,"case-sensitive mismatch returns complete DOCX text");
            Check(read(word,L"AbsentDocumentNeedle") && body==word_full,"missing literal returns complete DOCX text");
            Check(read(word) && body==word_full,"default full extraction is not truncated by previous early reads");
            Check(read(rtf) && body.find(L"RtfTailSentinel")!=std::wstring::npos,"default IFilter extraction retains entire RTF tail");
            const auto rtf_full=body;const auto boundary=body.find(L"BoundaryNeedle");
            Check(boundary!=std::wstring::npos && boundary/4096!=(boundary+std::wstring_view(L"BoundaryNeedle").size()-1)/4096,
                "extracted RTF literal actually straddles the filter read boundary");
            Check(read(rtf,L"boundaryneedle") && body.find(L"BoundaryNeedle")!=std::wstring::npos && body.size()>4096 &&
                body.size()<=8192 && body.size()<rtf_full.size(),"IFilter early matcher retains overlap between 4096-character reads");
            Check(read(rtf,L"AbsentRtfNeedle") && body==rtf_full,"missing literal returns complete IFilter text");
            Check(read(pdf,L"earlypdfmarker") && body.find(L"EarlyPdfMarker")!=std::wstring::npos,
                "PDF early read returns the matching text through its MTA filter");
            Check(read(pdf) && body.find(L"合同金额")!=std::wstring::npos,"default PDF extraction retains remaining mapped Chinese content");
            Check(!index::ReadSearchableDocument(word.wstring(),64ull*1024*1024,body,bytes,&error,text::Encoding::Auto,[]{return true;},L"EarlyMatch") &&
                error==ERROR_CANCELLED && body.empty(),"cancellation never returns a partial early-match prefix");
        }
        Check(WorkerPid()==0,"cancelled early-match task releases its parser worker");
        std::cout<<"Fixture retained: "<<base.string()<<'\n';
        CoUninitialize();return failures ? 1:0;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"--corpus-index") {
        index::ContentIndexConfig config; config.roots = {{argv[2]}};
        index::ContentIndex db((base / L"corpus.sqlite").wstring());
        Check(db.Configure(config), "configure isolated corpus index");
        bool idle = false;
        for (unsigned attempt = 0; attempt < 12 && !idle; ++attempt) {
            idle = db.WaitUntilIdle(10000);
            const auto status = db.Status();
            std::cout << "corpus indexed=" << status.indexed_files << " skipped=" << status.skipped_files
                << " errors=" << status.errors << " pending=" << status.pending_files << std::endl;
        }
        Check(idle, "isolated corpus indexing completes");
        const auto hits = Search(db, argv[3]);
        const auto expected = static_cast<size_t>(_wcstoui64(argv[4], nullptr, 10));
        Check(db.Status().indexed_files == expected && hits.size() == expected,
            "every reference document is indexed and returned by content query");
        std::cout << "corpus matches=" << hits.size() << " expected=" << expected << '\n';
        std::cout << "Fixture retained: " << base.string() << '\n';
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--document-budget") {
        index::ContentIndexConfig config;
        config.maximum_document_bytes = 96ull * 1024 * 1024;
        ipc::PayloadWriter encoded; index::content::PutConfig(encoded, config);
        index::ContentIndexConfig decoded;
        ipc::PayloadReader reader(encoded.data().data(), encoded.data().size());
        Check(index::content::GetConfig(reader, decoded) && !reader.remaining() &&
            decoded.maximum_document_bytes == config.maximum_document_bytes, "document budget round trips independently");
        ipc::PayloadReader legacy(encoded.data().data(), encoded.data().size() - 8);
        Check(index::content::GetConfig(legacy, decoded) && !legacy.remaining() &&
            decoded.maximum_file_bytes == 64ull * 1024 * 1024 && decoded.maximum_document_bytes == 512ull * 1024 * 1024,
            "older config keeps text limit and adopts document budget");
        const auto large = root / L"large.docx";
        Check(Package(large, {
            {L"/word/document.xml", L"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml",
             "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p><w:r><w:t>LargeDocumentNeedle</w:t></w:r></w:p></w:body></w:document>"},
            {L"/word/media/unused.bin", L"application/octet-stream", std::string(65u * 1024 * 1024, 'x'), OPC_COMPRESSION_NONE}}),
            "create large package with small searchable XML");
        Check(std::filesystem::file_size(large) > 64ull * 1024 * 1024, "fixture exceeds previous document limit");
        {
            index::DocumentReadSession task;
            std::wstring body; uint64_t bytes = 0; DWORD error = 0;
            Check(index::ReadSearchableDocument(large.wstring(), config.maximum_document_bytes, body, bytes, &error) &&
                body.find(L"LargeDocumentNeedle") != std::wstring::npos, "large package extracts without reading unused payload");
            Check(!index::ReadSearchableDocument(large.wstring(), 64ull * 1024 * 1024, body, bytes, &error) &&
                error == ERROR_FILE_TOO_LARGE && body.empty(), "explicit smaller document budget remains enforced");
        }
        config.roots = {{root.wstring()}};
        const auto database = (base / L"budget.sqlite").wstring();
        {
            index::ContentIndex db(database);
            Check(db.Configure(config) && db.WaitUntilIdle(15000), "index large document under separate budget");
            Check(Search(db, L"LargeDocumentNeedle").size() == 1, "default query retains document over text limit");
        }
        {
            index::ContentIndex reopened(database);
            Check(reopened.WaitUntilIdle(15000) && reopened.Configuration().maximum_document_bytes == config.maximum_document_bytes,
                "document budget survives index restart");
        }
        std::cout << "Fixture retained: " << base.string() << '\n';
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--pdf-task") {
        const auto pdf = root / L"中文文档.pdf";
        const auto word = root / L"document.docx";
        Check(Pdf(pdf, "PulsePdfMarker") && Word(word, "OfficeTaskMarker"), "create isolated PDF and Office task fixtures");
        Check(index::IsIndexedContentExtension(L".PDF") && index::IsExtractedDocumentExtension(L".pdf"), "PDF enters document extraction and indexing");
        std::wstring body; uint64_t bytes = 0; DWORD error = 0;
        auto read = [&](const std::filesystem::path& file) {
            return index::ReadSearchableDocument(file.wstring(), 64ull * 1024 * 1024, body, bytes, &error);
        };
        {
            index::DocumentReadSession session;
            Check(read(pdf) && body.find(L"PulsePdfMarker") != std::wstring::npos && body.find(L"合同金额") != std::wstring::npos,
                "system PDF filter extracts English and mapped Chinese text");
            std::cout << "PDF error=" << error << " chars=" << body.size() << '\n';
            Check(WorkerPid() != 0, "parser belongs to the active task");
            Check(read(word) && body.find(L"OfficeTaskMarker") != std::wstring::npos, "Office STA works after PDF MTA extraction");
            Check(read(pdf) && body.find(L"PulsePdfMarker") != std::wstring::npos, "PDF works after Office in reused task parser");
            Check(!index::ReadSearchableDocument(pdf.wstring(), 1, body, bytes, &error) && error == ERROR_FILE_TOO_LARGE,
                "PDF input size limit remains enforced");
            const auto blank = base / L"no-text.pdf"; const auto bad = base / L"broken.pdf";
            Check(Pdf(blank, "", false), "create PDF without searchable text layer");
            std::ofstream(bad) << "broken PDF";
            Check(!read(blank) && error == ERROR_NOT_SUPPORTED && body.empty(), "textless PDF is reported unchecked rather than a complete no-match");
            Check(!read(bad) && error && body.empty() && read(pdf), "bad PDF cannot poison subsequent extraction");
        }
        Check(WorkerPid() == 0, "task completion releases parser immediately without idle delay");
        {
            index::DocumentReadSession session;
            Check(read(pdf), "start a fresh task parser");
            Check(!index::ReadSearchableDocument(pdf.wstring(), 64ull * 1024 * 1024, body, bytes, &error,
                text::Encoding::Auto, [] { return true; }) && error == ERROR_CANCELLED, "cancelled task returns no PDF text");
        }
        Check(WorkerPid() == 0, "cancelled task releases parser");
        index::ContentSearchRequest request; request.indexed = false; request.root = root.wstring(); request.needle = L"PulsePdfMarker";
        std::atomic<bool> cancel{false}; size_t hits = 0; index::ContentSearchProgress final;
        Check(index::RunContentSearch(request, cancel, [&](const auto& progress, auto batch) {
            final = progress; hits += batch.size(); return true;
        }) && hits == 1 && final.done && !final.error && final.total_files == 2 && final.scanned_files == 2,
            "on-demand PDF scan reports correct result and completed work count");
        Check(WorkerPid() == 0, "on-demand scan releases parser before returning");
        {
            index::ContentIndex db((base / L"pdf.sqlite").wstring());
            index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
            Check(db.Configure(config) && db.WaitUntilIdle(15000), "PDF and Office content indexing completes");
            Check(db.Status().indexed_files == 2 && db.Status().errors == 0, "both document types cached without errors");
            Check(Search(db, L"PulsePdfMarker").size() == 1 && Search(db, L"合同金额").size() == 1,
                "cached PDF searches preserve English and Chinese matches");
        }
        std::cout << "Fixture retained: " << base.string() << '\n';
        CoUninitialize(); return failures ? 1 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--legacy-filter") {
        const auto rtf=root / L"legacy.rtf";
        std::ofstream(rtf) << "{\\rtf1\\ansi 3d3s legacy content \\u21512?\\u21516?}";
        const auto word=root / L"word.docx";
        Check(Word(word,"ModernOfficeMarker"),"create modern Office companion fixture");
        std::wstring body; uint64_t bytes=0; DWORD error=0;
        {
            index::DocumentReadSession session;
            auto read=[&](const std::filesystem::path& path) {
                return index::ReadSearchableDocument(path.wstring(),64ull*1024*1024,body,bytes,&error);
            };
            Check(read(rtf) && !error && body.find(L"3d3s")!=std::wstring::npos && body.find(L"合同")!=std::wstring::npos,
                "Office IFilter requests index attributes and emits synthetic legacy body");
            const auto pid=WorkerPid();
            Check(read(word) && body.find(L"ModernOfficeMarker")!=std::wstring::npos && WorkerPid()==pid,
                "modern Office parsing remains valid after legacy filtering");
            Check(read(rtf) && body.find(L"3d3s")!=std::wstring::npos,
                "legacy filtering remains valid when task parser is reused");
            Check(!index::ReadSearchableDocument(rtf.wstring(),64ull*1024*1024,body,bytes,&error,text::Encoding::Auto,[]{return true;}) &&
                error==ERROR_CANCELLED && body.empty(),"legacy cancellation never returns partial body");
        }
        Check(WorkerPid()==0,"legacy task releases parser on completion");
        CoUninitialize(); return failures ? 1:0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--live-updates") {
        const auto path = root / L"new.xlsx";
        const auto database = (base / L"live.sqlite").wstring();
        uint64_t previous = 0;
        {
            index::ContentIndex db(database);
            Check(db.WaitUntilIdle(5000), "initialize isolated index");
            index::ContentIndexConfig config;
            config.roots = {{root.wstring()}, {(base / L"missing").wstring()}};
            Check(db.Configure(config) && db.WaitUntilIdle(5000), "index available and unavailable roots");
            Check(db.Status().error == ERROR_PATH_NOT_FOUND && Search(db, L"合同").empty(), "search before file creation with an unrelated root error");
            auto wait_change = [&](uint64_t old) {
                const auto deadline = GetTickCount64() + 10000;
                while (GetTickCount64() < deadline) {
                    const auto status = db.Status();
                    if (status.revision != old && !status.indexing && !status.pending_files) return true;
                    Sleep(50);
                }
                const auto status = db.Status();
                std::cout << "timeout: revision=" << status.revision << " previous=" << old << " files=" << status.indexed_files
                    << " skipped=" << status.skipped_files << " error=" << status.error << " pending=" << status.pending_files << '\n';
                return false;
            };
            previous = db.Status().revision;
            Check(Sheet(path, "\xe5\x90\x88\xe5\x90\x8c"), "save new Excel with contract content");
            Check(wait_change(previous) && Search(db, L"合同").size() == 1 && db.Status().error == ERROR_PATH_NOT_FOUND,
                "watcher indexes new Excel and publishes revision despite unrelated error");
            previous = db.Status().revision;
            const auto count = db.Status().indexed_files;
            Check(Sheet(path, "\xe5\x8f\x91\xe7\xa5\xa8"), "replace Excel cell content");
            Check(wait_change(previous) && db.Status().indexed_files == count && Search(db, L"合同").empty() && Search(db, L"发票").size() == 1,
                "same file count still publishes replacement and removes old match");
            previous = db.Status().revision;
            Check(DeleteFileW(path.c_str()) != FALSE, "delete isolated Excel fixture");
            Check(wait_change(previous) && Search(db, L"发票").empty(), "deletion publishes a revision and removes match");
            Check(Sheet(path, "LockedSaveMarker"), "create temporarily locked save");
            HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(locked != INVALID_HANDLE_VALUE, "hold Office file open for writing");
            Sleep(900);
            previous = db.Status().revision;
            if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
            Check(wait_change(previous) && Search(db, L"LockedSaveMarker").size() == 1,
                "occupied save is retried after release without waiting for a full crawl");
            const auto nested = root / L"nested";
            std::filesystem::create_directories(nested);
            Check(db.WaitUntilIdle(5000), "settle nested folder creation");
            const auto renamed = nested / L"renamed.xlsx";
            previous = db.Status().revision;
            Check(MoveFileExW(path.c_str(), renamed.c_str(), 0) != FALSE, "move Excel into nested folder");
            Check(wait_change(previous), "recursive notification updates moved Excel");
            auto moved = Search(db, L"LockedSaveMarker");
            Check(moved.size() == 1 && moved[0].path == renamed.wstring(), "rename removes old path and indexes the new path");
            previous = db.Status().revision;
            Check(MoveFileExW(renamed.c_str(), (base / L"outside.xlsx").c_str(), 0) != FALSE, "move fixture out of indexed scope");
            Check(wait_change(previous) && Search(db, L"LockedSaveMarker").empty(), "unpaired old-name notification removes moved-out file promptly");
            previous = db.Status().revision;
            std::ofstream(root / L"ignored.bin") << "ignored";
            const auto deadline = GetTickCount64() + 5000;
            while (GetTickCount64() < deadline && db.Status().skipped_files == 0) Sleep(50);
            Check(db.Status().skipped_files == 1 && db.Status().revision == previous, "unsearchable file changes do not invalidate cached results");
            ipc::PayloadWriter payload; index::content::PutStatus(payload, db.Status()); index::content::PutConfig(payload, config);
            ipc::PayloadReader reader(payload.data().data(), payload.data().size());
            index::ContentIndexStatus decoded; index::ContentIndexConfig decoded_config;
            Check(index::content::GetStatus(reader, decoded) && decoded.revision == previous &&
                index::content::GetConfig(reader, decoded_config) && decoded_config.roots.size() == 2 && reader.remaining() == 0,
                "status revision round-trips without corrupting following config");
        }
        {
            index::ContentIndex reopened(database);
            Check(reopened.WaitUntilIdle(5000) && reopened.Status().revision > previous, "writer restart invalidates stale results");
        }
        CoUninitialize();
        return failures ? 1 : 0;
    }
    const auto word = root / L"中文合同.docx";
    Check(Word(word, "OfficeAlpha \xe5\x90\x88\xe5\x90\x8c\xe9\x87\x91\xe9\xa2\x9d"), "create Word fixture");
    if (argc == 2 && std::wstring_view(argv[1]) == L"--worker-limits") {
        std::wstring body; uint64_t bytes = 0; DWORD error = 0;
        auto read = [&] { return index::ReadSearchableDocument(word.wstring(), 64ull * 1024 * 1024, body, bytes, &error); };
        Check(read(), "start bounded worker");
        const DWORD pid = WorkerPid();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{}; entry.dwSize = sizeof(entry); unsigned suspended = 0;
        if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) do {
            if (pid && entry.th32OwnerProcessID == pid) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread) { if (SuspendThread(thread) != static_cast<DWORD>(-1)) ++suspended; CloseHandle(thread); }
            }
        } while (Thread32Next(snapshot, &entry));
        if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
        Check(suspended > 0, "simulate a hung extraction child owned by this fixture");
        const auto start = GetTickCount64();
        Check(!read() && error == ERROR_TIMEOUT && body.empty() && GetTickCount64() - start < 35000,
            "30-second timeout kills hung worker and returns no partial body");
        Check(read() && WorkerPid() != pid, "normal extraction recovers after timeout");
        const auto idle_start = GetTickCount64();
        while (WorkerPid() && GetTickCount64() - idle_start < 35000) Sleep(100);
        Check(WorkerPid() == 0, "worker exits after 30 seconds idle");
        Check(read(), "new work restarts an idle-expired worker");
        return failures ? 1 : 0;
    }
    Check(Package(root / L"table.xlsx", {
        {L"/xl/workbook.xml", L"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml",
            "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheets><sheet name=\"Quarter\"/></sheets></workbook>"},
        {L"/xl/worksheets/sheet1.xml", L"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml",
            "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData><row><c t=\"inlineStr\"><is><t>OfficeAlphaSheet</t></is></c><c><v>12345</v></c></row></sheetData></worksheet>"}}), "create Excel fixture");
    Check(Package(root / L"slides.pptx", {
        {L"/ppt/presentation.xml", L"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml",
            "<p:presentation xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"/>"},
        {L"/ppt/slides/slide1.xml", L"application/vnd.openxmlformats-officedocument.presentationml.slide+xml",
            "<p:sld xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"><a:p><a:r><a:t>OfficeAlphaSlide</a:t></a:r></a:p></p:sld>"}}), "create PowerPoint fixture");
    std::ofstream(root / L"native.txt") << "NativeAlpha";
    std::ofstream(base / L"invalid.pdf") << "OfficeAlphaPDF";
    Check(index::IsIndexedContentExtension(L".DOCX") && index::IsOfficeDocumentExtension(L".xls") &&
        index::IsIndexedContentExtension(L".pdf"), "Office and PDF admitted");
    std::wstring body; uint64_t bytes = 0; DWORD error = 0;
    auto read = [&](const std::filesystem::path& p, const std::function<bool()>& cancel = {}) {
        return index::ReadSearchableDocument(p.wstring(), 64ull * 1024 * 1024, body, bytes, &error, text::Encoding::Auto, cancel);
    };
    Check(read(root / L"native.txt") && body == L"NativeAlpha" && WorkerPid() == 0, "native text does not start document worker");
    Check(read(word) && body.find(L"合同金额") != std::wstring::npos, "worker extracts Chinese Word content");
    const DWORD first_pid = WorkerPid();
    Check(first_pid != 0 && read(root / L"table.xlsx") && body.find(L"12345") != std::wstring::npos && WorkerPid() == first_pid,
        "one worker reused for Word and Excel");
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, first_pid);
    PROCESS_MEMORY_COUNTERS counters{}; counters.cb = sizeof(counters);
    if (process && GetProcessMemoryInfo(process, &counters, sizeof(counters)))
        std::cout << "Worker peak working set: " << counters.PeakWorkingSetSize / 1024 << " KiB\n";
    if (process) CloseHandle(process);
    Check(!read(base / L"invalid.pdf") && error != 0 && body.empty(), "invalid PDF is rejected by the isolated parser");
    Check(!read(word, [] { return true; }) && error == ERROR_CANCELLED && body.empty(), "pre-cancel leaves no partial text");
    const auto huge = base / L"cancel.docx";
    Check(Word(huge, std::string(7 * 1024 * 1024, 'x')), "create cancellation fixture");
    int polls = 0;
    Check(!read(huge, [&] { return ++polls > 3; }) && error == ERROR_CANCELLED && body.empty(), "cancel active extraction and discard output");
    Check(read(word) && WorkerPid() != first_pid, "worker restarts after cancellation");
    Check(!index::ReadSearchableDocument(word.wstring(), 1, body, bytes, &error) &&
        error == ERROR_FILE_TOO_LARGE && body.empty(), "compressed input size budget");
    const auto bad = base / L"corrupt.docx"; std::ofstream(bad) << "not a package";
    Check(!read(bad) && error != 0 && body.empty() && read(word), "malformed Office failure does not poison next file");
    const auto legacy = base / L"invalid.xls"; std::ofstream(legacy) << "not an Excel workbook";
    Check(!read(legacy) && error != 0 && body.empty(), "legacy XLS reports missing filter or invalid data");
    std::cout << "Legacy XLS error: " << error << '\n';
    {
        index::ContentIndex db((base / L"index.sqlite").wstring());
        index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
        Check(db.Configure(config) && db.WaitUntilIdle(15000), "Office indexing completes");
        Check(db.Status().indexed_files == 4 && db.Status().errors == 0, "index contains native text and three Office formats only");
        Check(Search(db, L"合同金额").size() == 1 && Search(db, L"12345").size() == 1 &&
            Search(db, L"OfficeAlphaSlide").size() == 1, "cached FTS searches extracted bodies");
        const DWORD cached_pid = WorkerPid();
        process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, cached_pid);
        Check(process && TerminateProcess(process, 0) && WaitForSingleObject(process, 1000) == WAIT_OBJECT_0,
            "stop this fixture's extraction child");
        if (process) CloseHandle(process);
        Check(Search(db, L"合同金额").size() == 1 && WorkerPid() == 0, "cached queries do not start a document parser");
        Check(Word(word, "UpdatedOfficeMarker"), "modify Word fixture");
        bool refreshed = false;
        for (int i = 0; i < 100 && !refreshed; ++i) {
            Sleep(50);
            index::ContentSearchRequest request; request.needle = L"UpdatedOfficeMarker";
            std::atomic<bool> cancelled{false};
            db.Search(request, cancelled, [&](const auto&, auto hits) { refreshed |= !hits.empty(); return true; });
        }
        Check(refreshed && Search(db, L"合同金额").empty(), "changed document replaces cached text");
    }
    index::ContentSearchRequest request; request.indexed = false; request.root = root.wstring(); request.needle = L"OfficeAlphaSlide";
    std::atomic<bool> cancelled{false}; size_t hits = 0;
    Check(index::RunContentSearch(request, cancelled, [&](const auto&, auto batch) { hits += batch.size(); return true; }) && hits == 1,
        "on-demand folder scan also extracts Office text");
    std::cout << "Fixture retained: " << base.string() << '\n';
    CoUninitialize();
    return failures ? 1 : 0;
}
