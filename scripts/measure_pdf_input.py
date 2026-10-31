# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Read-only, paired PDF input experiments using frozen benchmark directories."""
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
base = root / 'bench_data/pdf-input-comparison'
paths = (base / 'original-pdfs.txt').read_text(encoding='utf-8').splitlines()
mode = sys.argv[1] if len(sys.argv) > 1 else 'single'
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 1
needle = sys.argv[3] if len(sys.argv) > 3 else ''
selected = [mode] if mode != 'compare' else ['single', 'cache', 'mapped']
results = []
for trial in range(rounds):
    for index, path in enumerate(paths):
        for read_mode in selected[trial % len(selected):] + selected[:trial % len(selected)]:
            directory = base / ('single' if read_mode == 'single' else 'candidate')
            env = dict(os.environ, PULSE_DOCUMENT_ADMISSION_DIR=str(base / 'slots'), PULSE_PDF_READ_MODE=read_mode)
            result = subprocess.run([str(directory / 'pulse_pdf_bench.exe'), 'pdfium', path, needle],
                                    env=env, capture_output=True, timeout=40)
            first, body = result.stdout.split(b'\n', 1)
            value = json.loads(first)
            value.update(mode=read_mode, trial=trial, index=index, path=path,
                         body_sha256=hashlib.sha256(body).hexdigest(), exit_code=result.returncode)
            results.append(value)
            print(json.dumps({key: value[key] for key in ['mode', 'trial', 'index', 'ok', 'error', 'us',
                'pdf_pages', 'pdf_file_reads', 'pdf_file_bytes', 'pdf_file_io_us', 'pdf_reader_us', 'peak_private_bytes']}), flush=True)
            (base / f'{mode}-results.json').write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding='utf-8')
summary = {}
for read_mode in selected:
    data = [item for item in results if item['mode'] == read_mode]
    summary[read_mode] = {'total_us': sum(item['us'] for item in data), 'errors': sum(not item['ok'] for item in data),
        'median_us': statistics.median(item['us'] for item in data),
        'read_bytes': sum(item['pdf_file_bytes'] for item in data),
        'io_us': sum(item['pdf_file_io_us'] for item in data),
        'max_peak_private_bytes': max(item['peak_private_bytes'] for item in data)}
print(json.dumps(summary), flush=True)
equivalent = True
for path in paths:
    group = [item for item in results if item['path'] == path]
    fingerprints = {(item['ok'], item['error'], item['chars'], item['pdf_pages'],
                     item['pdf_fallbacks'], item['body_sha256']) for item in group}
    equivalent = equivalent and len(fingerprints) == 1
print('[PASS] identical full text, page coverage and errors' if equivalent else '[FAIL] extraction differs', flush=True)
raise SystemExit(0 if equivalent else 1)
