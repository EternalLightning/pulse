# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""A generated multi-window PDF verifies exact input-cache boundary coverage."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
base = root / 'bench_data/pdf-input-comparison'
content = b'BT /F1 20 Tf 20 700 Td (first boundary text) Tj ET\n%'
content += b'x' * (4 * 1024 * 1024 + 65539)
content += b'\nBT /F1 20 Tf 20 600 Td (last boundary text) Tj ET'
objects = [b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
    b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
    b'<< /Length ' + str(len(content)).encode() + b' >>\nstream\n' + content + b'\nendstream']
data = b'%PDF-1.7\n'
offsets = [0]
for index, value in enumerate(objects, 1):
    offsets.append(len(data))
    data += f'{index} 0 obj\n'.encode() + value + b'\nendobj\n'
xref = len(data)
data += f'xref\n0 {len(objects)+1}\n0000000000 65535 f \n'.encode()
for offset in offsets[1:]:
    data += f'{offset:010} 00000 n \n'.encode()
data += f'trailer\n<< /Root 1 0 R /Size {len(objects)+1} >>\nstartxref\n{xref}\n%%EOF\n'.encode()
path = base / 'window-boundary.pdf'
path.write_bytes(data)
results = []
for mode in ['single', 'cache', 'mapped']:
    bench = base / ('single' if mode == 'single' else 'candidate') / 'pulse_pdf_bench.exe'
    env = dict(os.environ, PULSE_DOCUMENT_ADMISSION_DIR=str(base / 'slots'), PULSE_PDF_READ_MODE=mode)
    run = subprocess.run([str(bench), 'pdfium', str(path), ''], env=env, capture_output=True, timeout=10)
    first, body = run.stdout.split(b'\n', 1)
    result = json.loads(first)
    result.update(mode=mode, body_sha256=hashlib.sha256(body).hexdigest())
    result['complete'] = result['ok'] and b'first boundary text' in body and b'last boundary text' in body
    results.append(result)
passed = all(item['complete'] and item['body_sha256'] == results[0]['body_sha256'] for item in results)
passed = passed and results[2]['pdf_map_views'] > 1 and results[2]['pdf_map_fallbacks'] == 0
(base / 'boundary-results.json').write_text(json.dumps({'passed': passed, 'results': results}, indent=2), encoding='utf-8')
print(('[PASS]' if passed else '[FAIL]') + ' multi-block/multi-window PDF retains identical beginning and tail text')
raise SystemExit(0 if passed else 1)
