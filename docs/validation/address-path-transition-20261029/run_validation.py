import base64
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
repo = root.parents[2]
mode = sys.argv[1] if len(sys.argv) > 1 else "--address-path-only"
env = dict(os.environ, PULSE_TEST_FIXTURE_ROOT=str(root), PULSE_TEST_SNAPSHOT_STDOUT="1")
result = subprocess.run([str(repo / "build-release/pulse_pane_animation_application_test.exe"), mode],
                        cwd=repo, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
lines = []
for line in result.stdout.decode("utf-8", errors="replace").splitlines():
    if line.startswith("SNAPSHOT:"):
        _, name, data = line.split(":", 2)
        (root / Path(name).name).write_bytes(base64.b64decode(data))
    else:
        lines.append(line)
log = "\n".join(lines) + f"\nexit_code={result.returncode}\n"
(root / (mode.removeprefix("--") + ".log")).write_text(log, encoding="utf-8")
print(log)
sys.exit(result.returncode)
