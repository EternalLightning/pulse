# SPDX-License-Identifier: Apache-2.0
"""Summarize frame, CPU-model, and memory logs or CSV without assuming a refresh rate."""

import argparse
import csv
import json
import math
from pathlib import Path
import sys


FRAME_REQUIRED = {
    "scenario", "repeat", "frame", "active", "interval_ms", "input_ms",
    "prepare_ms", "paint_ms", "submit_ms", "present_ms", "after_ms",
    "total_ms", "presented", "view_reused", "pixel_rows",
}
VM_REQUIRED = {"scenario", "repeat", "sample", "total_ms", "vm_ms", "vm_builds"}
GPU_REQUIRED = {"id", "scenario", "repeat", "frame", "measurement", "status", "gpu_timestamp_elapsed_ms",
                "begin_ticks", "end_ticks", "frequency_hz", "disjoint", "hresult"}
PRESENT_COLUMNS = (
    "sample_id", "kind", "render_id", "scenario", "repeat", "frame", "measurement", "cpu_presented", "sample_qpc",
    "last_present_hr", "last_present_id", "frame_hr", "frame_state", "frame_epoch", "present_id", "present_refresh_count",
    "sync_refresh_count", "sync_qpc", "sync_gpu_time", "media_qi_hr", "media_hr", "media_state", "media_epoch",
    "media_present_id", "media_present_refresh_count", "media_sync_refresh_count", "media_sync_qpc",
    "media_composition_mode", "media_approved_present_duration",
)
PRESENT_STATES = ("unsupported", "error", "disjoint", "zero", "incomplete", "baseline", "unchanged", "advanced", "reset")
PRESENT_VALID = {"baseline", "unchanged", "advanced", "reset"}
MEMORY_FIELDS = (
    "working_set_bytes", "private_bytes", "gpu_local_usage_bytes",
    "gpu_local_budget_bytes", "gpu_nonlocal_usage_bytes", "gpu_nonlocal_budget_bytes",
)
NOTES = [
    "Percentiles use nearest rank: sorted[ceil(p * count) - 1]. Warmup exclusion belongs to the fixture.",
    "interval_ms is the measured frame-start interval; it does not establish displayed frames or dropped frames.",
    "cpu_work_ms = total_ms - present_ms is CPU-side elapsed work, not process CPU utilization; input_ms is separate.",
    "present_ms is elapsed time inside Present, not GPU execution time. Timings may include waiting.",
    "paint_list_setup_ms and paint_list_rows_ms are nested within paint_pane_ms; do not add them to the top-level paint phases.",
    "paint_list_layout_ms, paint_list_slots_ms, paint_list_motion_ms and paint_list_columns_ms partition list setup; wait_ms is before input/Render and outside total_ms.",
    "VM_CSV reports isolated CPU model measurements; it has no frame, Present, GPU, or active-animation semantics.",
    "Memory first/last/peak use available samples (-1 is unavailable); budgets are capacities, not allocations.",
    "GPU timestamps span BeginDraw..EndDraw, exclude Present, and may include supply gaps or scheduling; invalid/pending/dropped samples are not zero-duration GPU work.",
    "Presentation statistics are asynchronous DXGI snapshots: advancing IDs are observed presentation progress; polling intervals and refresh-count gaps are not display intervals or dropped frames.",
]


def distribution(values):
    ordered = sorted(values)
    return {
        "count": len(ordered),
        **{f"p{percent}": ordered[(percent * len(ordered) + 99) // 100 - 1]
           for percent in (50, 95, 99)},
        "max": ordered[-1],
    }


def read_records(path):
    """Accept a mixed tagged log or extracted CSV; reject partial rows."""
    headers = {}
    records = {"FRAME_CSV": [], "MEMORY_CSV": [], "VM_CSV": [], "GPU_CSV": [], "PRESENT_CSV": []}
    plain_kind = None
    with Path(path).open(encoding="utf-8-sig") as source:
        for line_number, line in enumerate(source, 1):
            line = line.strip()
            if not line:
                continue
            tagged = line.startswith(("FRAME_CSV,", "MEMORY_CSV,", "VM_CSV,", "GPU_CSV,", "PRESENT_CSV,"))
            if tagged:
                kind, line = line.split(",", 1)
            elif line.startswith("scenario,repeat,sample,"):
                kind = plain_kind = "VM_CSV"
            elif line.startswith("scenario,"):
                kind = plain_kind = "FRAME_CSV"
            elif line.startswith("time_ms,"):
                kind = plain_kind = "MEMORY_CSV"
            elif line.startswith("id,scenario,"):
                kind = plain_kind = "GPU_CSV"
            elif line.startswith("sample_id,kind,"):
                kind = plain_kind = "PRESENT_CSV"
            elif plain_kind:
                kind = plain_kind
            else:
                continue
            try:
                cells = next(csv.reader([line], strict=True))
            except csv.Error as error:
                raise ValueError(f"{path}:{line_number}: invalid CSV: {error}") from error
            required = {"FRAME_CSV": FRAME_REQUIRED, "VM_CSV": VM_REQUIRED, "GPU_CSV": GPU_REQUIRED,
                        "PRESENT_CSV": set(PRESENT_COLUMNS),
                        "MEMORY_CSV": {"time_ms", *MEMORY_FIELDS}}[kind]
            if cells[0] in ("scenario", "time_ms", "id", "sample_id"):
                if len(cells) != len(set(cells)) or not required.issubset(cells):
                    raise ValueError(f"{path}:{line_number}: invalid {kind} header; required: {sorted(required)}")
                if kind in headers and headers[kind] != cells:
                    raise ValueError(f"{path}:{line_number}: {kind} header changed within one input")
                headers[kind] = cells
                continue
            if kind not in headers or len(cells) != len(headers[kind]):
                raise ValueError(f"{path}:{line_number}: missing header or incomplete {kind} row")
            records[kind].append((line_number, dict(zip(headers[kind], cells))))
    return records


def number(value, context, *, integer=False, unavailable=False):
    try:
        result = int(value) if integer else float(value)
    except ValueError as error:
        raise ValueError(f"{context}: invalid number {value!r}") from error
    if not math.isfinite(result) or (result < 0 and not (unavailable and result == -1)):
        raise ValueError(f"{context}: expected finite nonnegative value" + (" or -1" if unavailable else ""))
    return result


def analyze_gpu(records, path):
    rows, seen = [], set()
    for line_number, row in records:
        context = f"{path}:{line_number} GPU"
        identifier = number(row["id"], context, integer=True)
        if identifier in seen:
            raise ValueError(f"{context}: duplicate query id")
        seen.add(identifier)
        sample = {field: number(row[field], f"{context} {field}", integer=True)
                  for field in ("repeat", "measurement", "begin_ticks", "end_ticks", "frequency_hz", "disjoint")}
        sample["frame"] = number(row["frame"], context, integer=True, unavailable=True)
        sample["scenario"], sample["status"] = row["scenario"], row["status"]
        if not sample["scenario"] or sample["status"] not in ("valid", "invalid", "pending", "dropped"):
            raise ValueError(f"{context}: invalid scenario or query status")
        if sample["measurement"] not in (0, 1) or sample["disjoint"] not in (0, 1):
            raise ValueError(f"{context}: invalid boolean")
        if (sample["frame"] >= 0) != bool(sample["measurement"]):
            raise ValueError(f"{context}: frame and measurement window disagree")
        try:
            result = int(row["hresult"], 16)
        except ValueError as error:
            raise ValueError(f"{context}: invalid HRESULT") from error
        if sample["status"] == "valid":
            if result or sample["disjoint"] or not sample["frequency_hz"] or sample["end_ticks"] < sample["begin_ticks"]:
                raise ValueError(f"{context}: invalid timestamps reported as valid")
            elapsed = number(row["gpu_timestamp_elapsed_ms"], context)
            ticks_ms = (sample["end_ticks"] - sample["begin_ticks"]) * 1000 / sample["frequency_hz"]
            if not math.isclose(elapsed, ticks_ms, rel_tol=1e-6, abs_tol=1e-6):
                raise ValueError(f"{context}: elapsed time disagrees with raw GPU ticks")
            sample["elapsed"] = elapsed
        elif row["gpu_timestamp_elapsed_ms"]:
            raise ValueError(f"{context}: unavailable elapsed time must be empty")
        rows.append(sample)
    groups = []
    for scenario in sorted({row["scenario"] for row in rows if row["measurement"]}):
        selected = [row for row in rows if row["measurement"] and row["scenario"] == scenario]
        counts = {status: sum(row["status"] == status for row in selected)
                  for status in ("valid", "invalid", "pending", "dropped")}
        values = [row["elapsed"] for row in selected if row["status"] == "valid"]
        groups.append({"scenario": scenario, **counts,
                       "gpu_timestamp_elapsed_ms": distribution(values) if values else None})
    return rows, groups


def analyze_presentation(records, path):
    rows, seen = [], set()
    for line_number, row in records:
        context = f"{path}:{line_number} presentation"
        sample = dict(row)
        for field in PRESENT_COLUMNS:
            if field in ("kind", "scenario", "frame_state", "media_state"):
                continue
            if field.endswith("_hr"):
                try:
                    sample[field] = int(row[field], 16)
                except ValueError as error:
                    raise ValueError(f"{context}: invalid {field}") from error
                if not 0 <= sample[field] <= 0xffffffff:
                    raise ValueError(f"{context}: invalid HRESULT range")
            else:
                sample[field] = number(row[field], f"{context} {field}", integer=True,
                                       unavailable=field in ("render_id", "repeat", "frame"))
        if sample["sample_id"] in seen:
            raise ValueError(f"{context}: duplicate snapshot id")
        seen.add(sample["sample_id"])
        if sample["kind"] not in ("initial", "after-render", "final") or not sample["scenario"]:
            raise ValueError(f"{context}: invalid kind or scenario")
        if sample["measurement"] not in (0, 1) or sample["cpu_presented"] not in (0, 1):
            raise ValueError(f"{context}: invalid boolean")
        if sample["measurement"] and (sample["kind"] != "after-render" or
                                      min(sample["render_id"], sample["repeat"], sample["frame"]) < 0):
            raise ValueError(f"{context}: invalid measured snapshot metadata")
        for prefix in ("frame", "media"):
            state = sample[f"{prefix}_state"]
            if state not in PRESENT_STATES:
                raise ValueError(f"{context}: invalid {prefix} state")
            if state in PRESENT_VALID:
                fields = ("present_id", "present_refresh_count", "sync_refresh_count", "sync_qpc")
                if sample[f"{prefix}_hr"] or any(sample[("media_" if prefix == "media" else "") + key] <= 0 for key in fields):
                    raise ValueError(f"{context}: incomplete or failed statistics reported as valid")
        rows.append(sample)
    groups = []
    for scenario in sorted({row["scenario"] for row in rows if row["measurement"]}):
        selected = [row for row in rows if row["measurement"] and row["scenario"] == scenario]
        group = {"scenario": scenario, "samples": len(selected)}
        for prefix in ("frame", "media"):
            counts = {state: sum(row[f"{prefix}_state"] == state for row in selected) for state in PRESENT_STATES}
            group[prefix] = {"states": counts, "valid_snapshots": sum(counts[state] for state in PRESENT_VALID),
                             "advancing_snapshots": counts["advanced"],
                             "hresults": {f"{result:08x}": sum(row[f"{prefix}_hr"] == result for row in selected)
                                          for result in sorted({row[f"{prefix}_hr"] for row in selected})}}
        groups.append(group)
    return rows, groups


def analyze(path):
    records = read_records(path)
    frames = []
    seen = set()
    for line_number, row in records["FRAME_CSV"]:
        context = f"{path}:{line_number}"
        parsed = {"scenario": row["scenario"]}
        if not parsed["scenario"]:
            raise ValueError(f"{context}: empty scenario")
        for field, value in row.items():
            if field.endswith("_ms") or field in ("repeat", "frame", "active", "presented", "view_reused", "pixel_rows", "vm_builds"):
                parsed[field] = number(value, f"{context} {field}", integer=not field.endswith("_ms"))
        for flag in ("active", "presented", "view_reused"):
            if parsed[flag] not in (0, 1):
                raise ValueError(f"{context}: {flag} must be 0 or 1")
        key = (parsed["scenario"], parsed["repeat"], parsed["frame"])
        if key in seen:
            raise ValueError(f"{context}: duplicate frame {key}")
        seen.add(key)
        work = parsed["total_ms"] - parsed["present_ms"]
        # Each source timing is rounded independently to six decimal places.
        if work < -0.000002:
            raise ValueError(f"{context}: present_ms exceeds total_ms")
        parsed["cpu_work_ms"] = max(0.0, work)
        frames.append(parsed)

    groups = []
    for scenario in sorted({row["scenario"] for row in frames}):
        scenario_rows = [row for row in frames if row["scenario"] == scenario]
        for state in ("all", "active", "inactive"):
            rows = [row for row in scenario_rows if state == "all" or bool(row["active"]) == (state == "active")]
            if not rows:
                continue
            groups.append({
                "scenario": scenario, "state": state, "frames": len(rows),
                "repeats": len({row["repeat"] for row in rows}),
                "presented_frames": sum(row["presented"] for row in rows),
                "view_reused_frames": sum(row["view_reused"] for row in rows),
                "pixel_cache_frames": sum(row["pixel_rows"] > 0 for row in rows),
                "metrics_ms": {field: distribution([row[field] for row in rows])
                               for field in sorted(rows[0]) if field.endswith("_ms")},
            })
            if "vm_builds" in rows[0]:
                builds = [row["vm_builds"] for row in rows]
                groups[-1]["vm_builds"] = {
                    "total": sum(builds), "mean_per_frame": sum(builds) / len(builds),
                    "max_per_frame": max(builds),
                }

    vm_rows = []
    vm_seen = set()
    for line_number, row in records["VM_CSV"]:
        context = f"{path}:{line_number}"
        parsed = {"scenario": row["scenario"]}
        if not parsed["scenario"]:
            raise ValueError(f"{context}: empty scenario")
        for field, value in row.items():
            if field.endswith("_ms") or field in ("repeat", "sample", "vm_builds"):
                parsed[field] = number(value, f"{context} {field}", integer=not field.endswith("_ms"))
        key = (parsed["scenario"], parsed["repeat"], parsed["sample"])
        if key in vm_seen:
            raise ValueError(f"{context}: duplicate VM sample {key}")
        vm_seen.add(key)
        vm_rows.append(parsed)
    vm_groups = []
    for scenario in sorted({row["scenario"] for row in vm_rows}):
        rows = [row for row in vm_rows if row["scenario"] == scenario]
        vm_groups.append({
            "scenario": scenario, "samples": len(rows),
            "repeats": len({row["repeat"] for row in rows}),
            "vm_builds": sum(row["vm_builds"] for row in rows),
            "metrics_ms": {field: distribution([row[field] for row in rows])
                           for field in sorted(rows[0]) if field.endswith("_ms")},
        })

    memory_rows = []
    for line_number, row in records["MEMORY_CSV"]:
        context = f"{path}:{line_number}"
        sample = {"time_ms": number(row["time_ms"], f"{context} time_ms")}
        for field in MEMORY_FIELDS:
            sample[field] = number(row[field], f"{context} {field}", integer=True, unavailable=True)
        if memory_rows and sample["time_ms"] < memory_rows[-1]["time_ms"]:
            raise ValueError(f"{context}: memory time_ms moved backwards")
        memory_rows.append(sample)
    gpu_rows, gpu_groups = analyze_gpu(records["GPU_CSV"], path)
    presentation_rows, presentation_groups = analyze_presentation(records["PRESENT_CSV"], path)
    if not frames and not memory_rows and not vm_rows and not gpu_rows and not presentation_rows:
        raise ValueError(f"{path}: no measurement records")
    memory = {"samples": len(memory_rows)}
    if memory_rows:
        memory["first_time_ms"] = memory_rows[0]["time_ms"]
        memory["last_time_ms"] = memory_rows[-1]["time_ms"]
        memory["fields"] = {}
        for field in MEMORY_FIELDS:
            values = [row[field] for row in memory_rows if row[field] != -1]
            memory["fields"][field] = {
                "available_samples": len(values), "unavailable_samples": len(memory_rows) - len(values),
                "first": values[0] if values else None, "last": values[-1] if values else None,
                "peak": max(values) if values else None,
            }
    return {"source": str(path), "frames": len(frames), "groups": groups,
            "vm_samples": len(vm_rows), "vm_groups": vm_groups, "memory": memory,
            "gpu_samples": len(gpu_rows), "gpu_groups": gpu_groups,
            "presentation_samples": len(presentation_rows), "presentation_groups": presentation_groups}


def render(report):
    lines = list(report["notes"])
    for dataset in report["datasets"]:
        lines.append(f"\n{dataset['source']} ({dataset['frames']} frame samples)")
        lines.append("scenario/state metric count p50 p95 p99 max (ms)")
        for group in dataset["groups"]:
            if "vm_builds" in group:
                builds = group["vm_builds"]
                lines.append(f"{group['scenario']}/{group['state']} vm_builds: total={builds['total']} " +
                             f"mean_per_frame={builds['mean_per_frame']:.6f} max_per_frame={builds['max_per_frame']}")
            for field, stats in group["metrics_ms"].items():
                lines.append(f"{group['scenario']}/{group['state']} {field} {stats['count']} " +
                             " ".join(f"{stats[name]:.6f}" for name in ("p50", "p95", "p99", "max")))
        if dataset["vm_samples"]:
            lines.append(f"Isolated CPU model samples: {dataset['vm_samples']}")
            lines.append("scenario metric count p50 p95 p99 max (ms)")
            for group in dataset["vm_groups"]:
                lines.append(f"{group['scenario']}: vm_builds={group['vm_builds']} repeats={group['repeats']}")
                for field, stats in group["metrics_ms"].items():
                    lines.append(f"{group['scenario']} {field} {stats['count']} " +
                                 " ".join(f"{stats[name]:.6f}" for name in ("p50", "p95", "p99", "max")))
        for group in dataset["gpu_groups"]:
            lines.append(f"GPU {group['scenario']}: " + " ".join(
                f"{status}={group[status]}" for status in ("valid", "invalid", "pending", "dropped")))
            stats = group["gpu_timestamp_elapsed_ms"]
            if stats:
                lines.append("gpu_timestamp_elapsed_ms p50/p95/p99/max: " + " ".join(
                    f"{stats[name]:.6f}" for name in ("p50", "p95", "p99", "max")))
        for group in dataset["presentation_groups"]:
            lines.append(f"Presentation {group['scenario']}: snapshots={group['samples']}")
            for prefix in ("frame", "media"):
                stats = group[prefix]
                lines.append(f"{prefix}: valid={stats['valid_snapshots']} advancing={stats['advancing_snapshots']} " +
                             " ".join(f"{state}={count}" for state, count in stats["states"].items()))
        memory = dataset["memory"]
        lines.append(f"Memory samples: {memory['samples']}")
        if memory["samples"]:
            lines.append(f"Memory time_ms: {memory['first_time_ms']} -> {memory['last_time_ms']}")
            for field, stats in memory["fields"].items():
                lines.append(f"{field}: first={stats['first']} last={stats['last']} peak={stats['peak']} " +
                             f"available={stats['available_samples']} unavailable={stats['unavailable_samples']}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+", help="Fixture logs or extracted frame/model/memory CSV files")
    parser.add_argument("--output", type=Path, help="Write a JSON report in addition to the console summary")
    args = parser.parse_args()
    try:
        report = {"notes": NOTES, "datasets": [analyze(path) for path in args.inputs]}
        if args.output:
            args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(render(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
