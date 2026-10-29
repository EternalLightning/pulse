#include "../preview_host/metafile_input.h"
#include "../preview_host/metafile_raster.h"
#include <objbase.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#undef min
#undef max
using std::max;
using std::min;
#include <gdiplus.h>

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!value) ++failures;
}
void Word(std::vector<BYTE>& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void Dword(std::vector<BYTE>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
bool Write(const std::filesystem::path& path, const std::vector<BYTE>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    return !!file;
}
std::vector<BYTE> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool WriteEmf(const std::filesystem::path& path, bool plus) {
    HDC reference = GetDC(nullptr);
    if (!reference) return false;
    bool written = false;
    if (plus) {
        Gdiplus::Metafile metafile(path.c_str(), reference, Gdiplus::EmfTypeEmfPlusOnly, L"Pulse budget test");
        if (metafile.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Graphics graphics(&metafile);
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0, 120, 215));
            written = graphics.FillRectangle(&brush, 20, 10, 160, 80) == Gdiplus::Ok;
        }
    } else {
        const RECT frame{0, 0, 20000, 10000};
        HDC drawing = CreateEnhMetaFileW(reference, path.c_str(), &frame, L"Pulse budget test\0");
        if (drawing) {
            Rectangle(drawing, 10, 10, 190, 90);
            HENHMETAFILE file = CloseEnhMetaFile(drawing);
            written = file != nullptr;
            if (file) DeleteEnhMetaFile(file);
        }
    }
    ReleaseDC(nullptr, reference);
    return written;
}
std::vector<BYTE> Wmf() {
    HDC drawing = CreateMetaFileW(nullptr);
    if (!drawing) return {};
    SetMapMode(drawing, MM_ANISOTROPIC);
    SetWindowExtEx(drawing, 200, 100, nullptr);
    Rectangle(drawing, 10, 10, 190, 90);
    HMETAFILE file = CloseMetaFile(drawing);
    if (!file) return {};
    const UINT bytes = GetMetaFileBitsEx(file, 0, nullptr);
    std::vector<BYTE> result(22 + bytes, 0);
    const UINT copied = GetMetaFileBitsEx(file, bytes, result.data() + 22);
    DeleteMetaFile(file);
    if (copied != bytes) return {};
    Dword(result, 0, 0x9ac6cdd7);
    Word(result, 10, 200); Word(result, 12, 100); Word(result, 14, 1440);
    uint16_t checksum = 0;
    for (size_t offset = 0; offset < 20; offset += 2) {
        uint16_t value = 0; std::memcpy(&value, result.data() + offset, sizeof(value)); checksum ^= value;
    }
    Word(result, 20, checksum);
    return result;
}
bool Raster(const std::filesystem::path& path) {
    std::vector<unsigned char> pixels;
    UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0;
    std::wstring error;
    if (!pulse::preview::RasterizeMetaFile(path.wstring(), 256, pixels, width, height, stride,
        source_width, source_height, &error) || !error.empty() || !width || !height ||
        width > 256 || height > 256 || !source_width || !source_height ||
        pixels.size() != static_cast<size_t>(stride) * height) return false;
    for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
        if (pixels[offset + 3] && (pixels[offset] < 240 || pixels[offset + 1] < 240 || pixels[offset + 2] < 240))
            return true;
    }
    return false;
}
}

int wmain() {
    using namespace pulse::preview;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Check(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "initialize native metafile fixture writer");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"metafile-budget-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    Check(WriteEmf(root / L"drawing.emf", false), "write real GDI EMF fixture");
    Check(WriteEmf(root / L"drawing-plus.emf", true), "write real GDI+ EMF+ fixture");
    auto emf = Read(root / L"drawing.emf");
    const auto emf_plus = Read(root / L"drawing-plus.emf");
    auto wmf = Wmf();
    Check(ValidateMetaFileInput(emf.data(), emf.size()) && ValidateMetaFileInput(emf_plus.data(), emf_plus.size()),
        "input preflight accepts ordinary EMF and EMF+ containers");
    Check(ValidateMetaFileInput(wmf.data(), wmf.size()) && Write(root / L"drawing.wmf", wmf),
        "input preflight accepts real placeable WMF records and checksum");
    if (wmf.size() >= 22) Check(ValidateMetaFileInput(wmf.data() + 22, wmf.size() - 22), "input preflight accepts standard WMF without placeable header");
    Check(Raster(root / L"drawing.emf"), "bounded stream renders actual EMF ink within the requested size");
    Check(Raster(root / L"drawing-plus.emf"), "bounded stream preserves EMF+ rendering");
    Check(Raster(root / L"drawing.wmf"), "bounded stream preserves placeable WMF rendering");
    Check(Raster(std::filesystem::path(L"\\\\?\\" + (root / L"drawing.emf").wstring())), "bounded stream accepts extended Windows paths");
    if (emf.size() >= 88) {
        auto invalid = emf; Dword(invalid, 48, MAXDWORD);
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject EMF declared byte count outside the input");
        invalid = emf; Dword(invalid, 52, static_cast<uint32_t>(kMaxMetaFileRecords + 1));
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject excessive EMF declared record count");
        invalid = emf; Dword(invalid, 4, 0);
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject zero-length EMF header records");
        invalid = emf; Dword(invalid, 60, MAXDWORD); Dword(invalid, 64, 88);
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject EMF description length outside its header");
        invalid = emf; invalid.pop_back();
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject truncated EMF records");
        invalid = emf; Dword(invalid, 40, 0);
        Check(Write(root / L"invalid.emf", invalid), "write malformed metafile fixture");
        std::vector<unsigned char> pixels{1}; std::wstring error;
        UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0;
        Check(!RasterizeMetaFile((root / L"invalid.emf").wstring(), 256, pixels, width, height, stride,
            source_width, source_height, &error) && pixels.empty() && error == L"metafile-input-invalid",
            "malformed container fails before the native decoder without output pixels");
    }
    if (wmf.size() >= 46) {
        auto invalid = wmf; invalid[20] ^= 1;
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject invalid placeable WMF checksum");
        invalid = wmf; Dword(invalid, 22 + 18, 0);
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject zero-length WMF records");
        invalid = wmf; Dword(invalid, 22 + 18, MAXDWORD);
        Check(!ValidateMetaFileInput(invalid.data(), invalid.size()), "reject WMF record lengths beyond the captured input");
    }
    std::vector<BYTE> many(18 + (kMaxMetaFileRecords + 1) * 6, 0);
    Word(many, 0, 1); Word(many, 2, 9); Word(many, 4, 0x300);
    Dword(many, 6, static_cast<uint32_t>(many.size() / 2)); Dword(many, 12, 3);
    for (size_t offset = 18; offset < many.size(); offset += 6) {
        Dword(many, offset, 3);
        Word(many, offset + 4, static_cast<uint16_t>(offset + 6 == many.size() ? 0 : META_SAVEDC));
    }
    Check(!ValidateMetaFileInput(many.data(), many.size()), "reject WMF actual record count over the parser budget");
    std::vector<BYTE> oversized(kMaxMetaFileBytes + 1, 0);
    Check(Write(root / L"oversized.wmf", oversized), "write isolated oversized metafile");
    std::vector<unsigned char> pixels{1}; std::wstring error;
    UINT width = 0, height = 0, stride = 0, source_width = 0, source_height = 0;
    Check(!RasterizeMetaFile((root / L"oversized.wmf").wstring(), 256, pixels, width, height, stride,
        source_width, source_height, &error) && pixels.empty() && error == L"metafile-input-too-large",
        "oversized input fails before allocation and native parsing");
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    Check(!cleanup, "isolated metafile budget fixtures cleaned up");
    // RasterizeMetaFile retains its process-wide GDI+ session, matching the host.
    return failures ? 1 : 0;
}
