# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Compare frozen text search executables on the same bounded generated corpus."""
import json
import os
from pathlib import Path
import re
import statistics
import subprocess

root = Path(__file__).resolve().parent.parent
output = root / 'build_realtime/text-io-comparison'
output.mkdir(exist_ok=True)
corpus = root / 'bench_data/text-metadata-comparison/files'
if not corpus.exists():
    corpus.mkdir(parents=True)
    for index in range(2048):
        suffix = b' 3d3s marker\n' if index % 17 == 0 else b' absent data\n'
        (corpus / f'{index:04}.txt').write_bytes(b'x' * (8192 - 16) + suffix)
executables = {
    'before': root / 'build_realtime/text-io-before/pulse_content_index_test.exe',
    'after': root / 'build_realtime/text-io-after/pulse_content_index_test.exe',
}
pattern = re.compile(r'TEXT_IO run=(\d+) elapsed_ms=([\d.]+) scanned=(\d+) hits=(\d+) hash=(\d+) bytes=(\d+) error=(\d+)')
results = []
for sequence, mode in enumerate(['before', 'after', 'after', 'before'] * 2):
    timing = output / f'{sequence}-{mode}'
    timing.mkdir(exist_ok=True)
    env = dict(os.environ, PULSE_TEST_TASK_TEXT_BENCH_ROOT=str(corpus),
               PULSE_CONTENT_TIMING_DIR=str(timing))
    completed = subprocess.run([str(executables[mode])], cwd=root, env=env, capture_output=True, timeout=120)
    (timing / 'console.log').write_bytes(completed.stdout + completed.stderr)
    if completed.returncode:
        raise RuntimeError(f'{mode} exited {completed.returncode}; see {timing}')
    matches = pattern.findall(completed.stdout.decode('utf-8', errors='replace'))
    if len(matches) != 3:
        raise RuntimeError(f'Missing benchmark output: {timing}')
    for run, elapsed, scanned, hits, checksum, count, error in matches:
        result = dict(mode=mode, sequence=sequence, run=int(run), elapsed_ms=float(elapsed),
                      scanned=int(scanned), hits=int(hits), hash=int(checksum), bytes=int(count), error=int(error))
        if (result['scanned'], result['hits'], result['hash'], result['bytes'], result['error']) != (
                2048, 121, 11250271013334864258, 16771072, 0):
            raise RuntimeError(f'Result coverage differs: {result}')
        results.append(result)
    print(f'{sequence} {mode}: {[float(item[1]) for item in matches]}', flush=True)
    (output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
summary = {mode: {'median_ms': statistics.median(item['elapsed_ms'] for item in results if item['mode'] == mode),
                  'mean_ms': statistics.mean(item['elapsed_ms'] for item in results if item['mode'] == mode),
                  'count': sum(item['mode'] == mode for item in results)} for mode in executables}
summary['reduction_percent'] = 100 * (1 - summary['after']['median_ms'] / summary['before']['median_ms'])
(output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
print(json.dumps(summary), flush=True)
