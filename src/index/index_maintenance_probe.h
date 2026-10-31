// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace pulse::index {
// Worker-owned snapshots and cumulative counters, emitted by the existing
// bounded, once-per-minute timing log; never log individual filenames.
struct IndexMaintenanceProbe {
    struct Merge {
        uint64_t attempts = 0, deleted_triggers = 0, structural_triggers = 0;
        uint64_t delta_triggers = 0, quiet_triggers = 0, forced_triggers = 0;
        uint64_t tick = 0, since_previous_ms = 0, last_wall_us = 0;
        uint64_t nodes = 0, deleted = 0, base_pool_chars = 0, pool_chars = 0;
        uint64_t pool_waste_chars = 0, struct_changes = 0, delta_bytes = 0;
        bool last_committed = false;
    } merge;
    struct Pool {
        uint64_t attempts = 0, successes = 0, failures = 0, tick = 0;
        uint64_t waste_chars = 0, before_chars = 0, after_chars = 0;
        uint64_t before_capacity_bytes = 0, after_capacity_bytes = 0, reclaimed_chars = 0;
    } pool;
    void RecordMerge(const char* reason) noexcept {
        ++merge.attempts;
        if (std::strcmp(reason, "deleted_ratio") == 0) ++merge.deleted_triggers;
        else if (std::strcmp(reason, "structural_threshold") == 0) ++merge.structural_triggers;
        else if (std::strcmp(reason, "delta_threshold") == 0) ++merge.delta_triggers;
        else if (std::strcmp(reason, "quiet_changes") == 0) ++merge.quiet_triggers;
        else ++merge.forced_triggers;
    }
    std::string Json() const {
        std::string out = "{\"merge\":{";
        auto field = [&](const char* name, uint64_t value) {
            if (out.back() != '{') out += ',';
            out += '"'; out += name; out += "\":"; out += std::to_string(value);
        };
        field("attempts", merge.attempts);
        field("deleted_triggers", merge.deleted_triggers);
        field("structural_triggers", merge.structural_triggers);
        field("delta_triggers", merge.delta_triggers);
        field("quiet_triggers", merge.quiet_triggers);
        field("forced_triggers", merge.forced_triggers);
        field("tick", merge.tick);
        field("since_previous_ms", merge.since_previous_ms);
        field("last_wall_us", merge.last_wall_us);
        field("last_committed", merge.last_committed ? 1 : 0);
        field("nodes", merge.nodes);
        field("deleted", merge.deleted);
        field("base_pool_chars", merge.base_pool_chars);
        field("pool_chars", merge.pool_chars);
        field("pool_waste_chars", merge.pool_waste_chars);
        field("struct_changes", merge.struct_changes);
        field("delta_bytes", merge.delta_bytes);
        out += "},\"name_pool\":{";
        field("attempts", pool.attempts);
        field("successes", pool.successes);
        field("failures", pool.failures);
        field("tick", pool.tick);
        field("waste_chars", pool.waste_chars);
        field("before_chars", pool.before_chars);
        field("after_chars", pool.after_chars);
        field("before_capacity_bytes", pool.before_capacity_bytes);
        field("after_capacity_bytes", pool.after_capacity_bytes);
        field("reclaimed_chars", pool.reclaimed_chars);
        return out + "}}";
    }
};
}
