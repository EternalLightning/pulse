#pragma once
#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>
namespace pulse::index {
struct IndexRequestLimits {
    static constexpr size_t sessions = 32;
    static constexpr size_t client_items = 32;
    static constexpr size_t client_bytes = 512 * 1024;
    static constexpr size_t global_items = 256;
    static constexpr size_t global_bytes = 4 * 1024 * 1024;
    static constexpr size_t response_items = 8;
    static constexpr size_t response_bytes = 64 * 1024 * 1024;
    static bool Admit(size_t sessions_used, bool existing, size_t items, size_t bytes,
                      size_t all_items, size_t all_bytes, size_t charge) {
        return (existing || sessions_used < sessions) && items < client_items &&
            all_items < global_items && charge <= client_bytes && bytes <= client_bytes - charge &&
            charge <= global_bytes && all_bytes <= global_bytes - charge;
    }
};
// Rotate the client's remaining work behind all other currently queued clients.
// FIFO within a client; a flood cannot monopolize the next dispatch.
template<class Task> Task PopFair(std::deque<Task>& queue) {
    Task task = std::move(queue.front()); queue.pop_front();
    const auto client = task.client;
    std::stable_partition(queue.begin(), queue.end(), [&](const auto& next) { return next.client != client; });
    return task;
}
} // namespace pulse::index
