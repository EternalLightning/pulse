#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::index {
// Source views must remain stable and must never reference the growing output.
// Names are exact UTF-16 code units, including case and embedded NULs.
class SnapshotNameInterner {
public:
    explicit SnapshotNameInterner(std::vector<wchar_t>& output) : output_(output) {}
    uint32_t Add(std::wstring_view name) {
        if (output_.size() > UINT32_MAX)
            throw std::length_error("snapshot name pool overflow");
        const auto [found, inserted] = names_.try_emplace(name, static_cast<uint32_t>(output_.size()));
        if (inserted) {
            if (name.size() > UINT32_MAX - output_.size()) throw std::length_error("snapshot name pool overflow");
            output_.insert(output_.end(), name.begin(), name.end());
        }
        return found->second;
    }
    size_t Count() const { return names_.size(); }
private:
    std::vector<wchar_t>& output_;
    std::unordered_map<std::wstring_view, uint32_t> names_;
};

// Worker-private full-build snapshot; validate/allocate before committing any
// offsets. IDs, order and all metadata remain unchanged. No Flatten is needed.
template<class Nodes>
DWORD InternSnapshotNamePool(std::vector<wchar_t>& pool, Nodes& nodes) noexcept {
    for (const auto& node : nodes)
        if (node.off > pool.size() || node.len > pool.size() - node.off) return ERROR_INVALID_DATA;
    try {
        std::vector<wchar_t> compact;
        std::vector<uint32_t> offsets;
        offsets.reserve(nodes.size());
        {
            SnapshotNameInterner names(compact);
            for (const auto& node : nodes) {
                const std::wstring_view name(node.len ? pool.data() + node.off : L"", node.len);
                offsets.push_back(names.Add(name));
            }
        }
        for (size_t i = 0; i < nodes.size(); ++i) nodes[i].off = offsets[i];
        pool.swap(compact);
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::length_error&) {
        return ERROR_ARITHMETIC_OVERFLOW;
    }
}

// Caller holds the Engine's exclusive lock. Retain every raw heap reference,
// including tombstones and nodes shadowed by replay patches: IDs can be reused.
// No mapped base names are visited. Validation/allocation precede any mutation.
template<class Nodes, class Patches>
DWORD CompactOverlayNamePool(std::vector<wchar_t>& pool, Nodes& nodes, Patches& patches) noexcept {
    auto visit = [&](auto&& fn) {
        for (auto& node : nodes) fn(node);
        for (auto& entry : patches) if (entry.second.has_name) fn(entry.second);
    };
    uint64_t chars = 0;
    bool valid = true;
    visit([&](const auto& ref) {
        const size_t off = ref.off;
        if (off > pool.size() || ref.len > pool.size() - off) valid = false;
        chars += ref.len;
    });
    if (!valid) return ERROR_INVALID_DATA;
    if (chars > UINT32_MAX) return ERROR_ARITHMETIC_OVERFLOW;
    try {
        // A successfully interned full build can become the live heap fallback
        // when disk publication fails. Preserve its valid shared offsets.
        if (chars > pool.size()) {
            struct Reference { uint32_t off; uint16_t len; };
            std::vector<Reference> references;
            visit([&](const auto& ref) { references.push_back({ref.off, ref.len}); });
            const DWORD error = InternSnapshotNamePool(pool, references);
            if (error != ERROR_SUCCESS) return error;
            size_t next = 0;
            visit([&](auto& ref) { ref.off = references[next++].off; });
            return ERROR_SUCCESS;
        }
        std::vector<wchar_t> compact(static_cast<size_t>(chars));
        size_t offset = 0;
        visit([&](auto& ref) {
            if (ref.len) std::memcpy(compact.data() + offset, pool.data() + ref.off,
                static_cast<size_t>(ref.len) * sizeof(wchar_t));
            ref.off = static_cast<uint32_t>(offset);
            offset += ref.len;
        });
        pool.swap(compact);
        return ERROR_SUCCESS;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::length_error&) {
        return ERROR_ARITHMETIC_OVERFLOW;
    }
}
}
