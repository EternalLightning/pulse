# SPDX-License-Identifier: Apache-2.0
import io
from pathlib import Path
import unittest
from unittest.mock import patch

import analyze_animation_frames as analyzer


HEADER = "scenario,repeat,frame,active,interval_ms,input_ms,prepare_ms,paint_ms,submit_ms,present_ms,after_ms,total_ms,presented,view_reused,pixel_rows,vm_ms,vm_layout_ms"


def frame(index, active=1):
    return f"view,0,{index},{active},{index + 1},0.1,1,2,0.1,4,0.1,8,1,0,0,0.2,0.1"


class AnalysisTests(unittest.TestCase):
    def analyze_text(self, content):
        with patch.object(Path, "open", return_value=io.StringIO(content)):
            return analyzer.analyze("fixture.log")

    def test_nearest_rank_small_and_percentile_boundaries(self):
        self.assertEqual(analyzer.distribution([7]), {"count": 1, "p50": 7, "p95": 7, "p99": 7, "max": 7})
        stats = analyzer.distribution(list(range(100, 0, -1)))
        self.assertEqual([stats[name] for name in ("p50", "p95", "p99", "max")], [50, 95, 99, 100])
        self.assertEqual(analyzer.distribution([1, 2])["p50"], 1)

    def test_tagged_log_active_groups_dynamic_cpu_fields_and_plain_csv(self):
        plain = HEADER + "\n" + "\n".join(frame(index, index % 2) for index in range(4))
        tagged = "[DISPLAY] refresh_hz=unknown\n" + "\n".join("FRAME_CSV," + line for line in plain.splitlines())
        result = self.analyze_text(tagged)
        self.assertEqual(result, {**self.analyze_text(plain), "source": result["source"]})
        groups = {group["state"]: group for group in result["groups"]}
        self.assertEqual([groups[state]["frames"] for state in ("all", "active", "inactive")], [4, 2, 2])
        self.assertEqual(groups["all"]["metrics_ms"]["cpu_work_ms"]["p50"], 4)
        self.assertEqual(groups["active"]["metrics_ms"]["interval_ms"]["max"], 4)
        self.assertIn("vm_layout_ms", groups["all"]["metrics_ms"])

    def test_optional_frame_vm_build_counts_are_separate_from_timings(self):
        legacy = self.analyze_text(HEADER + "\n" + frame(0))
        self.assertNotIn("vm_builds", legacy["groups"][0])
        csv_text = HEADER + ",vm_builds\n" + "\n".join(
            frame(index, index % 2) + f",{builds}" for index, builds in enumerate((0, 2, 0, 4)))
        groups = {group["state"]: group for group in self.analyze_text(csv_text)["groups"]}
        self.assertEqual(groups["all"]["vm_builds"], {"total": 6, "mean_per_frame": 1.5, "max_per_frame": 4})
        self.assertEqual(groups["active"]["vm_builds"], {"total": 6, "mean_per_frame": 3, "max_per_frame": 4})
        self.assertEqual(groups["inactive"]["vm_builds"], {"total": 0, "mean_per_frame": 0, "max_per_frame": 0})
        self.assertNotIn("vm_builds", groups["all"]["metrics_ms"])
        for value in ("-1", "0.5"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.analyze_text(HEADER + ",vm_builds\n" + frame(0) + "," + value)

    def test_memory_unavailable_and_first_last_peak(self):
        header = "MEMORY_CSV,time_ms," + ",".join(analyzer.MEMORY_FIELDS)
        rows = ["MEMORY_CSV," + str(index) + "," + ",".join(map(str, values)) for index, values in enumerate([
            [-1, 10, -1, -1, -1, -1], [20, 30, -1, -1, -1, -1], [15, 25, -1, -1, -1, -1],
        ])]
        memory_log = "\n".join([header, *rows])
        memory = self.analyze_text("FRAME_CSV," + HEADER + "\nFRAME_CSV," + frame(0) + "\n" + memory_log)["memory"]
        self.assertEqual(memory, self.analyze_text(memory_log.replace("MEMORY_CSV,", ""))["memory"])
        self.assertEqual(memory["fields"]["working_set_bytes"], {
            "available_samples": 2, "unavailable_samples": 1, "first": 20, "last": 15, "peak": 20,
        })
        self.assertIsNone(memory["fields"]["gpu_local_usage_bytes"]["peak"])

    def test_gpu_timestamps_preserve_ticks_and_exclude_unavailable_and_warmup(self):
        header = "id,scenario,repeat,frame,measurement,status,gpu_timestamp_elapsed_ms,begin_ticks,end_ticks,frequency_hz,disjoint,hresult"
        ticks = 2 ** 60
        rows = [f"0,scroll,0,0,1,valid,1,{ticks},{ticks + 1000},1000000,0,00000000",
                "1,scroll,0,1,1,invalid,,0,0,0,1,00000000",
                "2,scroll,0,2,1,pending,,0,0,0,0,00000001",
                "3,scroll,0,3,1,dropped,,0,0,0,0,00000001",
                "4,scroll,0,-1,0,valid,100,0,100000,1000000,0,00000000"]
        plain = "\n".join([header, *rows])
        report = self.analyze_text("\n".join("GPU_CSV," + line for line in plain.splitlines()))
        self.assertEqual(report, self.analyze_text(plain))
        self.assertEqual(report["gpu_samples"], 5)
        group = report["gpu_groups"][0]
        self.assertEqual([group[status] for status in ("valid", "invalid", "pending", "dropped")], [1, 1, 1, 1])
        self.assertEqual(group["gpu_timestamp_elapsed_ms"]["p95"], 1)
        self.assertEqual(report["frames"], 0)

    def test_gpu_invalid_results_are_unavailable_not_zero_duration(self):
        header = "GPU_CSV,id,scenario,repeat,frame,measurement,status,gpu_timestamp_elapsed_ms,begin_ticks,end_ticks,frequency_hz,disjoint,hresult"
        report = self.analyze_text(header + "\nGPU_CSV,0,view,0,0,1,invalid,,0,0,0,1,00000000")
        self.assertIsNone(report["gpu_groups"][0]["gpu_timestamp_elapsed_ms"])

    def test_gpu_reject_inconsistent_or_duplicate_results(self):
        header = "GPU_CSV,id,scenario,repeat,frame,measurement,status,gpu_timestamp_elapsed_ms,begin_ticks,end_ticks,frequency_hz,disjoint,hresult"
        row = "GPU_CSV,0,view,0,0,1,valid,1,0,1000,1000000,0,00000000"
        invalid = [row.replace(",valid,1,", ",valid,nan,"), row.replace(",valid,1,", ",valid,2,"),
                   row.replace("1000000,0,", "0,0,"), row.replace("1000000,0,", "1000000,1,"),
                   row.replace("00000000", "80004005"), row.replace(",0,0,1,valid", ",0,-1,1,valid"),
                   row.replace(",valid,1,", ",pending,1,")]
        for bad in [*invalid, row + "\n" + row]:
            with self.subTest(row=bad), self.assertRaises(ValueError):
                self.analyze_text(header + "\n" + bad)

    def presentation_log(self, overrides):
        base = dict.fromkeys(analyzer.PRESENT_COLUMNS, "0")
        base.update(kind="after-render", scenario="scroll", measurement="1", cpu_presented="1",
                    sample_qpc=str(2 ** 60), last_present_id="10", frame_state="baseline",
                    present_id="9", present_refresh_count="100", sync_refresh_count="100",
                    sync_qpc=str(2 ** 60 - 100), media_state="unsupported",
                    media_qi_hr="80004002", media_hr="80004002")
        lines = ["PRESENT_CSV," + ",".join(analyzer.PRESENT_COLUMNS)]
        for changes in overrides:
            row = {**base, **{key: str(value) for key, value in changes.items()}}
            lines.append("PRESENT_CSV," + ",".join(row[field] for field in analyzer.PRESENT_COLUMNS))
        return "\n".join(lines)

    def test_presentation_progress_is_not_polling_rate_or_dropped_frames(self):
        log = self.presentation_log([
            {},
            {"sample_id": 1, "frame": 1, "render_id": 1, "frame_state": "unchanged", "sync_qpc": 2 ** 60},
            {"sample_id": 2, "frame": 2, "render_id": 2, "frame_state": "advanced", "present_id": 10},
            {"sample_id": 3, "kind": "final", "frame": -1, "measurement": 0, "frame_state": "advanced", "present_id": 11},
        ])
        report = self.analyze_text(log)
        self.assertEqual(report, self.analyze_text(log.replace("PRESENT_CSV,", "")))
        self.assertEqual(report["presentation_samples"], 4)
        group = report["presentation_groups"][0]
        self.assertEqual(group["samples"], 3)
        self.assertEqual(group["frame"]["valid_snapshots"], 3)
        self.assertEqual(group["frame"]["advancing_snapshots"], 1)
        self.assertEqual(group["media"]["valid_snapshots"], 0)
        self.assertEqual(group["media"]["states"]["unsupported"], 3)
        self.assertNotIn("interval_ms", group)
        self.assertNotIn("dropped_frames", group)

    def test_presentation_zero_statistics_do_not_mean_zero_drops(self):
        report = self.analyze_text(self.presentation_log([
            {"frame_state": "zero", "present_id": 0, "present_refresh_count": 0,
             "sync_refresh_count": 0, "sync_qpc": 0},
        ]))
        frame = report["presentation_groups"][0]["frame"]
        self.assertEqual(frame["states"]["zero"], 1)
        self.assertEqual(frame["valid_snapshots"], 0)
        self.assertEqual(frame["advancing_snapshots"], 0)

    def test_presentation_reject_invalid_or_duplicate_snapshots(self):
        for changes in ({"cpu_presented": 2}, {"kind": "initial"}, {"frame_state": "unknown"},
                        {"frame_hr": "80004005"}, {"present_id": 0}, {"media_hr": "invalid-hex"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.analyze_text(self.presentation_log([changes]))
        with self.assertRaises(ValueError):
            self.analyze_text(self.presentation_log([{}, {}]))

    def test_reject_invalid_memory_values_and_backward_time(self):
        header = "time_ms," + ",".join(analyzer.MEMORY_FIELDS) + "\n"
        for rows in ("0,-2,0,0,0,0,0", "0,0.5,0,0,0,0,0", "nan,0,0,0,0,0,0", "1,0,0,0,0,0,0\n0,0,0,0,0,0,0"):
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                self.analyze_text(header + rows)

    def test_vm_cpu_only_has_separate_metrics_and_no_frame_semantics(self):
        csv_text = "scenario,repeat,sample,total_ms,vm_ms,vm_base_ms,vm_other_ms,vm_builds\nfilter,0,0,6,5,3,2,1\nfilter,0,1,4,3,2,1,0"
        result = self.analyze_text("\n".join("VM_CSV," + line for line in csv_text.splitlines()))
        self.assertEqual(result, self.analyze_text(csv_text))
        self.assertEqual(result["frames"], 0)
        self.assertEqual(result["groups"], [])
        self.assertEqual(result["vm_samples"], 2)
        group = result["vm_groups"][0]
        self.assertEqual(group["vm_builds"], 1)
        self.assertEqual(group["metrics_ms"]["vm_other_ms"]["max"], 2)
        self.assertNotIn("cpu_work_ms", group["metrics_ms"])
        self.assertNotIn("present_ms", group["metrics_ms"])
        with self.assertRaises(ValueError):
            self.analyze_text(csv_text + "\nfilter,0,1,4,3,2,1,0")

    def test_reject_invalid_or_partial_frame_measurements(self):
        row = frame(0)
        invalid_rows = [row.replace(",8,1,", ",3,1,"), row.replace(",8,1,", ",nan,1,"),
                        row.replace(",8,1,", ",inf,1,"), frame(0, 2), frame(-1), row.rsplit(",", 1)[0]]
        for invalid in invalid_rows:
            with self.subTest(row=invalid), self.assertRaises(ValueError):
                self.analyze_text(HEADER + "\n" + invalid)
        for invalid in (HEADER + "\n" + row + "\n" + row, HEADER, "[PROFILE_DONE] failures=0"):
            with self.subTest(content=invalid), self.assertRaises(ValueError):
                self.analyze_text(invalid)


if __name__ == "__main__":
    unittest.main()
