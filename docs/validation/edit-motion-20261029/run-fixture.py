from pathlib import Path
import base64
import subprocess
import sys

out = Path(__file__).resolve().parent
root = out.parents[2]
snapshots = out / 'snapshots'
snapshots.mkdir(exist_ok=True)
result = subprocess.run([str(root / 'build-release/pulse_child_edit_test.exe'), '--motion'],
                        cwd=root, capture_output=True)
lines = []
count = 0
for line in result.stdout.decode('utf-8', errors='replace').splitlines():
    if line.startswith('PULSE_PNG|'):
        _, name, data = line.split('|', 2)
        if Path(name).name != name:
            raise RuntimeError('Invalid snapshot name')
        (snapshots / name).write_bytes(base64.b64decode(data, validate=True))
        count += 1
    else:
        lines.append(line)
lines.append(result.stderr.decode('utf-8', errors='replace'))
lines.append(f'Exit={result.returncode}; PNG frames={count}')
(out / 'tests.log').write_text('\n'.join(lines), encoding='utf-8')
print('\n'.join(lines[-25:]))
sys.exit(result.returncode or (0 if count == 48 else 3))
