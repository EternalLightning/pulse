#pragma once

#include "../ipc/protocol.h"
#include "index_transport_security.h"

namespace pulse::index::agent {

inline constexpr uint32_t kMagic = 0x544E5050; // 'PPNT'
inline std::wstring PipeName() {
    const auto suffix = transport::Identity::Current().Suffix();
    return suffix.empty() ? std::wstring{} : L"\\\\.\\pipe\\PulseNetworkIndex." + suffix;
}
inline std::wstring SingletonName() {
    const auto suffix = transport::Identity::Current().Suffix();
    return suffix.empty() ? std::wstring{} : L"Local\\Pulse.Index.NetworkAgent." + suffix;
}
inline constexpr size_t kMaxPayload = 16 * 1024 * 1024;

enum Message : uint32_t {
    REQ_STATUS = 1,
    REQ_SEARCH = 2,
    REQ_ROOTS = 3,
    REQ_ADD_ROOT = 4,
    REQ_REMOVE_ROOT = 5,
    REQ_REBUILD = 6,
    REQ_CHANGE_LEASE = 7,
    REQ_CHANGE_SUMMARIES = 8,
    REQ_CHANGE_DETAILS = 9,
    RSP_STATUS = 101,
    RSP_SEARCH = 102,
    RSP_ROOTS = 103,
    RSP_RESULT = 104,
    RSP_CHANGE_LEASE = 107,
    RSP_CHANGE_SUMMARIES = 108,
    RSP_CHANGE_DETAILS = 109,
};

inline ipc::MsgHeader MakeHeader(uint32_t type, uint32_t id, uint32_t size) {
    ipc::MsgHeader header;
    header.magic = kMagic;
    header.type = type;
    header.request_id = id;
    header.payload_size = size;
    return header;
}

} // namespace pulse::index::agent
