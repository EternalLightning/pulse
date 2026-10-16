#pragma once
#include <cstdint>
#include <vector>

namespace pulse::index {

template <class Parent>
bool ValidParentForest(int32_t count, Parent parent, std::vector<int32_t>* parent_order = nullptr) {
    std::vector<uint8_t> state(static_cast<size_t>(count), 0);
    std::vector<int32_t> path;
    if (parent_order) {
        parent_order->clear();
        parent_order->reserve(static_cast<size_t>(count));
    }
    for (int32_t start = 0; start < count; ++start) {
        if (state[static_cast<size_t>(start)] == 2) continue;
        path.clear();
        int32_t current = start;
        while (current >= 0) {
            if (current >= count || state[static_cast<size_t>(current)] == 1) return false;
            if (state[static_cast<size_t>(current)] == 2) break;
            state[static_cast<size_t>(current)] = 1;
            path.push_back(current);
            current = parent(current);
        }
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            state[static_cast<size_t>(*it)] = 2;
            if (parent_order) parent_order->push_back(*it);
        }
    }
    return true;
}

// Brent's cycle detector bounds parent walks without allocating per USN record.
class ParentChainGuard {
public:
    explicit ParentChainGuard(int32_t count) : count_(count) {}
    bool Visit(int32_t node) {
        if (node < 0 || node >= count_ || node == checkpoint_) return false;
        if (distance_ == power_) {
            checkpoint_ = node;
            power_ *= 2;
            distance_ = 0;
        }
        ++distance_;
        return true;
    }
private:
    int32_t count_;
    int32_t checkpoint_ = -1;
    uint64_t power_ = 1;
    uint64_t distance_ = 1;
};

} // namespace pulse::index
