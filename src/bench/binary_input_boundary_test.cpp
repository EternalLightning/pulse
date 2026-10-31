// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../index/usn_record.h"
#include "../preview_host/metafile_input.h"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using Bytes = std::vector<BYTE>;
int checks = 0;
int failures = 0;

void Check(bool passed, const char* label) {
    ++checks;
    if (!passed) ++failures;
    std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
}

void Word(Bytes& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void Dword(Bytes& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

Bytes Usn(std::wstring_view name, WORD name_offset = 64) {
    const size_t length = (name_offset + name.size() * sizeof(WCHAR) + 7) & ~size_t{7};
    Bytes bytes(length, 0);
    USN_RECORD_V2 record{};
    record.RecordLength = static_cast<DWORD>(length);
    record.MajorVersion = 2;
    record.FileReferenceNumber = 42;
    record.ParentFileReferenceNumber = 7;
    record.Usn = 9001;
    record.Reason = USN_REASON_FILE_CREATE;
    record.FileNameLength = static_cast<WORD>(name.size() * sizeof(WCHAR));
    record.FileNameOffset = name_offset;
    std::memcpy(bytes.data(), &record, offsetof(USN_RECORD_V2, FileName));
    std::memcpy(bytes.data() + name_offset, name.data(), record.FileNameLength);
    return bytes;
}

template<class Mutation>
void RejectUsn(const Bytes& original, const char* label, Mutation mutate) {
    auto bytes = original;
    mutate(bytes);
    pulse::index::UsnRecordView view;
    Check(!pulse::index::ReadUsnRecord(bytes.data(), bytes.size(), view) &&
        !pulse::index::ValidateUsnRecords(bytes.data(), bytes.size()), label);
}

void TestUsn() {
    using namespace pulse::index;
    static_assert(sizeof(WCHAR) == 2);
    const auto bytes = Usn(L"ok");
    UsnRecordView view;
    Check(ReadUsnRecord(bytes.data(), bytes.size(), view) && view.name == L"ok" &&
        view.record.FileReferenceNumber == 42 && view.record.ParentFileReferenceNumber == 7 &&
        view.record.Usn == 9001 && view.record.Reason == USN_REASON_FILE_CREATE,
        "USN V2 reads bounded fixed fields and the nondefault filename offset");
    const auto canonical = Usn(L"xy", static_cast<WORD>(offsetof(USN_RECORD_V2, FileName)));
    Check(ReadUsnRecord(canonical.data(), canonical.size(), view) && view.name == L"xy",
        "USN V2 accepts a filename immediately following its fixed header");
    Check(ValidateUsnRecords(nullptr, 0), "USN empty batch needs no input pointer");
    Check(!ReadUsnRecord(nullptr, bytes.size(), view) && !ValidateUsnRecords(nullptr, bytes.size()),
        "USN nonempty input rejects a null pointer");
    bool truncated = true;
    for (size_t size = 0; size < bytes.size(); ++size)
        truncated = !ReadUsnRecord(bytes.data(), size, view) && truncated;
    Check(truncated, "USN rejects every proper prefix of a complete record");
    RejectUsn(bytes, "USN zero record length cannot stall iteration", [](Bytes& b) { Dword(b, 0, 0); });
    RejectUsn(bytes, "USN short record cannot expose fixed fields", [](Bytes& b) { Dword(b, 0, 56); });
    RejectUsn(bytes, "USN record length cannot exceed available bytes", [](Bytes& b) { Dword(b, 0, MAXDWORD); });
    RejectUsn(bytes, "USN record length requires eight-byte alignment", [](Bytes& b) { Dword(b, 0, 68); });
    RejectUsn(bytes, "USN unsupported major version is rejected", [](Bytes& b) { Word(b, 4, 3); });
    RejectUsn(bytes, "USN unsupported minor version is rejected", [](Bytes& b) { Word(b, 6, 1); });
    constexpr size_t name_offset_field = offsetof(USN_RECORD_V2, FileNameOffset);
    constexpr size_t name_length_field = offsetof(USN_RECORD_V2, FileNameLength);
    RejectUsn(bytes, "USN filename cannot overlap the fixed header", [](Bytes& b) { Word(b, name_offset_field, 58); });
    RejectUsn(bytes, "USN filename offset requires UTF-16 alignment", [](Bytes& b) { Word(b, name_offset_field, 63); });
    RejectUsn(bytes, "USN filename offset cannot exceed its record", [](Bytes& b) { Word(b, name_offset_field, 74); });
    RejectUsn(bytes, "USN filename length requires complete UTF-16 units", [](Bytes& b) { Word(b, name_length_field, 3); });
    RejectUsn(bytes, "USN filename length cannot extend beyond its record", [](Bytes& b) { Word(b, name_length_field, 10); });
    auto empty_name = bytes;
    Word(empty_name, name_offset_field, static_cast<WORD>(empty_name.size()));
    Word(empty_name, name_length_field, 0);
    Check(ReadUsnRecord(empty_name.data(), empty_name.size(), view) && view.name.empty(),
        "USN accepts a zero-length filename at the record boundary");
    const auto second = Usn(L"second", 72);
    auto batch = bytes;
    batch.insert(batch.end(), second.begin(), second.end());
    Check(ValidateUsnRecords(batch.data(), batch.size()), "USN validates concatenated records of different lengths");
    Dword(batch, bytes.size(), 0);
    Check(!ValidateUsnRecords(batch.data(), batch.size()), "USN batch rejects a malformed record after a valid prefix");
    batch = bytes;
    batch.insert(batch.end(), second.begin(), second.end() - 1);
    Check(!ValidateUsnRecords(batch.data(), batch.size()), "USN batch rejects a truncated final record");
    batch = bytes;
    batch.push_back(0);
    Check(!ValidateUsnRecords(batch.data(), batch.size()), "USN batch rejects an incomplete trailing header");
    view.name = L"stale";
    view.record.RecordLength = 123;
    Check(!ReadUsnRecord(nullptr, 0, view) && view.name.empty() && !view.record.RecordLength,
        "USN failed reads clear a previously populated output view");
}

void EmfHeader(Bytes& bytes, uint32_t header_bytes, uint32_t records) {
    Dword(bytes, 0, EMR_HEADER);
    Dword(bytes, 4, header_bytes);
    Dword(bytes, 16, 100); Dword(bytes, 20, 100);
    Dword(bytes, 32, 100); Dword(bytes, 36, 100);
    Dword(bytes, 40, ENHMETA_SIGNATURE);
    Dword(bytes, 44, 0x10000);
    Dword(bytes, 48, static_cast<uint32_t>(bytes.size()));
    Dword(bytes, 52, records);
    Word(bytes, 56, 1);
    Dword(bytes, 72, 100); Dword(bytes, 76, 100);
    Dword(bytes, 80, 1); Dword(bytes, 84, 1);
}

void EmfEof(Bytes& bytes, size_t offset) {
    Dword(bytes, offset, EMR_EOF);
    Dword(bytes, offset + 4, 20);
    Dword(bytes, offset + 16, 20);
}

Bytes Emf(size_t records = 2) {
    Bytes bytes(88 + (records - 2) * 8 + 20, 0);
    EmfHeader(bytes, 88, static_cast<uint32_t>(records));
    for (size_t offset = 88; offset + 20 < bytes.size(); offset += 8) {
        Dword(bytes, offset, EMR_SAVEDC);
        Dword(bytes, offset + 4, 8);
    }
    EmfEof(bytes, bytes.size() - 20);
    return bytes;
}

Bytes CommentEmf(size_t total_bytes) {
    Bytes bytes(total_bytes, 0);
    EmfHeader(bytes, 88, 3);
    const auto comment_bytes = static_cast<uint32_t>(total_bytes - 108);
    Dword(bytes, 88, EMR_GDICOMMENT);
    Dword(bytes, 92, comment_bytes);
    Dword(bytes, 96, comment_bytes - 12);
    EmfEof(bytes, bytes.size() - 20);
    return bytes;
}

Bytes Wmf(size_t records = 1) {
    Bytes bytes(18 + records * 6, 0);
    Word(bytes, 0, 1); Word(bytes, 2, 9); Word(bytes, 4, 0x300);
    Dword(bytes, 6, static_cast<uint32_t>(bytes.size() / 2));
    Dword(bytes, 12, 3);
    for (size_t offset = 18; offset < bytes.size(); offset += 6) {
        Dword(bytes, offset, 3);
        Word(bytes, offset + 4, static_cast<uint16_t>(offset + 6 == bytes.size() ? 0 : META_SAVEDC));
    }
    return bytes;
}

Bytes PlaceableWmf() {
    const auto standard = Wmf();
    Bytes bytes(22, 0);
    bytes.insert(bytes.end(), standard.begin(), standard.end());
    Dword(bytes, 0, 0x9ac6cdd7);
    Word(bytes, 10, 100); Word(bytes, 12, 100); Word(bytes, 14, 1440);
    uint16_t checksum = 0;
    for (size_t offset = 0; offset < 20; offset += 2) {
        uint16_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        checksum ^= value;
    }
    Word(bytes, 20, checksum);
    return bytes;
}

bool ValidMeta(const Bytes& bytes) {
    return pulse::preview::ValidateMetaFileInput(bytes.data(), bytes.size());
}

template<class Mutation>
void RejectMeta(const Bytes& original, const char* label, Mutation mutate) {
    auto bytes = original;
    mutate(bytes);
    Check(!ValidMeta(bytes), label);
}

void TestEmf() {
    using namespace pulse::preview;
    const auto bytes = Emf();
    Check(ValidMeta(bytes), "EMF accepts a complete binary header and EOF fixture");
    Check(ValidMeta(Emf(3)), "EMF accepts an intervening native record");
    bool truncated = true;
    for (size_t size = 0; size < bytes.size(); ++size)
        truncated = !ValidateMetaFileInput(bytes.data(), size) && truncated;
    Check(truncated, "EMF rejects every proper prefix of a complete fixture");
    Check(!ValidateMetaFileInput(nullptr, bytes.size()), "metafile nonempty input rejects a null pointer");
    Check(!ValidateMetaFileInput(nullptr, 0), "metafile empty input is rejected");
    RejectMeta(bytes, "EMF rejects an incorrect signature", [](Bytes& b) { Dword(b, 40, 0); });
    RejectMeta(bytes, "EMF rejects a declared file size beyond captured input", [](Bytes& b) { Dword(b, 48, MAXDWORD); });
    RejectMeta(bytes, "EMF rejects a declared file size shorter than captured input", [](Bytes& b) { Dword(b, 48, 104); });
    RejectMeta(bytes, "EMF rejects zero-sized header records", [](Bytes& b) { Dword(b, 4, 0); });
    RejectMeta(bytes, "EMF rejects headers shorter than the fixed fields", [](Bytes& b) { Dword(b, 4, 84); });
    RejectMeta(bytes, "EMF rejects headers extending beyond input", [](Bytes& b) { Dword(b, 4, MAXDWORD); });
    RejectMeta(bytes, "EMF rejects header record misalignment", [](Bytes& b) { Dword(b, 4, 90); });
    RejectMeta(bytes, "EMF rejects declared and actual record count mismatch", [](Bytes& b) { Dword(b, 52, 1); });
    RejectMeta(bytes, "EMF rejects declared record counts over budget", [](Bytes& b) {
        Dword(b, 52, static_cast<uint32_t>(kMaxMetaFileRecords + 1));
    });
    RejectMeta(bytes, "EMF zero-sized records cannot stall iteration", [](Bytes& b) { Dword(b, 92, 0); });
    RejectMeta(bytes, "EMF rejects records smaller than their common header", [](Bytes& b) { Dword(b, 92, 4); });
    RejectMeta(bytes, "EMF rejects record size misalignment", [](Bytes& b) { Dword(b, 92, 18); });
    RejectMeta(bytes, "EMF rejects records extending beyond input", [](Bytes& b) { Dword(b, 92, MAXDWORD); });
    RejectMeta(bytes, "EMF rejects an undersized EOF record", [](Bytes& b) { Dword(b, 92, 8); });
    RejectMeta(bytes, "EMF requires an EOF record", [](Bytes& b) { Dword(b, 88, EMR_SAVEDC); });
    RejectMeta(bytes, "EMF EOF must end at the input boundary", [](Bytes& b) {
        b.resize(b.size() + 8, 0);
        Dword(b, 48, static_cast<uint32_t>(b.size()));
        Dword(b, 52, 3);
    });
    Bytes described(116, 0);
    EmfHeader(described, 96, 2);
    Dword(described, 60, 4); Dword(described, 64, 88);
    Word(described, 88, 'A');
    EmfEof(described, 96);
    Check(ValidMeta(described), "EMF accepts description characters contained in an extended header");
    RejectMeta(described, "EMF description cannot overlap fixed header fields", [](Bytes& b) { Dword(b, 64, 84); });
    RejectMeta(described, "EMF description offset requires UTF-16 alignment", [](Bytes& b) { Dword(b, 64, 89); });
    RejectMeta(described, "EMF description offset cannot exceed its header", [](Bytes& b) { Dword(b, 64, MAXDWORD - 1); });
    RejectMeta(described, "EMF description length cannot overflow its header", [](Bytes& b) { Dword(b, 60, MAXDWORD); });
    Check(ValidMeta(Emf(kMaxMetaFileRecords)), "EMF accepts the exact actual record budget");
    auto many = Emf(kMaxMetaFileRecords + 1);
    Dword(many, 52, 2);
    Check(!ValidMeta(many), "EMF enforces actual record budget despite a small declared count");
    auto budget = CommentEmf(kMaxMetaFileBytes);
    Check(ValidMeta(budget), "EMF accepts a structurally complete comment container at the byte budget");
    budget = CommentEmf(kMaxMetaFileBytes + 4);
    Check(!ValidMeta(budget), "EMF rejects an otherwise structured container over the byte budget");
}

void TestWmf() {
    using namespace pulse::preview;
    const auto bytes = Wmf();
    Check(ValidMeta(bytes), "WMF accepts a complete standard binary header and EOF fixture");
    Check(ValidMeta(Wmf(2)), "WMF accepts an intervening native record");
    auto alternate = bytes;
    Word(alternate, 0, 2); Word(alternate, 4, 0x100);
    Check(ValidMeta(alternate), "WMF accepts supported memory type and older header version");
    bool truncated = true;
    for (size_t size = 0; size < bytes.size(); ++size)
        truncated = !ValidateMetaFileInput(bytes.data(), size) && truncated;
    Check(truncated, "WMF rejects every proper prefix of a standard fixture");
    RejectMeta(bytes, "WMF rejects unsupported file types", [](Bytes& b) { Word(b, 0, 0); });
    RejectMeta(bytes, "WMF rejects unsupported versions", [](Bytes& b) { Word(b, 4, 0x200); });
    RejectMeta(bytes, "WMF rejects incorrect fixed header length", [](Bytes& b) { Word(b, 2, 8); });
    RejectMeta(bytes, "WMF rejects nonzero reserved parameter count", [](Bytes& b) { Word(b, 16, 1); });
    RejectMeta(bytes, "WMF rejects a declared file size beyond input", [](Bytes& b) { Dword(b, 6, MAXDWORD); });
    RejectMeta(bytes, "WMF rejects byte lengths containing an incomplete WORD", [](Bytes& b) { b.push_back(0); });
    RejectMeta(bytes, "WMF zero-sized records cannot stall iteration", [](Bytes& b) { Dword(b, 18, 0); });
    RejectMeta(bytes, "WMF rejects records smaller than their common header", [](Bytes& b) { Dword(b, 18, 2); });
    RejectMeta(bytes, "WMF record size must respect its header maximum", [](Bytes& b) { Dword(b, 12, 2); });
    RejectMeta(bytes, "WMF record size cannot overflow or exceed captured input", [](Bytes& b) {
        Dword(b, 12, MAXDWORD); Dword(b, 18, MAXDWORD);
    });
    RejectMeta(bytes, "WMF rejects a truncated record header", [](Bytes& b) {
        b.resize(22); Dword(b, 6, 11);
    });
    RejectMeta(bytes, "WMF requires an EOF record", [](Bytes& b) { Word(b, 22, META_SAVEDC); });
    RejectMeta(bytes, "WMF EOF must end at the input boundary", [](Bytes& b) {
        b.resize(30, 0); Dword(b, 6, 15); Dword(b, 24, 3);
    });
    RejectMeta(bytes, "WMF EOF cannot contain extra parameters", [](Bytes& b) {
        b.resize(26, 0); Dword(b, 6, 13); Dword(b, 12, 4); Dword(b, 18, 4);
    });
    const auto placeable = PlaceableWmf();
    Check(ValidMeta(placeable), "WMF accepts a placeable binary header with a correct checksum");
    truncated = true;
    for (size_t size = 0; size < placeable.size(); ++size)
        truncated = !ValidateMetaFileInput(placeable.data(), size) && truncated;
    Check(truncated, "WMF rejects every proper prefix of a placeable fixture");
    RejectMeta(placeable, "WMF rejects a corrupted placeable checksum", [](Bytes& b) { b[20] ^= 1; });
    RejectMeta(placeable, "WMF validates the standard header following a placeable header", [](Bytes& b) { Word(b, 22 + 4, 0x200); });
    Check(ValidMeta(Wmf(kMaxMetaFileRecords)), "WMF accepts the exact actual record budget");
    Check(!ValidMeta(Wmf(kMaxMetaFileRecords + 1)), "WMF rejects actual record counts over budget");
}
} // namespace

int main() {
    TestUsn();
    TestEmf();
    TestWmf();
    std::cout << checks - failures << " PASS, " << failures << " FAIL\n";
    return failures ? 1 : 0;
}
