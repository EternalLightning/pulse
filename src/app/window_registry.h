#pragma once

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <vector>

namespace pulse::app {

// UI-thread-only registry for the top-level Pulse windows that share a core.
template <class Window>
class BasicWindowRegistry {
public:
    struct Entry { HWND hwnd = nullptr; Window* window = nullptr; };

    void Add(HWND hwnd, Window* window) {
        if (!hwnd || !window) return;
        Remove(hwnd);
        entries_.insert(entries_.begin(), {hwnd, window});
    }
    bool Remove(HWND hwnd) {
        const auto it = Find(hwnd);
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }
    void Touch(HWND hwnd) {
        const auto it = Find(hwnd);
        if (it != entries_.end() && it != entries_.begin())
            std::rotate(entries_.begin(), it, it + 1);
    }
    Window* Lookup(HWND hwnd) const {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
            [hwnd](const Entry& entry) { return entry.hwnd == hwnd; });
        return it == entries_.end() ? nullptr : it->window;
    }
    Window* MostRecent() const { return entries_.empty() ? nullptr : entries_.front().window; }
    HWND MostRecentHwnd() const { return entries_.empty() ? nullptr : entries_.front().hwnd; }
    Window* MostRecentExcept(HWND hwnd) const {
        for (const Entry& entry : entries_)
            if (entry.hwnd != hwnd) return entry.window;
        return nullptr;
    }
    std::size_t Count() const noexcept { return entries_.size(); }
    template <class F>
    void ForEach(F&& fn) const {
        for (const Entry& entry : entries_) fn(entry.hwnd, *entry.window);
    }
    bool IsLastWindow(HWND hwnd) const {
        return std::none_of(entries_.begin(), entries_.end(),
            [hwnd](const Entry& entry) { return entry.hwnd != hwnd; });
    }

private:
    auto Find(HWND hwnd) {
        return std::find_if(entries_.begin(), entries_.end(),
            [hwnd](const Entry& entry) { return entry.hwnd == hwnd; });
    }
    std::vector<Entry> entries_;
};

} // namespace pulse::app
