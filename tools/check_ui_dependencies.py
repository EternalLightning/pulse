#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

"""Check the UI -> app include boundary without compiler or third-party packages.

All literal project includes are followed, including inactive preprocessor branches.
System/SDK includes outside src are ignored. App compatibility headers are allowed
for app consumers, but no source/header under src/ui may reach src/app.
"""

from __future__ import annotations

import argparse
from collections import deque
from pathlib import Path
import re
import sys


INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc", ".cxx", ".inl", ".inc"}


def inside(path: Path, directory: Path) -> bool:
    return path == directory or directory in path.parents


def include_graph(root: Path) -> tuple[dict[Path, list[tuple[Path, int]]], list[str]]:
    src = root / "src"
    graph: dict[Path, list[tuple[Path, int]]] = {}
    unresolved: list[str] = []
    for source in sorted(src.rglob("*")):
        if not source.is_file() or source.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        source = source.resolve()
        text = source.read_text(encoding="utf-8-sig")
        edges: list[tuple[Path, int]] = []
        for match in INCLUDE.finditer(text):
            name = match.group(1).replace("\\", "/")
            candidates = [source.parent / name, src / name, root / name]
            dependency = next((p.resolve() for p in candidates if p.is_file()), None)
            line = text.count("\n", 0, match.start()) + 1
            if dependency is not None and inside(dependency, src):
                edges.append((dependency, line))
            elif dependency is None and (name.startswith(".") or name.startswith("src/")):
                # A missing relative include can hide a forbidden dependency.
                unresolved.append(f"{source.relative_to(root).as_posix()}:{line}: missing {name}")
        graph[source] = edges
    return graph, unresolved


def shortest_app_path(start: Path, graph: dict[Path, list[tuple[Path, int]]], app: Path):
    queue = deque([start])
    previous: dict[Path, tuple[Path, int] | None] = {start: None}
    while queue:
        source = queue.popleft()
        if inside(source, app):
            chain: list[tuple[Path, int, Path]] = []
            while previous[source] is not None:
                parent, line = previous[source]
                chain.append((parent, line, source))
                source = parent
            return list(reversed(chain))
        for dependency, line in graph.get(source, []):
            if dependency not in previous:
                previous[dependency] = (source, line)
                queue.append(dependency)
    return None


def check(root: Path) -> int:
    root = root.resolve()
    ui = root / "src" / "ui"
    app = root / "src" / "app"
    if not ui.is_dir() or not app.is_dir():
        print(f"[FAIL] expected src/ui and src/app below {root}", file=sys.stderr)
        return 2
    graph, unresolved = include_graph(root)
    starts = sorted(source for source in graph if inside(source, ui))
    failures = 0
    for source in starts:
        chain = shortest_app_path(source, graph, app)
        if chain is None:
            continue
        failures += 1
        steps = [f"{parent.relative_to(root).as_posix()}:{line}" for parent, line, _ in chain]
        steps.append(chain[-1][2].relative_to(root).as_posix())
        print("[FAIL] " + " -> ".join(steps))
    # Only report missing project includes reachable from UI, not unrelated app
    # or generated build inputs in modules outside this check's scope.
    reachable = set(starts)
    queue = deque(starts)
    while queue:
        for dependency, _ in graph.get(queue.popleft(), []):
            if dependency not in reachable:
                reachable.add(dependency)
                queue.append(dependency)
    for diagnostic in unresolved:
        relative = diagnostic.split(":", 1)[0]
        if (root / relative).resolve() in reachable:
            failures += 1
            print("[FAIL] " + diagnostic)
    if failures:
        print(f"[FAIL] UI dependency boundary: {failures} violations")
        return 1
    print(f"[PASS] {len(starts)} UI files; no direct or transitive includes into src/app")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    return check(parser.parse_args().root)


if __name__ == "__main__":
    raise SystemExit(main())
