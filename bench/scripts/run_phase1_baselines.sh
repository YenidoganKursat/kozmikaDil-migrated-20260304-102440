#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
if command -v ninja >/dev/null 2>&1; then
  BUILD_DIR="$ROOT_DIR/.build_phase1"
else
  BUILD_DIR="$ROOT_DIR/.build_phase1_make"
fi
RESULTS_DIR="$ROOT_DIR/bench/results"
RUNS=7

mkdir -p "$BUILD_DIR" "$RESULTS_DIR"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DSPARK_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target spark_bench_scalar_c spark_bench_list_c spark_bench_matrix_elemwise_c spark_bench_matmul_c

run_with_timer() {
  local exe="$1"
  local out_path="$2"
  local capture_output="$3"
  python3 - "$exe" "$out_path" "$capture_output" <<'PY'
import subprocess
import sys
import time

exe = sys.argv[1]
out_path = sys.argv[2]
capture_output = sys.argv[3] == "1"

start = time.perf_counter()
proc = subprocess.run(
    [exe],
    stdout=subprocess.PIPE if capture_output else subprocess.DEVNULL,
    stderr=subprocess.STDOUT,
    text=True,
    check=False,
)
end = time.perf_counter()

status = proc.returncode
if capture_output and proc.stdout:
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(proc.stdout)

print(f"{end - start:.9f} {status}")
PY
}

run_benchmark() {
  local name="$1"
  local exe="$2"
  local raw_tmp
  raw_tmp="$(mktemp)"

  local run_times=()
  local i

  # First run: capture output for correctness parsing.
  local first_run
  first_run="$(run_with_timer "$exe" "$raw_tmp" 1)"
  local first_status
  read -r _ first_status <<< "$first_run"

  if ! [[ "$first_status" =~ ^[0-9]+$ ]]; then
    first_status=1
  fi

  for i in $(seq 1 "$RUNS"); do
    local t
    t="$(run_with_timer "$exe" /dev/null 0)"
    local tr
    read -r tr _ <<< "$t"
    run_times+=("$tr")
  done

  python3 - "$name" "$exe" "$raw_tmp" "$first_status" "${run_times[@]}" <<'PY'
import json
import statistics
import sys

name = sys.argv[1]
command = sys.argv[2]
output_path = sys.argv[3]
runtimes = [float(v) for v in sys.argv[5:]]
first_status = int(sys.argv[4])

fields = {}
try:
    text = open(output_path, 'r', encoding='utf-8').read()
except OSError:
    text = ''
for line in text.splitlines():
    if '=' in line:
        key, value = line.split('=', 1)
        fields[key.strip()] = value.strip()

passes = fields.get('pass', 'FAIL')
if first_status != 0:
    passes = 'FAIL'
checksum = fields.get('checksum', '0')
expected = fields.get('expected', '0')
metric = 0.0
try:
    metric = float(checksum)
except ValueError:
    metric = 0.0

mean_time = statistics.fmean(runtimes)
trimmed = sorted(runtimes)
if len(trimmed) > 2:
    trimmed = trimmed[1:-1]
mean_time = statistics.fmean(trimmed)
median_time = statistics.median(trimmed)
stdev_time = statistics.pstdev(trimmed) if len(trimmed) > 1 else 0.0
min_time = min(trimmed)
max_time = max(trimmed)
drifts = [((t - median_time) / median_time) * 100.0 for t in trimmed if median_time > 0]
max_drift_percent = max(abs(v) for v in drifts) if drifts else 0.0
repro_ok = max_drift_percent <= 3.0

out = {
    "name": name,
    "command": command,
    "pass": passes,
    "checksum": checksum,
    "expected": expected,
    "metric": metric,
    "runs": [float(t) for t in runtimes],
    "trimmed_runs": [float(t) for t in trimmed],
    "mean_time_sec": mean_time,
    "median_time_sec": median_time,
    "stdev_time_sec": stdev_time,
    "min_time_sec": min_time,
    "max_time_sec": max_time,
    "max_drift_percent": max_drift_percent,
    "reproducible": bool(repro_ok) if first_status == 0 else False,
    "first_status": first_status,
}
print(json.dumps(out))
PY
}

JSON_OUTPUT="$RESULTS_DIR/phase1_raw.json"
printf '{"benchmarks":[\n' > "$JSON_OUTPUT"

results=()
results+=("$(run_benchmark scalar "$BUILD_DIR/bench/benchmarks/spark_bench_scalar_c")")
results+=("$(run_benchmark list "$BUILD_DIR/bench/benchmarks/spark_bench_list_c")")
results+=("$(run_benchmark matrix_elemwise "$BUILD_DIR/bench/benchmarks/spark_bench_matrix_elemwise_c")")
results+=("$(run_benchmark matmul "$BUILD_DIR/bench/benchmarks/spark_bench_matmul_c")")

for i in "${!results[@]}"; do
  sep=','
  if [[ "$i" -eq 0 ]]; then
    sep=''
  fi
  printf '  %s%s\n' "$sep" "${results[$i]}" >> "$JSON_OUTPUT"
done
printf '\n]}\n' >> "$JSON_OUTPUT"

python3 - "$JSON_OUTPUT" <<'PY'
import csv
import json
import sys

json_path = sys.argv[1]
obj = json.load(open(json_path, 'r', encoding='utf-8'))

with open(json_path.replace('.json', '.csv'), 'w', newline='', encoding='utf-8') as f:
    writer = csv.writer(f)
    writer.writerow([
        'name',
        'pass',
        'metric',
        'checksum',
        'expected',
        'mean_time_sec',
        'median_time_sec',
        'stdev_time_sec',
        'min_time_sec',
        'max_time_sec',
        'max_drift_percent',
        'reproducible',
        'command',
    ])
    for b in obj['benchmarks']:
        writer.writerow([
            b['name'],
            b['pass'],
            b['metric'],
            b['checksum'],
            b['expected'],
            b['mean_time_sec'],
            b['median_time_sec'],
            b['stdev_time_sec'],
            b['min_time_sec'],
            b['max_time_sec'],
            b['max_drift_percent'],
            int(bool(b['reproducible'])),
            b['command'],
        ])
print('OK', json_path)
PY

if command -v hyperfine >/dev/null 2>&1; then
  hyperfine --warmup 2 --runs "$RUNS" --export-json "$RESULTS_DIR/hyperfine.json" \
    "$BUILD_DIR/bench/benchmarks/spark_bench_scalar_c" \
    "$BUILD_DIR/bench/benchmarks/spark_bench_list_c" \
    "$BUILD_DIR/bench/benchmarks/spark_bench_matrix_elemwise_c" \
    "$BUILD_DIR/bench/benchmarks/spark_bench_matmul_c"
fi

if command -v perf >/dev/null 2>&1; then
  perf stat -x, -o "$RESULTS_DIR/perf_stats.txt" -r 3 "$BUILD_DIR/bench/benchmarks/spark_bench_scalar_c" || true
fi
