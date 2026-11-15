// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../ipc/file_operation_protocol.h"
#include "../ipc/delete_plan_protocol.h"
#include <algorithm>
#include <array>
#include <span>

namespace pulse::ops {
inline constexpr std::array<uint8_t, 4> kRecycleRequestMagic{'R', 'C', 0, 1};
inline bool DecodeElevatedRecycle(std::span<const uint8_t> bytes, ipc::ShellOperationRequest& request) {
    if (bytes.size() < kRecycleRequestMagic.size() ||
        !std::equal(kRecycleRequestMagic.begin(), kRecycleRequestMagic.end(), bytes.begin()) ||
        !ipc::ReadShellOperation(bytes.data() + 4, bytes.size() - 4, request) ||
        request.kind != ipc::ShellOperationKind::Recycle || request.elevation_authorized) return false;
    return std::all_of(request.items.begin(), request.items.end(), [](const auto& item) {
        const auto plain = path::StripExtendedPathPrefix(item.source);
        return ipc::IsLosslessDeleteShellPath(item.source) && plain.size() > 2 && plain[1] == L':' &&
            item.destination.empty() && item.expected_identity.Valid();
    });
}
inline std::vector<uint8_t> EncodeElevatedRecycle(const ipc::ShellOperationRequest& request) {
    auto payload = ipc::WriteShellOperation(request);
    payload.insert(payload.begin(), kRecycleRequestMagic.begin(), kRecycleRequestMagic.end());
    ipc::ShellOperationRequest decoded;
    return DecodeElevatedRecycle(payload, decoded) ? payload : std::vector<uint8_t>{};
}
}
