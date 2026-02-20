"""Common process helpers for benchmark runners."""

from __future__ import annotations

import os
import pathlib
import subprocess
from typing import Dict, List


def run_checked(repo_root: pathlib.Path, cmd: List[str], env: Dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    return subprocess.run(
        cmd,
        cwd=str(repo_root),
        capture_output=True,
        text=True,
        check=True,
        env=merged_env,
    )


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("empty output")
    return lines[-1]

