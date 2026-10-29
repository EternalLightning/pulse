from pathlib import Path
import base64
import os
import subprocess
import sys

out = Path(__file__).resolve().parent
root = out.parents[2]
cases = {
    'ui': ('pulse_pane_animation_application_test.exe', '--interaction-feedback-only', 32),
    'edit': ('pulse_child_edit_test.exe', '--motion', 48),
    'probe': ('pulse_cloud_features_test.exe', '--sync-probe-only', 0),
    'metadata': ('pulse_shell_query_test.exe', '--cloud-metadata-only', 0),
}
requested = sys.argv[1:] or list(cases)
failed = False
for name in requested:
    executable, option, expected = cases[name]
    if name == 'ui' and Path('C:/Feedback').exists():
        raise RuntimeError('Synthetic feedback path exists; refusing to read non-fixture content')
    target = out / name
    snapshots = target / 'snapshots'
    snapshots.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['PULSE_TEST_FIXTURE_ROOT'] = str(target)
    env['PULSE_TEST_SNAPSHOT_STDOUT'] = '1'
    result = subprocess.run([str(root / 'build-release' / executable), option],
                            cwd=root, env=env, capture_output=True)
    lines = []
    count = 0
    for line in result.stdout.decode('utf-8', errors='replace').splitlines():
        if line.startswith('SNAPSHOT:'):
            _, filename, data = line.split(':', 2)
        elif line.startswith('PULSE_PNG|'):
            _, filename, data = line.split('|', 2)
        else:
            lines.append(line)
            continue
        if Path(filename).name != filename:
            raise RuntimeError('Unexpected snapshot path')
        (snapshots / filename).write_bytes(base64.b64decode(data, validate=True))
        count += 1
    lines.append(result.stderr.decode('utf-8', errors='replace'))
    lines.append(f'Exit={result.returncode}; PNG frames={count}')
    (target / 'tests.log').write_text('\n'.join(lines), encoding='utf-8')
    print(f'{name}: PASS={sum("[PASS]" in line for line in lines)}, '
          f'FAIL={sum("[FAIL]" in line for line in lines)}, exit={result.returncode}, frames={count}')
    print('\n'.join(line for line in lines if '[FAIL]' in line))
    failed |= result.returncode != 0 or count != expected
sys.exit(1 if failed else 0)
