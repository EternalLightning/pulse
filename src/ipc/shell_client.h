// shell_client.h — Async client for Pulse.Shell.exe (UI-process side).
//
// Owns the child process lifetime: pipe connects to \\.\pipe\pulse_shell_<pid>;
// on send/receive failure the child is restarted (CreateProcess) and the
// read-only requests retry once; commands and mutations are never replayed.
// All callbacks fire on the client's reader thread — never block the UI.
#pragma once
#include "protocol.h"
#include "shell_launch_protocol.h"
#include "../common/file_identity.h"
#include "../common/shell_menu_item.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ops { class OpsManager; struct ShellCompletionTestProbe; }

namespace pulse::ipc {

using CtxMenuItem = pulse::ShellMenuItem;
struct ShellLaunchClientTestProbe;

class ShellClient {
public:
    struct Callbacks {
        std::function<void(uint32_t id, float percent, std::wstring item,
                           uint32_t items_done, uint32_t total_items)> progress;
        std::function<void(uint32_t id, uint32_t hr, bool cancelled, std::wstring error)> done;
        std::function<void(uint32_t id, uint32_t hr, bool cancelled, std::wstring error)> launch_done;
        std::function<void(uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                           std::vector<FileIdentity> produced_identities)> produced_done;
        std::function<void(uint32_t id, uint32_t hr, bool cancelled, std::wstring error,
                           std::vector<std::wstring> deleted_paths,
                           std::vector<std::wstring> recycled_paths,
                           std::vector<std::wstring> recycle_destinations,
                           std::wstring failed_path)> delete_done;
        // RSP_CTX_ITEMS for a REQ_CTX_QUERY; id is the query id (== session id).
        std::function<void(uint32_t id, std::vector<CtxMenuItem> items, bool partial,
                           std::vector<std::wstring> slow_clsids)> ctx_items;
    };

    static ShellClient& Instance();

    void Start(Callbacks cb);
    void Stop();

    uint32_t CreateFolder(const std::wstring& path);
    uint32_t CreateNewFile(const std::wstring& path);
    void Cancel(uint32_t id);
    void Abort(uint32_t id);
    bool Ping();
    using LaunchReply = std::function<void(HRESULT, bool, std::wstring)>;
    uint32_t Launch(const ShellLaunchRequest& request, LaunchReply reply = {});

    // Explorer context-menu session. Query returns the session id; items come
    // back through Callbacks::ctx_items. Invoke completes via Callbacks::done
    // (the host closes the session afterwards). Close is fire-and-forget for
    // sessions dismissed without invoking.
    uint32_t QueryContextMenu(const std::vector<std::wstring>& paths,
                              uint32_t owner_hwnd, bool background, bool extended,
                              const std::vector<std::wstring>& disabled_clsids = {},
                              const std::function<void(uint32_t)>& before_send = {});
    uint32_t InvokeContextMenu(uint32_t session_id, uint32_t item_id,
                               const std::wstring& verb = {},
                               const std::wstring& text = {},
                               const std::function<void(uint32_t)>& before_send = {});
    void CloseContextMenu(uint32_t session_id);

private:
    friend class pulse::ops::OpsManager;
    friend struct pulse::ops::ShellCompletionTestProbe;
    friend struct ShellLaunchClientTestProbe;
    uint32_t DeleteAuthorized(const std::vector<std::wstring>& paths, uint64_t token,
                              bool recycle = false, HWND owner = nullptr,
                              const std::vector<FileIdentity>& expected_identities = {});
    ShellClient() = default;
    ~ShellClient();

    struct Pending {
        uint32_t type = 0;
        uint32_t id = 0;
        std::vector<uint8_t> payload;
        bool retried = false;
    };

    struct Connection {
        explicit Connection(HANDLE handle);
        ~Connection();
        HANDLE pipe;
        HANDLE cancel;
    };
    std::shared_ptr<Connection> CurrentConnection();
    void SignalConnection();
    void RetireConnection();

    uint32_t Submit(uint32_t type, const std::vector<uint8_t>& payload,
                    const std::function<void(uint32_t)>& before_send = {});
    bool EnsureConnected();
    bool SpawnChild();
    void KillChild();
    bool SendFrame(uint32_t type, uint32_t id, const std::vector<uint8_t>& payload);
    void ReaderThread();
    void HandleDisconnect(const std::shared_ptr<Connection>& expected = {});
    void FireDone(uint32_t id, uint32_t hr, bool cancelled, const std::wstring& error);
    void FireRequestDone(uint32_t type, uint32_t id, uint32_t hr, bool cancelled, const std::wstring& error);
    // Caller holds send_mutex_; return a snapshot for dispatch after unlocking.
    std::wstring UnreachableMessage() const;

    Callbacks cb_;
    std::mutex send_mutex_;          // serializes complete frames + reconnect
    std::mutex connection_mutex_;    // short publication/cancellation lock; never holds I/O
    std::shared_ptr<Connection> connection_;
    std::mutex pending_mutex_;
    std::map<uint32_t, Pending> pending_;
    std::mutex launch_reply_mutex_;
    std::map<uint32_t, LaunchReply> launch_replies_;

    PROCESS_INFORMATION child_{};
    bool child_started_ = false;
    std::wstring last_error_;
    ULONGLONG last_spawn_try_ = 0;

    std::function<void()> before_frame_body_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> next_id_{1};
};

} // namespace pulse::ipc
