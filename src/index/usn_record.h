#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace pulse::index {

struct UsnRecordView {
    USN_RECORD_V2 record{};
    std::wstring_view name;
};

inline bool ReadUsnRecord(const BYTE* data, size_t available, UsnRecordView& view) {
    view = {};
    if (!data || available < sizeof(USN_RECORD_COMMON_HEADER)) return false;
    USN_RECORD_COMMON_HEADER header{};
    std::memcpy(&header, data, sizeof(header));
    constexpr size_t fixed_bytes = offsetof(USN_RECORD_V2, FileName);
    if (header.MajorVersion != 2 || header.MinorVersion != 0 ||
        header.RecordLength < fixed_bytes || header.RecordLength > available ||
        header.RecordLength % 8 != 0) return false;
    std::memcpy(&view.record, data, fixed_bytes);
    const auto& record = view.record;
    if (record.FileNameOffset < fixed_bytes || record.FileNameOffset % sizeof(WCHAR) ||
        record.FileNameLength % sizeof(WCHAR) || record.FileNameOffset > record.RecordLength ||
        record.FileNameLength > record.RecordLength - record.FileNameOffset) return false;
    view.name = {reinterpret_cast<const wchar_t*>(data + record.FileNameOffset),
                 record.FileNameLength / sizeof(WCHAR)};
    return true;
}

inline bool ValidateUsnRecords(const BYTE* data, size_t bytes) {
    while (bytes) {
        UsnRecordView view;
        if (!ReadUsnRecord(data, bytes, view)) return false;
        data += view.record.RecordLength;
        bytes -= view.record.RecordLength;
    }
    return true;
}

} // namespace pulse::index
