#!/usr/bin/env python3
"""Entry-point wrapper for mixed signed primitive operator benchmark.

The implementation is intentionally split into task-based modules under
`bench/scripts/primitives/mixed_ops/` to keep each file focused and short.
"""

from __future__ import annotations

import pathlib
import sys


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from mixed_ops.cli import run_cli


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    return run_cli(repo_root=repo_root)


if __name__ == "__main__":
    raise SystemExit(main())

