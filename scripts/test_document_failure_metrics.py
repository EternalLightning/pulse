# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Exercise parser diagnostics with a disposable fixture host, never user processes."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import time

root = Path(__file__).resolve().parent.parent
base = root / 'bench_data/pdf-input-comparison'
bench = base / 'failures/pulse_pdf_bench.exe'
checks = []
results = []
for mode in ['exit', 'body', 'header', 'hang']:
    env = dict(os.environ, PULSE_DOCUMENT_ADMISSION_DIR=str(base / 'slots'), PULSE_DOCUMENT_FAILURE_FIXTURE=mode)
    args = [str(bench), 'pdfium', str(base / 'synthetic.pdf'), '']
    if mode == 'hang':
        args.append('100')
    process = subprocess.run(args, env=env, capture_output=True, timeout=5)
    value = json.loads(process.stdout.split(b'\n', 1)[0])
    value['mode'] = mode
    results.append(value)
    stage = 3 if mode == 'body' else 2
    good = not value['ok'] and value['child_failure_stage'] == stage and value['child_exit_known'] == 1
    # A timed cancellation can occur before the fixture finishes allocating.
    # The three completed fixture paths independently verify the 32 MiB peak.
    required_peak = 1 if mode == 'hang' else 32 * 1024 * 1024
    good = good and value['child_memory_known'] != 0 and value['child_peak_private_bytes'] >= required_peak
    if mode in ['exit', 'body']:
        good = good and value['error'] in [109, 1067] and value['child_exit_code'] == 0xe1234567
    elif mode == 'header':
        good = good and value['error'] == 13
    else:
        good = good and value['error'] == 1223 and value['child_exit_code'] == 259 and value['us'] < 1100000
    checks.append(bool(good))
    print(('[PASS] ' if good else '[FAIL] ') + mode + ': ' + json.dumps(value), flush=True)
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                              ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
kernel.CreateFileW.restype = ctypes.c_void_p
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
deadline = time.monotonic() + 1
released = False
while time.monotonic() < deadline:
    handles = [kernel.CreateFileW(str(base / 'slots' / f'slot-{i}.lock'),
                                 0xc0000000, 0, None, 4, 0x80, None) for i in range(4)]
    released = all(handle != ctypes.c_void_p(-1).value for handle in handles)
    for handle in handles:
        if handle != ctypes.c_void_p(-1).value:
            kernel.CloseHandle(handle)
    if released:
        break
    time.sleep(.02)
checks.append(released)
print(('[PASS] ' if released else '[FAIL] ') + 'all four parser slots released after failures and cancellation', flush=True)
(base / 'failure-metrics.json').write_text(json.dumps({'passed': all(checks), 'results': results}, indent=2), encoding='utf-8')
raise SystemExit(0 if all(checks) else 1)
