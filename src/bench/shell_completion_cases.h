// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "../ops/ops_manager.h"
#include "../ipc/shell_client.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <utility>

namespace pulse::ops {

struct ShellCompletionTestProbe {
    using Completion = OpsManager::ShellCompletion;
    using Mailbox = std::shared_ptr<OpsManager::OpenMailbox>;

    static void StartCallbacks(OpsManager& manager) {
        // Acquire real callback owners without starting the reader/helper.
        // Final-owner release restores the idle transport through normal Stop.
        ipc::ShellClient::Instance().running_.store(true);
        manager.StartShellClient();
    }
    static ipc::ShellClient::Owner Owner(const OpsManager& manager) {
        return manager.ShellOwner();
    }
    static ipc::ShellClient::Owner OpenOwner(const OpsManager& manager) {
        return manager.open_mailbox_->shell_owner;
    }
    static bool SeparateOwners(const OpsManager& manager) {
        const auto operations = Owner(manager);
        const auto opens = OpenOwner(manager);
        return operations && opens && operations != opens &&
            operations->callbacks.done && operations->callbacks.progress && operations->callbacks.ctx_items &&
            !operations->callbacks.launch_done && opens->callbacks.launch_done &&
            !opens->callbacks.done && !opens->callbacks.progress && !opens->callbacks.ctx_items;
    }
    static Mailbox DetachOpenMailbox(OpsManager& manager) {
        std::lock_guard lock(manager.open_mutex_);
        return std::move(manager.open_mailbox_);
    }
    static void DispatchDone(const ipc::ShellClient::Owner& owner, uint32_t id) {
        auto& client = ipc::ShellClient::Instance();
        client.BindOwner(id, owner);
        client.FireDone(id, static_cast<uint32_t>(E_ACCESSDENIED), false, L"owner completion");
    }
    static void DispatchLaunchError(const ipc::ShellClient::Owner& owner, uint32_t id) {
        auto& client = ipc::ShellClient::Instance();
        client.BindOwner(id, owner);
        client.FireRequestDone(ipc::REQ_SHELL_LAUNCH, id, static_cast<uint32_t>(E_FAIL), false, L"owner launch error");
    }

    static void Done(OpsManager& manager, uint32_t id, Completion result) {
        manager.OnShellDone(id, std::move(result));
    }
    static bool Wait(OpsManager& manager, uint32_t id, Completion& result) {
        return manager.WaitShellDone(id, result);
    }
    static size_t Pending(OpsManager& manager) {
        std::lock_guard lock(manager.done_mutex_);
        return manager.done_results_.size();
    }
    static bool Registered(OpsManager& manager, uint32_t token, uint32_t id) {
        std::lock_guard lock(manager.menu_mutex_);
        const auto by_token = manager.menu_session_by_token_.find(token);
        const auto by_id = manager.menu_token_by_session_.find(id);
        return by_token != manager.menu_session_by_token_.end() && by_token->second == id &&
            by_id != manager.menu_token_by_session_.end() && by_id->second == token;
    }
    static void RegisterQuery(OpsManager& manager, uint32_t token, uint32_t id) {
        manager.RegisterShellMenuQuery(token, id);
    }
    static void RegisterInvoke(OpsManager& manager, uint32_t id) {
        manager.RegisterShellMenuInvoke(id);
    }
    static void PrepareWait(OpsManager& manager, uint32_t id) {
        manager.current_req_id_.store(id);
        manager.shell_cancel_requested_.store(true);
        manager.shell_activity_tick_.store(GetTickCount64());
        std::lock_guard lock(manager.mutex_);
        manager.status_.active = true;
        manager.status_.phase = OpPhase::Running;
    }
    static bool WaitStateCleared(OpsManager& manager) {
        return manager.current_req_id_.load() == 0 && !manager.shell_cancel_requested_.load();
    }
    static bool TransportIdle() {
        auto& client = ipc::ShellClient::Instance();
        std::lock_guard lock(client.send_mutex_);
        return !client.running_.load() && !client.CurrentConnection() &&
            !client.child_started_ && !client.reader_.joinable();
    }
    static void ExpireWait(OpsManager& manager) {
        manager.shell_activity_tick_.store(GetTickCount64() - 601000);
        std::lock_guard lock(manager.done_mutex_);
        manager.done_cv_.notify_all();
    }
    static uint32_t SynchronousQueryFailure(OpsManager& manager, uint32_t token,
                                           bool& registered_before_done, bool& no_helper) {
        ipc::ShellClient client;
        client.cb_.done = [&](uint32_t id, uint32_t hr, bool cancelled, std::wstring error) {
            registered_before_done = Registered(manager, token, id);
            Done(manager, id, Completion{hr, cancelled, std::move(error)});
        };
        client.running_.store(true);
        const auto id = client.QueryContextMenu({L"C:\\fixture\\menu.txt"}, 0, false, false, {},
            [&](uint32_t request_id) {
                RegisterQuery(manager, token, request_id);
                // Fail EnsureConnected after real before_send registration,
                // without starting a reader or attempting to spawn a helper.
                client.running_.store(false);
            });
        no_helper = !client.child_started_ && !client.CurrentConnection() && !client.reader_.joinable();
        return id;
    }
};

inline int RunShellCompletionRegressions() {
    using Probe = ShellCompletionTestProbe;
    using Completion = Probe::Completion;
    int failures = 0;
    const auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
        if (!value) ++failures;
    };
    const auto evidence = [](const wchar_t* name, uint32_t hr, bool cancelled) {
        const std::wstring root = std::wstring(L"C:\\fixture\\") + name;
        return Completion{hr, cancelled, root + L" error", root + L" failed",
            {root + L" deleted"}, {root + L" recycled"}, {root + L" recycle destination"}};
    };
    const auto same = [](const Completion& actual, const Completion& expected) {
        return actual.hr == expected.hr && actual.cancelled == expected.cancelled && actual.error == expected.error &&
            actual.failed_path == expected.failed_path && actual.deleted_paths == expected.deleted_paths &&
            actual.recycled_paths == expected.recycled_paths && actual.recycle_destinations == expected.recycle_destinations;
    };
    const bool idle = Probe::TransportIdle();
    check(idle, "completion fixture starts with no active global shell transport");
    if (!idle) return 1;
    {
        OpsManager first, second;
        Probe::StartCallbacks(first);
        Probe::StartCallbacks(second);
        check(Probe::SeparateOwners(first) && Probe::SeparateOwners(second),
              "launch callbacks have independent mailbox owners without manager callbacks");
        const auto retired_owner = Probe::Owner(first);
        const auto survivor_owner = Probe::Owner(second);
        const auto survivor_open_owner = Probe::OpenOwner(second);
        auto detached_mailbox = Probe::DetachOpenMailbox(first);
        ipc::ShellClient::Instance().Release(detached_mailbox->shell_owner);
        detached_mailbox.reset();
        size_t menu_callbacks = 0;
        first.SetShellMenuCallback([&](uint32_t token, auto items, bool partial, auto slow) {
            if (token == 91 && items.empty() && !partial && slow.empty()) ++menu_callbacks;
        });
        Probe::RegisterInvoke(first, 502);
        Probe::RegisterQuery(first, 91, 503);
        Probe::DispatchDone(retired_owner, 501);
        Probe::DispatchDone(retired_owner, 502);
        Probe::DispatchDone(retired_owner, 503);
        Completion actual;
        check(Probe::Owner(first) == retired_owner && Probe::Pending(first) == 1 &&
              first.TakeCtxInvokeDone() && menu_callbacks == 1 && !Probe::Registered(first, 91, 503) &&
              Probe::Wait(first, 501, actual) && actual.hr == static_cast<uint32_t>(E_ACCESSDENIED),
              "file and menu callbacks survive mailbox retirement and retain completion routing");
        first.Stop();
        Probe::DispatchDone(retired_owner, 504);
        Probe::DispatchDone(survivor_owner, 505);
        Probe::DispatchLaunchError(survivor_open_owner, 506);
        const auto errors = second.DrainShellLaunchErrors();
        check(Probe::Pending(first) == 0 && Probe::Pending(second) == 1 && errors.size() == 1 &&
              errors.front() == L"owner launch error" && Probe::Wait(second, 505, actual),
              "closing one manager suppresses late callbacks while another window retains both owners");
        second.Stop();
        Probe::DispatchLaunchError(survivor_open_owner, 507);
        check(second.DrainShellLaunchErrors().empty() && Probe::TransportIdle(),
              "final shutdown retires both callback owners without launching a helper");
    }
    {
        OpsManager manager;
        const auto first = evidence(L"first", static_cast<uint32_t>(E_ACCESSDENIED), false);
        const auto second = evidence(L"second", static_cast<uint32_t>(S_OK), true);
        Probe::Done(manager, 102, second);
        Probe::Done(manager, 101, first);
        Completion actual;
        check(Probe::Wait(manager, 101, actual) && same(actual, first),
              "interleaved completion retains first request HRESULT, paths and deletion evidence");
        check(Probe::Wait(manager, 102, actual) && same(actual, second) && Probe::Pending(manager) == 0,
              "interleaved completion retains second request evidence and cancellation");
    }
    {
        OpsManager manager;
        const auto first = evidence(L"terminal-first", static_cast<uint32_t>(E_FAIL), false);
        Probe::Done(manager, 201, first);
        Probe::Done(manager, 201, evidence(L"terminal-duplicate", static_cast<uint32_t>(S_OK), true));
        Completion actual;
        check(Probe::Wait(manager, 201, actual) && same(actual, first) && Probe::Pending(manager) == 0,
              "duplicate terminal preserves first completion and every evidence field");
    }
    {
        OpsManager manager;
        const auto file = evidence(L"file-operation", static_cast<uint32_t>(E_ACCESSDENIED), false);
        Probe::Done(manager, 301, file);
        Probe::RegisterInvoke(manager, 302);
        Probe::Done(manager, 302, evidence(L"menu-invoke", static_cast<uint32_t>(S_OK), false));
        check(manager.TakeCtxInvokeDone() && !manager.TakeCtxInvokeDone() && Probe::Pending(manager) == 1,
              "menu invoke completion stays out of file operation results");
        size_t callbacks = 0;
        manager.SetShellMenuCallback([&](uint32_t token, auto items, bool partial, auto slow) {
            if (token == 77 && items.empty() && !partial && slow.empty()) ++callbacks;
        });
        Probe::RegisterQuery(manager, 77, 303);
        Probe::Done(manager, 303, evidence(L"menu-query", static_cast<uint32_t>(E_FAIL), false));
        Completion actual;
        check(callbacks == 1 && Probe::Pending(manager) == 1 && !Probe::Registered(manager, 77, 303) &&
              Probe::Wait(manager, 301, actual) && same(actual, file),
              "menu query failure releases its session without overwriting file evidence");
    }
    {
        OpsManager manager;
        size_t callbacks = 0;
        manager.SetShellMenuCallback([&](uint32_t token, auto items, bool partial, auto slow) {
            if (token == 88 && items.empty() && !partial && slow.empty()) ++callbacks;
        });
        bool registered_before_done = false, no_helper = false;
        const auto id = Probe::SynchronousQueryFailure(manager, 88, registered_before_done, no_helper);
        check(id != 0 && registered_before_done && no_helper && callbacks == 1 &&
              !Probe::Registered(manager, 88, id) && Probe::Pending(manager) == 0,
              "real Submit registers query before synchronous failure without starting a helper");
    }
    {
        OpsManager manager;
        Probe::PrepareWait(manager, 400);
        auto waiter = std::async(std::launch::async, [&] {
            Completion result;
            const bool completed = Probe::Wait(manager, 400, result);
            return std::make_pair(completed, std::move(result));
        });
        bool finished = false;
        for (unsigned wake = 0; wake < 20 && !finished; ++wake) {
            Probe::ExpireWait(manager);
            finished = waiter.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
        }
        if (!finished) manager.BeginShutdown();
        const auto result = waiter.get();
        check(finished && result.first && result.second.hr == static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)) &&
              !result.second.cancelled && !result.second.error.empty() && result.second.deleted_paths.empty() &&
              result.second.recycled_paths.empty() && Probe::Pending(manager) == 0 && Probe::WaitStateCleared(manager),
              "expired waiter synthesizes timeout when Abort has no pending transport callback");
    }
    {
        OpsManager manager;
        Probe::PrepareWait(manager, 401);
        std::promise<void> entered;
        auto started = entered.get_future();
        auto waiter = std::async(std::launch::async, [&] {
            Completion result;
            entered.set_value();
            const bool completed = Probe::Wait(manager, 401, result);
            return std::make_pair(completed, std::move(result));
        });
        started.wait();
        check(waiter.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
              "missing completion keeps request waiter pending");
        manager.BeginShutdown();
        const bool awakened = waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!awakened) Probe::Done(manager, 401, Completion{static_cast<uint32_t>(E_ABORT), true, L"fixture cleanup"});
        const auto result = waiter.get();
        check(awakened && !result.first && Probe::WaitStateCleared(manager) &&
              manager.Status().phase == OpPhase::Cancelled && Probe::Pending(manager) == 0,
              "shutdown wakes blocked waiter and clears request state without helper or filesystem work");
    }
    std::printf("Shell completion regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}

} // namespace pulse::ops
