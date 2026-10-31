# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Compare frozen reader builds and check bounded shared-parser lifetimes."""
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import threading
import time
import zipfile

root = Path(__file__).resolve().parent.parent
base = root / 'bench_data/document-pool-comparison'
env = dict(os.environ, PULSE_DOCUMENT_ADMISSION_DIR=str(base / 'slots'), PULSE_PDF_ENGINE='pdfium')
manifest = base / 'original-pdfs.txt'
before = base / 'before/pulse_document_pool_bench.exe'
after = base / 'after/pulse_document_pool_bench.exe'
checks = []

def check(ok, name):
    checks.append(bool(ok))
    print(('[PASS] ' if ok else '[FAIL] ') + name, flush=True)

def command(exe=after, paths=manifest, threads=6, rounds=3, needle='3d3s', cancel=None):
    args = [str(exe), str(paths), str(threads), str(rounds), needle]
    if cancel is not None:
        args.append(str(cancel))
    return args

def run(name, args, extra=None):
    if '--concurrent-only' in sys.argv and name != 'idle-contender-office':
        return json.loads((base / f'{name}.json').read_text(encoding='utf-8'))
    result = subprocess.run(args, env=dict(env, **(extra or {})), capture_output=True, timeout=90)
    value = json.loads(result.stdout)
    value['exit_code'] = result.returncode
    (base / f'{name}.json').write_text(json.dumps(value, indent=2), encoding='utf-8')
    return value

def signatures(value):
    return [(item['error'], item['chars'], item['hash']) for item in value['results']]

baseline, pooled = [], []
for trial in range(3):
    old = run(f'baseline-{trial}', command(before))
    new = run(f'pooled-{trial}', command())
    baseline.append(old)
    pooled.append(new)
    check(old['errors'] == new['errors'] == 0 and signatures(old) == signatures(new), f'original PDFs identical, alternating trial {trial + 1}')
    check(new['process_starts'] <= 4 and new['peak_children'] <= 4 and new['remaining_children'] == 0,
          f'trial {trial + 1} reuses at most four parsers and releases all')

# A small valid package is enough to exercise PDF -> OPC -> PDF reuse.
office = base / 'mixed.docx'
with zipfile.ZipFile(office, 'w', zipfile.ZIP_DEFLATED) as package:
    package.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/></Types>')
    package.writestr('_rels/.rels', '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>')
    package.writestr('word/document.xml', '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>Mixed Office 中文 3d3s text</w:t></w:r></w:p></w:body></w:document>')
pdfs = manifest.read_text(encoding='utf-8').splitlines()
mixed = base / 'mixed-files.txt'
mixed.write_text('\n'.join([pdfs[0], str(office), pdfs[1], str(office)]), encoding='utf-8')
old_mixed = run('mixed-before', command(before, mixed, 1, 3))
new_mixed = run('mixed-after', command(after, mixed, 1, 3))
check(old_mixed['errors'] == new_mixed['errors'] == 0 and signatures(old_mixed) == signatures(new_mixed)
      and new_mixed['process_starts'] == 1 and new_mixed['remaining_children'] == 0,
      'one warm parser alternates PDF and Office with identical text')

cancelled = run('cancelled', command(cancel=100))
check(cancelled['remaining_children'] == 0 and cancelled['elapsed_us'] < 1100000 and
      any(item['error'] == 1223 for item in cancelled['results']),
      'cancelled readers and pool waiters release processes within one second')
two_pools = run('two-pools-first-cancelled', command(cancel=100), {'PULSE_POOL_BENCH_SECOND_POOL': '1'})
expected = signatures(baseline[0])
check(two_pools['remaining_children'] == 0 and two_pools['peak_children'] <= 4 and
      any(item['error'] == 1223 for item in two_pools['results']) and
      any(item['error'] == 0 for item in two_pools['results']) and
      all(item['error'] == 1223 or (item['error'], item['chars'], item['hash']) == expected[index]
          for index, item in enumerate(two_pools['results'])),
      'cancelling one pool lets a second pool finish correctly')

class ProcessEntry(c.Structure):
    _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD), ('heap', c.c_size_t),
                ('module', w.DWORD), ('threads', w.DWORD), ('parent', w.DWORD),
                ('priority', w.LONG), ('flags', w.DWORD), ('exe', w.WCHAR * 260)]
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel.GetExitCodeProcess.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]

def child_count(parents):
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == c.c_void_p(-1).value:
        return 0
    entry = ProcessEntry()
    entry.size = c.sizeof(entry)
    handles = []
    found = kernel.Process32FirstW(snapshot, c.byref(entry))
    while found:
        if entry.parent in parents and entry.exe.lower() == 'pulse.document.exe':
            handle = kernel.OpenProcess(0x101000, False, entry.pid)
            if handle:
                handles.append(handle)
        found = kernel.Process32NextW(snapshot, c.byref(entry))
    kernel.CloseHandle(snapshot)
    count = 0
    # A Toolhelp snapshot can retain already-exited processes. Validate the
    # acquired process handles after enumeration instead of counting stale PIDs.
    for handle in handles:
        code = w.DWORD()
        count += bool(kernel.GetExitCodeProcess(handle, c.byref(code))) and code.value == 259 and kernel.WaitForSingleObject(handle, 0) == 258
        kernel.CloseHandle(handle)
    return count

children = [subprocess.Popen(command(), env=env, stdout=subprocess.PIPE) for _ in range(2)]
outputs = [None, None]
readers = [threading.Thread(target=lambda index: outputs.__setitem__(index, children[index].communicate(timeout=60)[0]), args=(i,)) for i in range(2)]
for reader in readers:
    reader.start()
peak = 0
while any(child.poll() is None for child in children):
    peak = max(peak, child_count({child.pid for child in children}))
    time.sleep(.01)
for reader in readers:
    reader.join()
concurrent = [json.loads(output) for output in outputs]
check(peak <= 4 and all(child.returncode == 0 for child in children) and
      all(value['errors'] == 0 and value['remaining_children'] == 0 and signatures(value) == expected for value in concurrent),
      'two instances finish without starvation and never exceed four combined parsers')

ready = base / 'idle-ready.txt'
ready.unlink(missing_ok=True)
holder = subprocess.Popen(command(threads=4, rounds=1), env=dict(env,
    PULSE_POOL_BENCH_READY_FILE=str(ready), PULSE_POOL_BENCH_HOLD_MS='3000'), stdout=subprocess.PIPE)
deadline = time.monotonic() + 30
while not ready.exists() and holder.poll() is None and time.monotonic() < deadline:
    time.sleep(.01)
office_manifest = base / 'office-only.txt'
office_manifest.write_text(str(office), encoding='utf-8')
contender = run('idle-contender-office', command(paths=office_manifest, threads=1, rounds=1))
holder_was_alive = holder.poll() is None
idle_result = json.loads(holder.communicate(timeout=10)[0])
check(ready.exists() and ready.read_text() == '4' and holder_was_alive and contender['errors'] == 0 and
      contender['elapsed_us'] < 1000000 and idle_result['remaining_children'] == 0,
      'idle first instance yields slots for second instance Office in under one second')

summary = {'passed': all(checks), 'checks': len(checks), 'combined_peak_children': peak,
           'before': [{key: value for key, value in item.items() if key != 'results'} for item in baseline],
           'after': [{key: value for key, value in item.items() if key != 'results'} for item in pooled],
           'median_before_us': statistics.median(item['elapsed_us'] for item in baseline),
           'median_after_us': statistics.median(item['elapsed_us'] for item in pooled),
           'cancel_elapsed_us': cancelled['elapsed_us'], 'idle_office_us': contender['elapsed_us'],
           'two_pools': two_pools, 'two_instances': concurrent}
(base / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps({key: value for key, value in summary.items() if key not in ['before', 'after', 'two_pools', 'two_instances']}), flush=True)
raise SystemExit(0 if all(checks) else 1)
