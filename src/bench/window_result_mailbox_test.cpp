// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#include "../app/window_result_mailbox.h"
#include <cstdio>
#include <future>
#include <memory>
#include <thread>

namespace {
constexpr UINT kMessage = WM_APP + 46;
using Mailbox = pulse::app::WindowResultMailbox<std::shared_ptr<int>>;
int failures = 0;
void Check(bool pass, const char* name) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}
MSG Next(HWND hwnd) {
    MSG msg{};
    PeekMessageW(&msg, hwnd, kMessage, kMessage, PM_REMOVE);
    return msg;
}
}

int main() {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"mailbox fixture", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 1;
    {
        Mailbox box(hwnd, kMessage);
        auto payload = std::make_shared<int>(7);
        std::weak_ptr<int> lifetime = payload;
        Check(box.Publish(std::move(payload)), "completion before close posts notification");
        MSG msg = Next(hwnd);
        auto results = box.Take(msg.wParam);
        Check(msg.message == kMessage && msg.lParam == 0 && results.size() == 1 &&
            *results.front() == 7, "notification owns no heap payload; result consumed once");
        Check(box.Take(msg.wParam).empty(), "duplicate notification cannot consume twice");
        results.clear();
        Check(lifetime.expired(), "consumed result is released");
        box.Close();
    }
    {
        auto old_box = std::make_shared<Mailbox>(hwnd, kMessage);
        auto payload = std::make_shared<int>(9);
        std::weak_ptr<int> lifetime = payload;
        old_box->Publish(std::move(payload));
        old_box->Close();
        Check(lifetime.expired(), "close releases queued result before dispatch");
        const MSG old_message = Next(hwnd);
        // Deterministically model reuse of the identical numeric HWND.
        Mailbox replacement(hwnd, kMessage);
        replacement.Publish(std::make_shared<int>(11));
        Check(replacement.Take(old_message.wParam).empty(), "old window notification rejected by new session");
        auto current = replacement.Take(Next(hwnd).wParam);
        Check(current.size() == 1 && *current.front() == 11, "stale notification preserves new session result");
        Check(!old_box->Publish(std::make_shared<int>(12)), "old producer cannot post to reused window");
        replacement.Close();
    }
    {
        auto box = std::make_shared<Mailbox>(hwnd, kMessage);
        std::promise<void> release;
        auto gate = release.get_future();
        bool posted = true;
        std::thread worker([box, gate = std::move(gate), &posted]() mutable {
            gate.wait();
            posted = box->Publish(std::make_shared<int>(13));
        });
        box->Close();
        release.set_value();
        worker.join();
        Check(!posted && Next(hwnd).message == 0, "in-flight worker completing after close posts nothing");
    }
    bool race_ok = true;
    for (int i = 0; i < 200; ++i) {
        auto box = std::make_shared<Mailbox>(hwnd, kMessage);
        auto payload = std::make_shared<int>(i);
        std::weak_ptr<int> lifetime = payload;
        std::thread worker([box, payload = std::move(payload)]() mutable {
            box->Publish(std::move(payload));
        });
        box->Close();
        worker.join();
        race_ok = race_ok && lifetime.expired();
        const MSG msg = Next(hwnd);
        race_ok = race_ok && box->Take(msg.wParam).empty();
    }
    Check(race_ok, "200 concurrent completion/close races release all payloads");
    DestroyWindow(hwnd);
    {
        Mailbox failed(hwnd, kMessage);
        auto payload = std::make_shared<int>(1);
        std::weak_ptr<int> lifetime = payload;
        Check(!failed.Publish(std::move(payload)) && lifetime.expired(), "failed post releases result");
    }
    return failures ? 1 : 0;
}
