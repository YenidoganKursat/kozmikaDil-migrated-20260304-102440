"""Shared helpers for single-op window benchmark runners."""

from __future__ import annotations

import os
import pathlib
import subprocess
from typing import Optional


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"


def run_checked(
    cmd: list[str],
    cwd: pathlib.Path,
    env: Optional[dict[str, str]] = None,
) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        env=merged,
        check=True,
        capture_output=True,
        text=True,
    )


def parse_lines(stdout: str) -> list[str]:
    return [line.strip() for line in stdout.splitlines() if line.strip()]

