# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Released-body Completion boundaries using only disposable generated files and hosts."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

root = Path(__file__).resolve().parent.parent
base = root / 'bench_data/document-memory-policy-fixtures' / str(time.time_ns())
binary = base / 'bin'
slots = base / 'slots'
binary.mkdir(parents=True)
slots.mkdir()
shutil.copy2(root / 'build_realtime/pulse_pdf_bench.exe', binary / 'pulse_pdf_bench.exe')
shutil.copy2(root / 'build_realtime/pulse_document_failure_fixture.exe', binary / 'Pulse.Document.exe')
checks, results = [], []

def run(mode, cancel_ms=0, engine="pdfium"):
    case = base / (mode + '-' + str(len(results)))
    case.mkdir()
    path = case / 'generated.pdf'
    path.write_bytes(b'%PDF-1.7 generated isolated fixture\n')
    before = path.stat()
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    env = dict(os.environ, PULSE_DOCUMENT_ADMISSION_DIR=str(slots), PULSE_DOCUMENT_FAILURE_FIXTURE=mode)
    process = subprocess.run([str(binary / 'pulse_pdf_bench.exe'), engine, str(path), '', str(cancel_ms)],
                             env=env, capture_output=True, timeout=34)
    header, body = process.stdout.split(b'\n', 1)
    value = json.loads(header)
    after = path.stat()
    value.update(mode=mode, body=body.decode('utf-8'),
                 same_size_time=(before.st_size,before.st_mtime_ns)==(after.st_size,after.st_mtime_ns),
                 changed=digest!=hashlib.sha256(path.read_bytes()).hexdigest())
    results.append(value)
    return value

def check(good, label):
    checks.append(bool(good))
    print(('[PASS] ' if good else '[FAIL] ') + label, flush=True)
    (base / 'results.json').write_text(json.dumps({'checks': checks, 'results': results}, indent=2), encoding='utf-8')

v = run('tail_delay')
check(v['ok'] and v['us'] >= 250000 and v['body'] == 'completed' and v['memory_recycles'] == 1
      and v['released_private_bytes'] == 192 * 1024 * 1024 and v['read_attempts'] == 1,
      'parent waits for released-body Completion before high-water recycle')
v = run('tail_below')
check(v['ok'] and v['body'] == 'completed' and v['memory_recycles'] == 0
      and v['released_private_bytes'] == 192 * 1024 * 1024 - 1 and v['read_attempts'] == 1,
      'current private below threshold keeps the worker')
v = run('tail_delay', cancel_ms=100)
check(not v['ok'] and v['error'] == 1223 and v['chars'] == 0 and v['body'] == ''
      and v['memory_recycles'] == 0, 'cancelled tail never publishes partial body')
for mode in ['tail_flags', 'tail_size', 'tail_magic', 'tail_range']:
    v = run(mode)
    check(not v['ok'] and v['error'] == 13 and v['chars'] == 0 and v['read_attempts'] == 1,
          mode + ' rejected')
v = run('exit')
check(not v['ok'] and v['child_exit_code'] == 0xe1234567 and v['read_attempts'] == 1,
      'early process exit retains failure diagnostics')

for mode in ['fresh_filter', 'fresh_filter_exit']:
    v = run(mode, engine='auto')
    check(v['ok'] and v['body'] == 'completed' and v['read_attempts'] == 2
          and v['fresh_filter_fallbacks'] == 1 and v['fresh_filter_recoveries'] == 1
          and v['fresh_filter_first_error'] != 0, mode + ' enters one clean IFilter and preserves first failure')
v = run('fresh_filter_always', engine='auto')
check(not v['ok'] and v['read_attempts'] == 2 and v['fresh_filter_fallbacks'] == 1
      and v['fresh_filter_recoveries'] == 0, 'second engine failure is final')
v = run('fresh_filter_changed', engine='auto')
check(not v['ok'] and v['same_size_time'] and v['changed'] and v['fresh_filter_fallbacks'] == 0
      and v['fresh_filter_skips'] == 1, 'ChangeTime blocks fallback after same-size same-mtime rewrite')
v = run('fresh_filter_hang', cancel_ms=100, engine='auto')
check(not v['ok'] and v['error'] == 1223 and v['chars'] == 0 and v['read_attempts'] == 2,
      'cancellation stops clean IFilter attempt without publishing a body')
v = run('fresh_filter', engine='pdfium')
check(not v['ok'] and v['read_attempts'] == 1 and v['fresh_filter_fallbacks'] == 0,
      'forced PDFium never invokes IFilter')
v = run('fresh_filter', engine='ifilter')
check(v['ok'] and v['read_attempts'] == 1 and v['fresh_filter_fallbacks'] == 0,
      'forced IFilter stays single request')

kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                              ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
kernel.CreateFileW.restype = ctypes.c_void_p
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
handles = [kernel.CreateFileW(str(slots / f'slot-{i}.lock'), 0xc0000000, 0, None, 4, 0x80, None) for i in range(4)]
released = all(handle != ctypes.c_void_p(-1).value for handle in handles)
for handle in handles:
    if handle != ctypes.c_void_p(-1).value:
        kernel.CloseHandle(handle)
check(released, 'all four global slots released after completion, cancellation and early exit')
print(str(base), flush=True)
raise SystemExit(0 if all(checks) else 1)
