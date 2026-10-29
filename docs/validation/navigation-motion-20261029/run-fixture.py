from pathlib import Path
import base64
import os
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
out = Path(__file__).resolve().parent
snapshots = out / 'snapshots'
snapshots.mkdir(exist_ok=True)
env = os.environ.copy()
env['PULSE_TEST_FIXTURE_ROOT'] = str(out)
env['PULSE_TEST_SNAPSHOT_STDOUT'] = '1'
result = subprocess.run([str(root / 'build-release/pulse_pane_animation_application_test.exe'),
                         '--navigation-motion-only'], cwd=root, env=env, capture_output=True)
lines = []
count = 0
for line in result.stdout.decode('utf-8', errors='replace').splitlines():
    if line.startswith('SNAPSHOT:'):
        _, name, data = line.split(':', 2)
        if Path(name).name != name:
            raise RuntimeError('Invalid snapshot filename')
        (snapshots / name).write_bytes(base64.b64decode(data, validate=True))
        count += 1
    else:
        lines.append(line)
lines.append(result.stderr.decode('utf-8', errors='replace'))
lines.append(f'Fixture exit={result.returncode}; PNG frames={count}')
(out / 'tests.log').write_text('\n'.join(lines), encoding='utf-8')
print('\n'.join(lines[-20:]))
sys.exit(result.returncode or (0 if count == 44 else 3))
