// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "file_identity.h"
#include "../common/localization.h"
#include <string>
#include <vector>

namespace pulse::ops {
inline bool ValidateProducedUndo(const std::vector<std::wstring>& paths,
                                 const std::vector<FileIdentity>& identities,
                                 std::wstring& error, std::wstring& failed_path) {
    if (paths.empty() || paths.size() != identities.size()) {
        error = l10n::Get(l10n::StringId::OperationUndoIdentityUnverified);
        return false;
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        FileIdentity current;
        if (!identities[i].Valid() || !pulse::ReadPathFileIdentity(paths[i], current) || current != identities[i]) {
            failed_path = paths[i];
            error = l10n::Format(l10n::StringId::OperationUndoResultChangedFormat, paths[i].c_str());
            return false;
        }
    }
    return true;
}

inline std::wstring ProducedIdentityText(const FileIdentity& identity) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring text(49, L'0');
    text[0] = identity.extended ? L'e' : L'l';
    for (size_t i = 0; i < 16; ++i)
        text[1 + i] = digits[(identity.value.VolumeSerialNumber >> ((15 - i) * 4)) & 15];
    for (size_t i = 0; i < 16; ++i) {
        text[17 + i * 2] = digits[identity.value.FileId.Identifier[i] >> 4];
        text[18 + i * 2] = digits[identity.value.FileId.Identifier[i] & 15];
    }
    return text;
}

inline bool ParseProducedIdentity(const std::wstring& text, FileIdentity& identity) {
    if (text.size() != 49 || (text[0] != L'e' && text[0] != L'l')) return false;
    const auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        return -1;
    };
    identity = {};
    identity.extended = text[0] == L'e';
    for (size_t i = 1; i < 17; ++i) {
        const int digit = nibble(text[i]);
        if (digit < 0) return false;
        identity.value.VolumeSerialNumber = (identity.value.VolumeSerialNumber << 4) | static_cast<uint64_t>(digit);
    }
    for (size_t i = 0; i < 16; ++i) {
        const int high = nibble(text[17 + i * 2]), low = nibble(text[18 + i * 2]);
        if (high < 0 || low < 0) return false;
        identity.value.FileId.Identifier[i] = static_cast<BYTE>((high << 4) | low);
    }
    return identity.Valid();
}
}
