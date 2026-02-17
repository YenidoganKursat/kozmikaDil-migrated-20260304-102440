import json
import math
import subprocess
from pathlib import Path
from typing import Dict, Tuple


def run(command, env=None, cwd=None) -> Tuple[int, str]:
    proc = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False, env=env, cwd=cwd)
    return proc.returncode, proc.stdout


def first_float(text: str) -> float:
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        try:
            return float(stripped)
        except ValueError:
            continue
    raise ValueError("no float value found in output")


def is_close(lhs: float, rhs: float, rel_tol=1e-9, abs_tol=1e-9) -> bool:
    return math.isclose(lhs, rhs, rel_tol=rel_tol, abs_tol=abs_tol)


def load_json(path: Path) -> Dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))
