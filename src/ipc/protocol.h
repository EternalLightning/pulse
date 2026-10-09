// protocol.h — Pulse UI process <-> Pulse.Shell.exe wire protocol (named pipe).
//
// Transport: named pipe `\\.\pipe\pulse_shell_<ui-pid>`, byte mode, blocking I/O.
// The UI process (client) creates the pipe name from its own PID and passes the
// same PID to Pulse.Shell.exe as argv[1]; the shell host is the pipe server.
//
// Framing: fixed 16-byte header + payload.
//   struct MsgHeader { u32 magic; u32 type; u32 request_id; u32 payload_size; }
// Payload is a packed sequence of fields (little-endian, no alignment):
//   string  := u32 wchar_count (excluding terminator) + wchar data
//   u32     := 4 bytes
//
// Message flow:
//   client -> host : REQ_DELETE_RECYCLE / REQ_REALDELETE /
//                    REQ_RENAME / REQ_NEW_FOLDER / REQ_NEW_FILE /
//                    REQ_CANCEL / REQ_PING / REQ_SHUTDOWN / REQ_RESTORE_RECYCLE /
//                    REQ_CTX_QUERY / REQ_CTX_INVOKE / REQ_CTX_CLOSE
//   host -> client : RSP_PROGRESS (0..N) then exactly one RSP_DONE per request,
//                    RSP_PONG for REQ_PING, RSP_CTX_ITEMS for REQ_CTX_QUERY.
//
// Context-menu sessions (Explorer verbs): REQ_CTX_QUERY builds the third-party
// IContextMenu on a dedicated STA thread inside the host; its request id is the
// session id. The IContextMenu/HMENU stay alive on that thread until
// REQ_CTX_INVOKE (host closes the session after invoking, answering RSP_DONE)
// or REQ_CTX_CLOSE (no response). Sessions self-expire after 120s.
//
// Cancellation: REQ_CANCEL carries the target request id in the header's
// request_id field; the host sets an atomic flag that the progress sink checks
// in every callback, aborting the running IFileOperation.
//
// Payload layouts (all strings UTF-16, wchar_count first):
//   REQ_DELETE_RECYCLE /
//   REQ_REALDELETE /
//   REQ_RESTORE_RECYCLE  : paths(array) + exact recycle payloads(array, optional)
//   REQ_RENAME               : path(string) + new_name(string)  (name only, not a path)
//   REQ_NEW_FOLDER /
//   REQ_NEW_FILE             : path(string)  (full path of the item to create)
//   REQ_CANCEL / REQ_PING /
//   REQ_SHUTDOWN             : empty
//   REQ_CTX_QUERY            : owner_hwnd(u32) + flags(u32, CTXF_*) +
//                              paths(string array) + disabled_clsids(string array)
//   REQ_CTX_INVOKE           : session_id(u32) + item_id(u32) +
//                              verb(string) + text(string)
//   REQ_CTX_CLOSE            : session_id(u32)
//   RSP_PROGRESS             : percent(f32) + current_item(string) +
//                              items_done(u32) + total_items(u32)
//   RSP_DONE                 : result_hresult(u32) + cancelled(u32) + error_text(string)
//     authorized deletion    : above + deleted_paths(array) + recycled_paths(array) +
//                              recycle_destinations(array) + failed_path(string)
//   RSP_PONG                 : empty
//   RSP_CTX_ITEMS            : session_id(u32) + flags(u32, CTX_ITEMS_*) +
//                              count(u32) + per item:
//                              item_id(u32) + flags(u32, CTX_ITEM_*) +
//                              verb(string) + text(string) +
//                              clsid(string) + handler(string) +
//                              slow_clsids(string array, optional; handlers that
//                              took >= 1000ms, final payload only)
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace pulse::ipc {

constexpr uint32_t kMagic = 0x53504C50; // 'PLPS'
constexpr size_t kMaxPayload = 16 * 1024 * 1024;
constexpr HRESULT kMalformedDeleteResult = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0201);

enum MsgType : uint32_t {
    REQ_DELETE_RECYCLE = 3,
    REQ_REALDELETE = 4,
    REQ_RENAME = 5,
    REQ_CANCEL = 6,
    REQ_PING = 7,
    REQ_SHUTDOWN = 8,
    REQ_NEW_FOLDER = 9,
    REQ_NEW_FILE = 10,
    REQ_RESTORE_RECYCLE = 11,
    REQ_CTX_QUERY = 12,
    REQ_CTX_INVOKE = 13,
    REQ_CTX_CLOSE = 14,
    // Exact permanent roots; legacy deletion frames are fail-closed.
    // Payload: host creation FILETIME(u64), one-shot token(u64), paths(array).
    REQ_AUTHORIZED_DELETE = 15,
    // Same plan fields followed by owner HWND(u64); Shell owns recycle warnings.
    REQ_AUTHORIZED_RECYCLE = 16,
    RSP_PROGRESS = 100,
    RSP_DONE = 101,
    RSP_PONG = 102,
    RSP_CTX_ITEMS = 103,
};

// REQ_CTX_QUERY flags.
enum CtxQueryFlags : uint32_t {
    CTXF_EXTENDED = 1,    // Shift held: include extended verbs
    CTXF_BACKGROUND = 2,  // paths[0] is a folder; build its background menu
};

// RSP_CTX_ITEMS per-item flags. Software-owned submenus keep one level of
// hierarchy: a HAS_CHILDREN row is followed by its CHILD rows in order
// (children never nest further; deeper levels are cut on the host).
enum CtxItemFlags : uint32_t {
    CTX_ITEM_ENABLED = 1,
    CTX_ITEM_SEPARATOR_AFTER = 2,
    CTX_ITEM_HAS_CHILDREN = 4,  // submenu header; item_id is not invokable
    CTX_ITEM_CHILD = 8,         // belongs to the nearest preceding header
};

// RSP_CTX_ITEMS message flags (after session_id).
enum CtxItemsMsgFlags : uint32_t {
    CTX_ITEMS_PARTIAL = 1,  // more handlers still running; keep the session pending
};

struct MsgHeader {
    uint32_t magic = kMagic;
    uint32_t type = 0;
    uint32_t request_id = 0;
    uint32_t payload_size = 0;
};
static_assert(sizeof(MsgHeader) == 16);

// ---------------------------------------------------------------------------
// Payload writer/reader helpers (header-only, shared by both processes).
// ---------------------------------------------------------------------------
class PayloadWriter {
public:
    void PutU32(uint32_t v) {
        size_t off = buf_.size();
        buf_.resize(off + 4);
        std::memcpy(buf_.data() + off, &v, 4);
    }
    void PutU64(uint64_t v) {
        size_t off = buf_.size();
        buf_.resize(off + 8);
        std::memcpy(buf_.data() + off, &v, 8);
    }
    void PutF32(float v) {
        size_t off = buf_.size();
        buf_.resize(off + 4);
        std::memcpy(buf_.data() + off, &v, 4);
    }
    void PutString(const std::wstring& s) {
        PutU32((uint32_t)s.size());
        size_t off = buf_.size();
        buf_.resize(off + s.size() * sizeof(wchar_t));
        if (!s.empty()) std::memcpy(buf_.data() + off, s.data(), s.size() * sizeof(wchar_t));
    }
    void PutStringArray(const std::vector<std::wstring>& v) {
        PutU32((uint32_t)v.size());
        for (const auto& s : v) PutString(s);
    }
    const std::vector<uint8_t>& data() const { return buf_; }
private:
    std::vector<uint8_t> buf_;
};

class PayloadReader {
public:
    PayloadReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool GetU32(uint32_t& v) {
        if (off_ + 4 > size_) return false;
        std::memcpy(&v, data_ + off_, 4);
        off_ += 4;
        return true;
    }
    bool GetU64(uint64_t& v) {
        if (off_ + 8 > size_) return false;
        std::memcpy(&v, data_ + off_, 8);
        off_ += 8;
        return true;
    }
    bool GetF32(float& v) {
        if (off_ + 4 > size_) return false;
        std::memcpy(&v, data_ + off_, 4);
        off_ += 4;
        return true;
    }
    bool GetString(std::wstring& s) {
        uint32_t n = 0;
        if (!GetU32(n)) return false;
        const uint64_t bytes = static_cast<uint64_t>(n) * sizeof(wchar_t);
        if (bytes > remaining() || bytes > kMaxPayload - allocation_bytes_) return false;
        try {
            s.resize(n);
            if (n) std::memcpy(s.data(), data_ + off_, static_cast<size_t>(bytes));
        } catch (const std::bad_alloc&) {
            s.clear();
            return false;
        } catch (const std::length_error&) {
            s.clear();
            return false;
        }
        off_ += static_cast<size_t>(bytes);
        allocation_bytes_ += static_cast<size_t>(bytes);
        return true;
    }
    bool GetStringArray(std::vector<std::wstring>& v) {
        uint32_t n = 0;
        v.clear();
        if (!GetU32(n)) return false;
        constexpr uint32_t kMaxArrayItems = 65536;
        if (n > kMaxArrayItems || n > remaining() / sizeof(uint32_t)) return false;
        size_t scan = off_;
        uint64_t allocation = static_cast<uint64_t>(n) * sizeof(std::wstring);
        // Validate the complete array before allocating, including empty-string overhead.
        for (uint32_t i = 0; i < n; ++i) {
            if (size_ - scan < sizeof(uint32_t)) return false;
            uint32_t chars = 0;
            std::memcpy(&chars, data_ + scan, sizeof(chars));
            scan += sizeof(chars);
            const uint64_t bytes = static_cast<uint64_t>(chars) * sizeof(wchar_t);
            if (bytes > size_ - scan) return false;
            allocation += bytes;
            if (allocation > kMaxPayload - allocation_bytes_) return false;
            scan += static_cast<size_t>(bytes);
        }
        if (allocation > kMaxPayload - allocation_bytes_) return false;
        try {
            std::vector<std::wstring> parsed;
            parsed.reserve(n);
            allocation_bytes_ += static_cast<size_t>(n) * sizeof(std::wstring);
            for (uint32_t i = 0; i < n; ++i) {
                std::wstring s;
                if (!GetString(s)) return false;
                parsed.push_back(std::move(s));
            }
            v = std::move(parsed);
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
        return true;
    }
    bool TryStringArray(std::vector<std::wstring>& v) {
        if (off_ >= size_) {
            v.clear();
            return true;
        }
        return GetStringArray(v);
    }
    size_t remaining() const { return off_ < size_ ? size_ - off_ : 0; }
private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t off_ = 0;
    size_t allocation_bytes_ = 0;
};

struct DeleteResultPayload {
    uint32_t hr = 0;
    bool cancelled = false;
    std::wstring error;
    std::vector<std::wstring> deleted_paths;
    std::vector<std::wstring> recycled_paths;
    std::vector<std::wstring> recycle_destinations;
    std::wstring failed_path;
};

inline bool ReadDeleteResultPayload(PayloadReader& reader, DeleteResultPayload& payload) {
    payload = {};
    DeleteResultPayload parsed;
    uint32_t cancelled = 0;
    if (!reader.GetU32(parsed.hr) || !reader.GetU32(cancelled) || cancelled > 1 ||
        !reader.GetString(parsed.error) || !reader.GetStringArray(parsed.deleted_paths) ||
        !reader.GetStringArray(parsed.recycled_paths) || !reader.GetStringArray(parsed.recycle_destinations) ||
        !reader.GetString(parsed.failed_path) || reader.remaining() != 0 ||
        parsed.recycled_paths.size() != parsed.recycle_destinations.size()) return false;
    const auto valid_path = [](const std::wstring& path) { return path.find(L'\0') == std::wstring::npos; };
    if (!valid_path(parsed.failed_path)) return false;
    for (const auto& paths : {&parsed.deleted_paths, &parsed.recycled_paths, &parsed.recycle_destinations})
        for (const auto& path : *paths) if (!valid_path(path)) return false;
    parsed.cancelled = cancelled != 0;
    payload = std::move(parsed);
    return true;
}

struct ContextInvokePayload {
    uint32_t session = 0;
    uint32_t item = 0;
    std::wstring verb;
    std::wstring text;
};

inline bool ReadContextInvokePayload(PayloadReader& reader, ContextInvokePayload& payload) {
    payload = {};
    ContextInvokePayload parsed;
    if (!reader.GetU32(parsed.session) || !reader.GetU32(parsed.item) ||
        !reader.GetString(parsed.verb) || !reader.GetString(parsed.text) || reader.remaining() != 0 ||
        parsed.verb.find(L'\0') != std::wstring::npos || parsed.text.find(L'\0') != std::wstring::npos)
        return false;
    payload = std::move(parsed);
    return true;
}

inline std::wstring PipeNameFor(uint32_t ui_pid) {
    return L"\\\\.\\pipe\\pulse_shell_" + std::to_wstring(ui_pid);
}

// ---------------------------------------------------------------------------
// Exact byte-mode pipe I/O. The mode must match how the handle was opened.
// Concurrent reads/writes on one handle require FILE_FLAG_OVERLAPPED; anonymous
// pipes and the preview transport retain their synchronous mode.
// ---------------------------------------------------------------------------
struct PipeIoOptions {
    bool overlapped = true;
    // Timeout for each pending chunk, NOT a whole-frame deadline. Callers with
    // request-level budgets must enforce those separately (e.g. cancel event).
    DWORD pending_timeout_ms = INFINITE;
    HANDLE cancel_event = nullptr;
    DWORD chunk_size = MAXDWORD;
    HANDLE progress_event = nullptr; // Signalled after every successful chunk.
};

namespace detail {
template<bool Writing, typename Byte>
inline bool PipeTransfer(HANDLE pipe, Byte* data, DWORD size, const PipeIoOptions& options) {
    if (!size) return true;
    // A synchronous ReadFile/WriteFile cannot be interrupted by these options.
    if (!options.chunk_size || (!options.overlapped &&
        (options.cancel_event || options.pending_timeout_ms != INFINITE))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    HANDLE event = options.overlapped ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    if (options.overlapped && !event) return false;
    const auto finish = [&](DWORD error) {
        if (event) CloseHandle(event);
        if (error) SetLastError(error);
        return error == ERROR_SUCCESS;
    };
    while (size) {
        if (options.cancel_event) {
            const DWORD wait = WaitForSingleObject(options.cancel_event, 0);
            if (wait == WAIT_OBJECT_0) return finish(ERROR_CANCELLED);
            if (wait == WAIT_FAILED) return finish(GetLastError());
        }
        const DWORD chunk = size < options.chunk_size ? size : options.chunk_size;
        OVERLAPPED ov{};
        ov.hEvent = event;
        if (event) ResetEvent(event);
        DWORD done = 0;
        BOOL ok;
        if constexpr (Writing) ok = WriteFile(pipe, data, chunk, &done, event ? &ov : nullptr);
        else ok = ReadFile(pipe, data, chunk, &done, event ? &ov : nullptr);
        if (!ok && event && GetLastError() == ERROR_IO_PENDING) {
            HANDLE waits[]{event, options.cancel_event};
            const DWORD wait = WaitForMultipleObjects(options.cancel_event ? 2 : 1, waits,
                FALSE, options.pending_timeout_ms);
            if (wait != WAIT_OBJECT_0) {
                const DWORD error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                    wait == WAIT_OBJECT_0 + 1 ? ERROR_CANCELLED :
                    wait == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
                // Cancellation is only a request. Reap completion before the
                // OVERLAPPED, event or caller's buffer can leave this scope,
                // including when cancellation loses a race with completion.
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                return finish(error);
            }
            ok = GetOverlappedResult(pipe, &ov, &done, FALSE);
        }
        if (!ok) return finish(GetLastError());
        if (!done) return finish(ERROR_BROKEN_PIPE);
        if (options.progress_event) SetEvent(options.progress_event);
        data += done;
        size -= done;
    }
    return finish(ERROR_SUCCESS);
}
} // namespace detail

inline bool PipeRead(HANDLE pipe, void* out, DWORD size, const PipeIoOptions& options = {}) {
    return detail::PipeTransfer<false>(pipe, static_cast<uint8_t*>(out), size, options);
}

inline bool PipeWrite(HANDLE pipe, const void* data, DWORD size, const PipeIoOptions& options = {}) {
    return detail::PipeTransfer<true>(pipe, static_cast<const uint8_t*>(data), size, options);
}

} // namespace pulse::ipc
