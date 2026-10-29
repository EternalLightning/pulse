from pathlib import Path
import json
import os
import subprocess

folder = Path(__file__).resolve().parent
repo = folder.parents[2]
cases = [
    ("pulse_smooth_scroll_test", []),
    ("pulse_pane_animation_application_test", ["--page-scroll-only"]),
    ("pulse_pane_animation_application_test", ["--column-scroll-only"]),
    ("pulse_pane_animation_application_test", ["--surface-scroll"]),
    ("pulse_menu_text_test", ["--smooth-scroll"]),
    ("pulse_menu_text_test", ["--nested"]),
    ("pulse_file_operation_failure_test", ["--scroll-only"]),
    ("pulse_text_diff_ui_test", ["--smooth-scroll"]),
    ("pulse_global_search_window_test", ["--smooth-scroll"]),
    ("pulse_playback_ui_test", ["--smooth-scroll"]),
]
results = []
for target, args in cases:
    name = target + ("-" + args[0][2:] if args else "")
    env = dict(os.environ, PULSE_TEST_FIXTURE_ROOT=str(folder), PULSE_TEST_SNAPSHOT_STDOUT="1")
    try:
        result = subprocess.run([str(repo / "build-release" / (target + ".exe")), *args],
                                cwd=repo, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=45)
        output = result.stdout.decode("utf-8", errors="replace")
        record = dict(target=target, args=args, exit_code=result.returncode,
                      status="passed" if result.returncode == 0 else "failed")
        if result.returncode == 0xC0000142 and not result.stdout:
            record["status"] = "startup_failed"
        output += f"\nexit_code={result.returncode} (0x{result.returncode & 0xffffffff:08X})\n"
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode("utf-8", errors="replace") + "\ntimeout=45s\n"
        record = dict(target=target, args=args, status="timeout")
    (folder / (name + ".log")).write_text(output, encoding="utf-8")
    results.append(record)
    print(name, record, flush=True)
(folder / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
