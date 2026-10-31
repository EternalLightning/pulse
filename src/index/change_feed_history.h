// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include "change_tracking.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>

namespace pulse::index {
// In-memory only. The engine mutex protects readers and the single writer.
// Keep the same event-count horizon without a heap object/string per event.
class ChangeFeedHistory {
public:
    static constexpr size_t kMaxRecords = 100000;
    explicit ChangeFeedHistory(size_t limit = kMaxRecords) : limit_(limit) {}
    size_t Size() const { return count_; }
    size_t PageCapacityBytes() const {
        size_t bytes = 0;
        for (const auto& page : pages_) bytes += page.data.capacity();
        return bytes;
    }
    bool Empty() const { return count_ == 0; }
    uint64_t FirstId() const { return Empty() ? 0 : HeaderAt(pages_.front(), pages_.front().begin).id; }
    void Clear() { pages_.clear(); count_ = 0; }

    void Append(const ChangeRecord& record, uint64_t id) {
        if (!limit_) return;
        assert(pages_.empty() || id > pages_.back().last_id);
        if (record.path.size() > UINT32_MAX || record.old_path.size() > UINT32_MAX)
            throw std::length_error("change feed path too long");
        const Header h{id, record.time, record.file_id, static_cast<uint32_t>(record.kind),
            static_cast<uint32_t>(record.source), record.is_dir ? 1u : 0u,
            static_cast<uint32_t>(record.path.size()), static_cast<uint32_t>(record.old_path.size())};
        const size_t bytes = Bytes(h);
        if (pages_.empty() || bytes > pages_.back().data.capacity() - pages_.back().data.size()) {
            Page page;
            page.data.reserve((std::max)(kPageBytes, bytes));
            Write(page, h, record);
            pages_.push_back(std::move(page));
        } else {
            Write(pages_.back(), h, record);
        }
        ++count_;
        if (count_ > limit_) {
            auto& page = pages_.front();
            page.begin += Bytes(HeaderAt(page, page.begin));
            --count_;
            if (page.begin == page.data.size()) pages_.pop_front();
        }
    }

    // Append at most limit events; skip whole pages before the requested cursor.
    void ReadAfter(uint64_t cursor, size_t limit, std::vector<ChangeRecord>& out) const {
        auto page = std::lower_bound(pages_.begin(), pages_.end(), cursor,
            [](const Page& p, uint64_t value) { return p.last_id <= value; });
        for (; page != pages_.end() && limit; ++page) {
            for (size_t offset = page->begin; offset < page->data.size() && limit;) {
                const auto h = HeaderAt(*page, offset);
                const auto* strings = page->data.data() + offset + sizeof(Header);
                offset += Bytes(h);
                if (h.id <= cursor) continue;
                ChangeRecord record;
                record.id = h.id; record.time = h.time; record.file_id = h.file_id;
                record.kind = static_cast<ChangeKind>(h.kind);
                record.source = static_cast<ChangeSource>(h.source); record.is_dir = h.is_dir != 0;
                record.path.resize(h.path_chars); record.old_path.resize(h.old_chars);
                std::memcpy(record.path.data(), strings, h.path_chars * sizeof(wchar_t));
                std::memcpy(record.old_path.data(), strings + h.path_chars * sizeof(wchar_t), h.old_chars * sizeof(wchar_t));
                out.push_back(std::move(record));
                --limit;
            }
        }
    }
private:
    struct Header {
        uint64_t id, time, file_id;
        uint32_t kind, source, is_dir, path_chars, old_chars;
    };
    struct Page {
        std::vector<uint8_t> data;
        size_t begin = 0;
        uint64_t last_id = 0;
    };
    static constexpr size_t kPageBytes = 64 * 1024;
    static size_t Bytes(const Header& h) {
        return sizeof(Header) + (size_t(h.path_chars) + h.old_chars) * sizeof(wchar_t);
    }
    static Header HeaderAt(const Page& page, size_t offset) {
        Header h;
        std::memcpy(&h, page.data.data() + offset, sizeof(h));
        return h;
    }
    static void Write(Page& page, const Header& h, const ChangeRecord& record) {
        const auto offset = page.data.size();
        page.data.resize(offset + Bytes(h));
        auto* dest = page.data.data() + offset;
        std::memcpy(dest, &h, sizeof(h));
        std::memcpy(dest + sizeof(h), record.path.data(), record.path.size() * sizeof(wchar_t));
        std::memcpy(dest + sizeof(h) + record.path.size() * sizeof(wchar_t),
            record.old_path.data(), record.old_path.size() * sizeof(wchar_t));
        page.last_id = h.id;
    }
    size_t limit_, count_ = 0;
    std::deque<Page> pages_;
};
}
