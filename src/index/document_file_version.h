// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "document_protocol.h"

namespace pulse::index::document {
inline bool QueryFileVersion(HANDLE file, const BY_HANDLE_FILE_INFORMATION& info, FileVersion& version) noexcept {
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) return false;
    version = {info.dwVolumeSerialNumber, (uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow,
        (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
        (uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32) | info.ftLastWriteTime.dwLowDateTime,
        static_cast<uint64_t>(basic.ChangeTime.QuadPart)};
    return version.id && version.changed;
}
inline bool FallbackSourceMatches(const Request& request, const Begin& begin) noexcept {
    return !(request.flags & 2) || ((begin.flags & 1) && begin.version == request.expected_version &&
        begin.config_mode == request.expected_config && begin.config_mode == 0 && begin.engine == 2);
}
}
