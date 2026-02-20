"""Result IO helpers for mixed signed operator benchmarks."""

from __future__ import annotations

import csv
import json
import pathlib
from typing import Dict, List


def write_csv(path: pathlib.Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    fieldnames = list(rows[0].keys())
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def write_result_files(
    out_dir: pathlib.Path,
    loops: int,
    runs: int,
    warmup: int,
    seed_x: int,
    seed_y: int,
    backend: str,
    rows: List[Dict[str, object]],
) -> tuple[pathlib.Path, pathlib.Path]:
    payload = {
        "loops": loops,
        "runs": runs,
        "warmup": warmup,
        "seed_x": seed_x,
        "seed_y": seed_y,
        "backend": backend,
        "rows": rows,
    }
    json_path = out_dir / "mixed_signed_ops_100m_runtime.json"
    csv_path = out_dir / "mixed_signed_ops_100m_runtime.csv"
    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    write_csv(csv_path, rows)
    return json_path, csv_path

