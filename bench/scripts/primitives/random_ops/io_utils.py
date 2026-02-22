"""I/O utilities for random operator benchmark scripts."""

from __future__ import annotations

import csv
import os
import pathlib
import subprocess
from typing import Iterable


REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
RESULT_DIR = REPO_ROOT / "bench" / "results" / "primitives"


def run_checked(cmd: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(
        cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
        env=merged,
    )


def parse_last_line(stdout: str) -> str:
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("empty program output")
    return lines[-1]


def parse_filter(raw: str, allowed: Iterable[str]) -> list[str]:
    if not raw:
        return list(allowed)
    selected = [part.strip() for part in raw.split(",") if part.strip()]
    unknown = [part for part in selected if part not in allowed]
    if unknown:
        raise SystemExit(f"unknown filter values: {','.join(unknown)}")
    return selected


def write_csv(path: pathlib.Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

