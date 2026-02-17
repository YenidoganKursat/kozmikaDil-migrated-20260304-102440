#!/usr/bin/env python3
import argparse
import json
import math
import os
import shlex
import shutil
import statistics
import subprocess
import tempfile
import time


ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
K_BIN = os.path.join(ROOT_DIR, "k")
PROGRAM_DIR = os.path.join(ROOT_DIR, "bench", "programs", "phase4")
BASELINE_DIR = os.path.join(PROGRAM_DIR, "c_baselines")
RESULTS_DIR = os.path.join(ROOT_DIR, "bench", "results")
WARMUP_ENV_KEY = "SPARK_PHASE4_WARMUP_RUNS"
REPEAT_ENV_KEY = "SPARK_PHASE4_REPEAT"
ADAPTIVE_REPEAT_PROBE_ENV_KEY = "SPARK_PHASE4_ADAPTIVE_PROBE_RUNS"
MAX_ADAPTIVE_REPEAT_ENV_KEY = "SPARK_PHASE4_MAX_ADAPTIVE_REPEAT"
MIN_SAMPLE_TIME_ENV_KEY = "SPARK_PHASE4_MIN_SAMPLE_TIME_SEC"
PIN_CORE_ENV_KEY = "SPARK_PHASE4_PIN_CORE"
REQUIRE_REPRODUCIBLE_ENV_KEY = "SPARK_PHASE4_REQUIRE_REPRODUCIBLE"
STABILITY_PROFILE_ENV_KEY = "SPARK_PHASE4_STABILITY_PROFILE"
NATIVE_COMPILER = os.environ.get("SPARK_CC", os.environ.get("SPARK_CXX", ""))
BASELINE_COMPILER = shutil.which("clang++") or shutil.which("c++") or "c++"
NATIVE_CFLAGS_ENV = os.environ.get(
    "SPARK_CFLAGS",
    os.environ.get(
        "SPARK_CXXFLAGS",
        "-std=c11 -O3 -DNDEBUG -fomit-frame-pointer -fno-stack-protector -fno-plt -fno-fast-math -fno-math-errno",
    ),
)
BASELINE_CXX = shutil.which("clang") or shutil.which("cc") or BASELINE_COMPILER
BASELINE_CFLAGS_ENV = os.environ.get("SPARK_BASELINE_CXXFLAGS", "-std=c11 -O3 -DNDEBUG -fno-fast-math -fno-math-errno")
NATIVE_PROFILE_DEFAULT = "aggressive"
BASELINE_PROFILE_DEFAULT = "portable"

PROFILE_FLAGS = {
    "portable": [],
    "native": ["-march=native"],
    "aggressive": [
        "-march=native",
        "-flto=thin",
        "-fomit-frame-pointer",
        "-fno-stack-protector",
        "-fno-plt",
        "-fstrict-aliasing",
        "-funroll-loops",
        "-falign-functions=32",
        "-falign-loops=16",
    ],
    "max": [
        "-march=native",
        "-flto=thin",
        "-fstrict-aliasing",
        "-fomit-frame-pointer",
        "-fno-stack-protector",
        "-fno-plt",
        "-falign-functions=32",
        "-falign-loops=16",
        "-funroll-loops",
        "-fvectorize",
        "-fslp-vectorize",
        "-mllvm",
        "-inline-threshold=500",
    ],
}


BENCHMARKS_PHASE4 = [
    {"name": "scalar_sum", "source": "scalar_sum.k", "baseline": "scalar_sum.c", "expected": "19999900000"},
    {"name": "scalar_for_range", "source": "scalar_for_range.k", "baseline": "scalar_for_range.c", "expected": "41665416675000"},
    {"name": "scalar_functions", "source": "scalar_functions.k", "baseline": "scalar_functions.c", "expected": "41665416675000"},
    {"name": "scalar_if_else", "source": "scalar_if_else.k", "baseline": "scalar_if_else.c", "expected": "10"},
    {"name": "scalar_f64", "source": "scalar_f64.k", "baseline": "scalar_f64.c", "expected": "1250012500"},
    {"name": "scalar_bool", "source": "scalar_bool.k", "baseline": "scalar_bool.c", "expected": "16666"},
    {"name": "scalar_nested_calls", "source": "scalar_nested_calls.k", "baseline": "scalar_nested_calls.c", "expected": "8"},
    {"name": "scalar_while_if", "source": "scalar_while_if.k", "baseline": "scalar_while_if.c", "expected": "25000"},
    {"name": "scalar_branches", "source": "scalar_branches.k", "baseline": "scalar_branches.c", "expected": "21"},
    {"name": "scalar_matrix_like_guard", "source": "scalar_matrix_like_guard.k", "baseline": "scalar_matrix_like_guard.c", "expected": "18"},
    {"name": "scalar_sum_small", "source": "scalar_sum_small.k", "baseline": "scalar_sum_small.c", "expected": "1249975000"},
    {"name": "scalar_sum_large", "source": "scalar_sum_large.k", "baseline": "scalar_sum_large.c", "expected": "61249825000"},
    {"name": "scalar_for_range_mul", "source": "scalar_for_range_mul.k", "baseline": "scalar_for_range_mul.c", "expected": "134550"},
    {"name": "scalar_for_range_branch", "source": "scalar_for_range_branch.k", "baseline": "scalar_for_range_branch.c", "expected": "625025000"},
    {"name": "scalar_while_decr", "source": "scalar_while_decr.k", "baseline": "scalar_while_decr.c", "expected": "83332"},
    {"name": "scalar_call_chain", "source": "scalar_call_chain.k", "baseline": "scalar_call_chain.c", "expected": "5000100000"},
    {"name": "scalar_while_branch", "source": "scalar_while_branch.k", "baseline": "scalar_while_branch.c", "expected": "149965"},
    {"name": "scalar_if_chain", "source": "scalar_if_chain.k", "baseline": "scalar_if_chain.c", "expected": "149970"},
    {"name": "scalar_axpy_int", "source": "scalar_axpy_int.k", "baseline": "scalar_axpy_int.c", "expected": "7500050000"},
    {"name": "scalar_parity", "source": "scalar_parity.k", "baseline": "scalar_parity.c", "expected": "-1"},
]

BENCHMARKS_PHASE5 = [
    {"name": "list_iteration", "source": "list_iteration.k", "baseline": "bench_list_iteration.c", "expected": "79999800000"},
    {"name": "matrix_elemwise", "source": "matrix_elemwise.k", "baseline": "bench_matrix_elemwise.c", "expected": "157362600"},
]

PHASE_DEFAULTS = {
    "phase4": {
        "benchmarks": BENCHMARKS_PHASE4,
        "program_dir": os.path.join(ROOT_DIR, "bench", "programs", "phase4"),
        "result_basename": "phase4_benchmarks",
    },
    "phase5": {
        "benchmarks": BENCHMARKS_PHASE5,
        "program_dir": os.path.join(ROOT_DIR, "bench", "programs", "phase5"),
        "result_basename": "phase5_benchmarks",
    },
}

ACTIVE_BENCHMARKS = BENCHMARKS_PHASE4
PROGRAM_DIR = PHASE_DEFAULTS["phase4"]["program_dir"]
BASELINE_DIR = os.path.join(PROGRAM_DIR, "c_baselines")
RESULT_BASENAME = PHASE_DEFAULTS["phase4"]["result_basename"]
BENCHMARKS = ACTIVE_BENCHMARKS


def copy_config(config, **overrides):
    cloned = dict(config)
    cloned.update(overrides)
    return cloned


def split_flags(flag_text):
    if not flag_text:
        return []
    return shlex.split(flag_text)


def env_bool(name, default):
    value = os.environ.get(name, "")
    if not value:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on", "y"}


def append_flag_unique(flags, flag):
    if flag not in flags:
        flags.append(flag)


def apply_lto_flags(lto_mode, flags):
    if lto_mode == "thin":
        append_flag_unique(flags, "-flto=thin")
    elif lto_mode == "full":
        append_flag_unique(flags, "-flto")


def apply_pgo_flags(profile, flags, pgo_profile):
    if profile == "instrument":
        append_flag_unique(flags, "-fprofile-instr-generate")
        append_flag_unique(flags, "-fcoverage-mapping")
    elif profile == "use":
        if not pgo_profile:
            raise SystemExit("--pgo-profile is required when --*-pgo=use is set")
        append_flag_unique(flags, f"-fprofile-instr-use={pgo_profile}")


def merge_flags(profile_flags, default_flags):
    merged = []
    seen = set()
    for item in profile_flags + default_flags:
        if item in seen:
            continue
        seen.add(item)
        merged.append(item)
    return merged


def run_once(command, env=None, capture_output=True, repeat=1):
    if repeat <= 0:
        repeat = 1

    start = time.perf_counter()
    stdout_handle = subprocess.PIPE if capture_output else subprocess.DEVNULL
    stderr_handle = subprocess.STDOUT if capture_output else subprocess.DEVNULL

    status = 0
    output = ""
    for rep in range(repeat):
        proc = subprocess.run(
            command,
            stdout=stdout_handle,
            stderr=stderr_handle,
            text=capture_output,
            check=False,
            env=env,
        )
        if rep == 0:
            output = proc.stdout if capture_output else ""
        if status == 0 and proc.returncode != 0:
            status = proc.returncode
            break
    end = time.perf_counter()
    return status, output, end - start


def run_repeated_command(command, env=None, repeat=1, capture_output=True):
    status, output, elapsed = run_once(
        command,
        env=env,
        capture_output=capture_output,
        repeat=repeat,
    )
    return status, output, elapsed


def resolve_run_command(command, pin_core):
    if pin_core is None:
        return command

    taskset = shutil.which("taskset")
    if taskset is None:
        return command

    return [taskset, "-c", str(pin_core)] + command


def resolve_effective_repeat(
    command,
    env,
    requested_repeat,
    min_sample_time_sec,
    adaptive_repeat,
    adaptive_probe_runs=1,
    adaptive_repeat_cap=0,
):
    repeat = max(1, requested_repeat)
    if not adaptive_repeat or min_sample_time_sec <= 0.0:
        return repeat

    if adaptive_probe_runs > 0:
        run_once(command, env=env, capture_output=False, repeat=adaptive_probe_runs)

    probe_status, _, elapsed = run_once(command, env=env, capture_output=False, repeat=repeat)
    if probe_status != 0:
        return repeat
    if elapsed <= 0.0:
        return repeat

    if elapsed >= min_sample_time_sec:
        return repeat

    estimated_per_iter = elapsed / repeat
    if estimated_per_iter <= 0.0:
        return repeat

    needed = int(math.ceil(min_sample_time_sec / estimated_per_iter))
    adjusted = max(repeat, needed)
    if adaptive_repeat_cap > 0:
        return min(adjusted, max(repeat, adaptive_repeat_cap))
    return adjusted


def parse_numeric_token(text):
    if not text:
        return ""
    tokens = text.strip().split()
    for token in reversed(tokens):
        if token:
            if token.lower() in {"true", "false"}:
                return "1" if token.lower() == "true" else "0"
            return token
    return ""


def collect_metrics(runs, drift_limit=3.0):
    if not runs:
        return {
            "sample_count": 0,
            "trimmed_count": 0,
            "mean_time_sec": 0.0,
            "median_time_sec": 0.0,
            "stdev_time_sec": 0.0,
            "min_time_sec": 0.0,
            "max_time_sec": 0.0,
            "trimmed_runs": [],
            "max_drift_percent": 0.0,
            "reproducible": False,
        }

    sorted_times = sorted(runs)
    trimmed = sorted_times[1:-1] if len(sorted_times) > 4 else sorted_times
    if not trimmed:
        trimmed = sorted_times

    mean_time = statistics.fmean(trimmed)
    median_time = statistics.median(trimmed)
    stdev_time = statistics.pstdev(trimmed) if len(trimmed) > 1 else 0.0
    min_time = min(trimmed)
    max_time = max(trimmed)
    drift = [abs((t - median_time) / median_time) * 100.0 for t in trimmed] if median_time > 0 else [0.0]
    max_drift = max(drift) if drift else 0.0

    return {
        "sample_count": len(runs),
        "trimmed_count": len(trimmed),
        "mean_time_sec": mean_time,
        "median_time_sec": median_time,
        "stdev_time_sec": stdev_time,
        "min_time_sec": min_time,
        "max_time_sec": max_time,
        "trimmed_runs": trimmed,
        "max_drift_percent": max_drift,
        "reproducible": bool(max_drift <= drift_limit),
    }


def geometric_mean(values):
    if not values:
        return 0.0
    normalized = [v for v in values if v > 0 and math.isfinite(v)]
    if not normalized:
        return 0.0
    return math.exp(sum(math.log(v) for v in normalized) / len(normalized))


def compare_with_expected(actual_text, expected_text):
    if expected_text == "":
        return False, 0.0, 0.0

    actual_token = parse_numeric_token(actual_text)
    if actual_token == "":
        return False, 0.0, 0.0

    try:
        actual_value = float(actual_token)
    except ValueError:
        return False, 0.0, 0.0

    try:
        expected_value = float(expected_text)
    except ValueError:
        return False, 0.0, 0.0

    eps = 1e-9
    if math.isfinite(expected_value):
        ok = abs(actual_value - expected_value) <= eps * max(1.0, abs(expected_value))
    else:
        ok = actual_value == expected_value

    return ok, actual_value, expected_value


def compile_binary(source, binary_path, is_k_binary=False, config=None):
    config = config or {}
    native_cxx = config.get("native_cxx", "")
    native_profile = config.get("native_profile", NATIVE_PROFILE_DEFAULT)
    native_explicit = split_flags(config.get("native_cflags", ""))
    native_profile_flags = PROFILE_FLAGS.get(native_profile, PROFILE_FLAGS["portable"])
    native_default = split_flags(
        config.get("native_default_cflags", NATIVE_CFLAGS_ENV) if config.get("native_default_cflags", "") else NATIVE_CFLAGS_ENV
    )
    native_cflags = native_explicit if native_explicit else merge_flags(native_profile_flags, native_default)
    native_profile_lto = config.get("native_lto", "off")
    native_profile_pgo = config.get("native_pgo", "off")
    apply_lto_flags(native_profile_lto, native_cflags)
    apply_pgo_flags(native_profile_pgo, native_cflags, config.get("pgo_profile", ""))

    baseline_cxx = config.get("baseline_cxx", BASELINE_COMPILER)
    baseline_profile = config.get("baseline_profile", BASELINE_PROFILE_DEFAULT)
    baseline_explicit = split_flags(config.get("baseline_cflags", BASELINE_CFLAGS_ENV))
    baseline_profile_flags = PROFILE_FLAGS.get(baseline_profile, PROFILE_FLAGS["portable"])
    baseline_default = split_flags(
        config.get("baseline_default_cflags", BASELINE_CFLAGS_ENV)
        if config.get("baseline_default_cflags", "")
        else BASELINE_CFLAGS_ENV
    )
    baseline_cflags = baseline_explicit if baseline_explicit else merge_flags(baseline_profile_flags, baseline_default)
    baseline_profile_lto = config.get("baseline_lto", "off")
    baseline_profile_pgo = config.get("baseline_pgo", "off")
    apply_lto_flags(baseline_profile_lto, baseline_cflags)
    apply_pgo_flags(baseline_profile_pgo, baseline_cflags, config.get("pgo_profile", ""))

    if is_k_binary:
        env = os.environ.copy()
        if native_cxx:
            env["SPARK_CC"] = native_cxx
            env["SPARK_CXX"] = native_cxx
        if native_cflags:
            cflags_text = " ".join(native_cflags)
            env["SPARK_CFLAGS"] = cflags_text
            env["SPARK_CXXFLAGS"] = cflags_text
        if native_profile_lto != "off":
            env["SPARK_LTO"] = native_profile_lto
        else:
            env.pop("SPARK_LTO", None)
        if native_profile_pgo != "off":
            env["SPARK_PGO"] = native_profile_pgo
            if native_profile_pgo == "use":
                env["SPARK_PGO_PROFILE"] = config.get("pgo_profile", "")
            else:
                env.pop("SPARK_PGO_PROFILE", None)
        else:
            env.pop("SPARK_PGO", None)
            env.pop("SPARK_PGO_PROFILE", None)
        command = [K_BIN, "build"]
        if config.get("allow_t5", False):
            command.append("--allow-t5")
        command.extend([source, "-o", binary_path])
        return run_once(command, env=env)

    command = [baseline_cxx, *baseline_cflags, source, "-o", binary_path]
    return run_once(command)


def resolve_llvm_profdata():
    direct = shutil.which("llvm-profdata")
    if direct:
        return direct
    xcrun = shutil.which("xcrun")
    if not xcrun:
        return ""
    status, output, _ = run_once([xcrun, "--find", "llvm-profdata"], capture_output=True)
    if status != 0:
        return ""
    return output.strip()


def prepare_auto_native_pgo(source, benchmark_name, cache_dir, compile_config):
    if not compile_config.get("auto_native_pgo", False):
        return {"enabled": False, "ok": True, "profile": "", "log": ""}

    llvm_profdata = resolve_llvm_profdata()
    if not llvm_profdata:
        return {
            "enabled": True,
            "ok": False,
            "profile": "",
            "log": "auto-pgo: llvm-profdata not found",
        }

    instrument_binary = os.path.join(cache_dir, f"{benchmark_name}.native.instrument")
    instrument_config = copy_config(
        compile_config,
        native_pgo="instrument",
        pgo_profile="",
        auto_native_pgo=False,
    )
    compile_status, compile_output, compile_time = compile_binary(
        source,
        instrument_binary,
        is_k_binary=True,
        config=instrument_config,
    )
    logs = [f"auto-pgo: instrument compile time={compile_time:.6f}s"]
    if compile_output:
        logs.append(compile_output.strip())
    if compile_status != 0 or not os.path.exists(instrument_binary):
        logs.append(f"auto-pgo: instrument compile failed (exit {compile_status})")
        return {
            "enabled": True,
            "ok": False,
            "profile": "",
            "log": "\n".join(logs),
        }

    profile_runs = max(1, int(compile_config.get("auto_native_pgo_runs", 2)))
    profile_raw_files = []
    run_command = resolve_run_command([instrument_binary], compile_config.get("pin_core"))
    for index in range(profile_runs):
        raw_file = os.path.join(cache_dir, f"{benchmark_name}.native.{index}.profraw")
        env = os.environ.copy()
        env["LLVM_PROFILE_FILE"] = raw_file
        run_status, run_output, run_time = run_once(run_command, env=env, capture_output=True, repeat=1)
        logs.append(f"auto-pgo: profile run[{index}] time={run_time:.6f}s")
        if run_output:
            logs.append(run_output.strip())
        if run_status != 0:
            logs.append(f"auto-pgo: profile run[{index}] failed (exit {run_status})")
            return {
                "enabled": True,
                "ok": False,
                "profile": "",
                "log": "\n".join(logs),
            }
        profile_raw_files.append(raw_file)

    profile_data = os.path.join(cache_dir, f"{benchmark_name}.native.profdata")
    merge_command = [llvm_profdata, "merge", "-o", profile_data, *profile_raw_files]
    merge_status, merge_output, merge_time = run_once(merge_command, capture_output=True)
    logs.append(f"auto-pgo: merge time={merge_time:.6f}s")
    if merge_output:
        logs.append(merge_output.strip())
    if merge_status != 0 or not os.path.exists(profile_data):
        logs.append(f"auto-pgo: merge failed (exit {merge_status})")
        return {
            "enabled": True,
            "ok": False,
            "profile": "",
            "log": "\n".join(logs),
        }

    logs.append(f"auto-pgo: profile ready -> {profile_data}")
    return {
        "enabled": True,
        "ok": True,
        "profile": profile_data,
        "log": "\n".join(logs),
    }


def run_mode(
    command_base,
    source,
    runs,
    warmup_runs=0,
    env=None,
    repeat=1,
    drift_limit=3.0,
    min_sample_time_sec=0.0,
    adaptive_repeat=False,
    adaptive_probe_runs=1,
    adaptive_repeat_cap=0,
    pin_core=None,
):
    run_command = command_base + [source]
    run_command = resolve_run_command(run_command, pin_core)
    effective_repeat = resolve_effective_repeat(
        run_command,
        env=env,
        requested_repeat=repeat,
        min_sample_time_sec=min_sample_time_sec,
        adaptive_repeat=adaptive_repeat,
        adaptive_probe_runs=adaptive_probe_runs,
        adaptive_repeat_cap=adaptive_repeat_cap,
    )
    statuses = []
    outputs = []
    run_times = []

    for _ in range(warmup_runs):
        run_repeated_command(run_command, env=env, repeat=effective_repeat, capture_output=False)

    for _ in range(runs):
        status, output, elapsed = run_repeated_command(
            run_command,
            env=env,
            repeat=effective_repeat,
            capture_output=(len(statuses) == 0),
        )
        statuses.append(status)
        outputs.append(output)
        run_times.append(elapsed)

    # use first successful run for output/signature checks
    first_output = outputs[0]
    first_status = statuses[0]
    for index, status in enumerate(statuses):
        if status != 0 and outputs[index].strip():
            first_output = outputs[index]
            first_status = status
            break

    return {
        "status": first_status,
        "first_output": first_output,
        "repeat": effective_repeat,
        "runs": run_times,
        **collect_metrics(run_times, drift_limit=drift_limit),
    }


def run_binary(
    binary_path,
    runs,
    warmup_runs=0,
    env=None,
    repeat=1,
    drift_limit=3.0,
    min_sample_time_sec=0.0,
    adaptive_repeat=False,
    adaptive_probe_runs=1,
    adaptive_repeat_cap=0,
    pin_core=None,
):
    run_command = [binary_path]
    run_command = resolve_run_command(run_command, pin_core)
    effective_repeat = resolve_effective_repeat(
        run_command,
        env=env,
        requested_repeat=repeat,
        min_sample_time_sec=min_sample_time_sec,
        adaptive_repeat=adaptive_repeat,
        adaptive_probe_runs=adaptive_probe_runs,
        adaptive_repeat_cap=adaptive_repeat_cap,
    )
    statuses = []
    outputs = []
    run_times = []

    for _ in range(warmup_runs):
        run_repeated_command(run_command, env=env, repeat=effective_repeat, capture_output=False)

    for _ in range(runs):
        status, output, elapsed = run_repeated_command(
            run_command,
            env=env,
            repeat=effective_repeat,
            capture_output=(len(statuses) == 0),
        )
        statuses.append(status)
        outputs.append(output)
        run_times.append(elapsed)

    first_output = outputs[0]
    first_status = statuses[0]
    for index, status in enumerate(statuses):
        if status != 0 and outputs[index].strip():
            first_output = outputs[index]
            first_status = status
            break

    return {
        "status": first_status,
        "first_output": first_output,
        "repeat": effective_repeat,
        "runs": run_times,
        **collect_metrics(run_times, drift_limit=drift_limit),
    }


def run_program(entry, runs, warmup_runs, repeat, cache_dir, compile_config):
    source = os.path.join(PROGRAM_DIR, entry["source"])
    baseline_source = os.path.join(BASELINE_DIR, entry["baseline"])
    expected = entry["expected"]

    interpret = run_mode(
        [K_BIN, "run", "--interpret"],
        source,
        runs,
        warmup_runs=warmup_runs,
        repeat=repeat,
        drift_limit=compile_config.get("drift_limit", 3.0),
        min_sample_time_sec=compile_config.get("min_sample_time_sec", 0.0),
        adaptive_repeat=compile_config.get("adaptive_repeat", False),
        adaptive_probe_runs=compile_config.get("adaptive_probe_runs", 1),
        adaptive_repeat_cap=compile_config.get("adaptive_repeat_cap", 0),
        pin_core=compile_config.get("pin_core"),
    )

    native_binary = os.path.join(cache_dir, f"{entry['name']}.native")
    native_compile_config = dict(compile_config)
    auto_pgo = prepare_auto_native_pgo(source, entry["name"], cache_dir, compile_config)
    native_compile_logs = []
    if auto_pgo.get("enabled", False):
        native_compile_logs.append(auto_pgo.get("log", ""))
    if auto_pgo.get("ok", False) and auto_pgo.get("profile", ""):
        native_compile_config["native_pgo"] = "use"
        native_compile_config["pgo_profile"] = auto_pgo["profile"]
    elif auto_pgo.get("enabled", False):
        # Fall back to non-PGO native compile when auto profile generation fails.
        native_compile_config["native_pgo"] = "off"
        native_compile_config["pgo_profile"] = ""

    native_compile_status, native_compile_output, native_compile_time = compile_binary(
        source,
        native_binary,
        is_k_binary=True,
        config=native_compile_config,
    )
    if native_compile_output:
        native_compile_logs.append(native_compile_output.strip())
    native = {
        "compile": {
            "status": native_compile_status,
            "output": "\n".join(part for part in native_compile_logs if part),
            "time_sec": native_compile_time,
        }
    }
    if native_compile_status == 0 and os.path.exists(native_binary):
            native.update(
                run_binary(
                    native_binary,
                    runs,
                    warmup_runs=warmup_runs,
                    repeat=repeat,
                    drift_limit=compile_config.get("drift_limit", 3.0),
                    min_sample_time_sec=compile_config.get("min_sample_time_sec", 0.0),
                    adaptive_repeat=compile_config.get("adaptive_repeat", False),
                    adaptive_probe_runs=compile_config.get("adaptive_probe_runs", 1),
                    adaptive_repeat_cap=compile_config.get("adaptive_repeat_cap", 0),
                    pin_core=compile_config.get("pin_core"),
                )
            )
    else:
        native.update(
            {
                "status": native_compile_status,
                "first_output": "",
                "repeat": repeat,
                "runs": [],
                "mean_time_sec": 0.0,
                "median_time_sec": 0.0,
                "stdev_time_sec": 0.0,
                "min_time_sec": 0.0,
                "max_time_sec": 0.0,
                "trimmed_runs": [],
                "max_drift_percent": 0.0,
                "reproducible": False,
            }
        )

    baseline_binary = os.path.join(cache_dir, f"{entry['name']}.baseline")
    baseline_compile_status, baseline_compile_output, baseline_compile_time = compile_binary(
        baseline_source,
        baseline_binary,
        is_k_binary=False,
        config=compile_config,
    )
    baseline = {
        "compile": {
            "status": baseline_compile_status,
            "output": baseline_compile_output,
            "time_sec": baseline_compile_time,
        }
    }
    if baseline_compile_status == 0 and os.path.exists(baseline_binary):
            baseline.update(
                run_binary(
                    baseline_binary,
                    runs,
                    warmup_runs=warmup_runs,
                    repeat=repeat,
                    drift_limit=compile_config.get("drift_limit", 3.0),
                    min_sample_time_sec=compile_config.get("min_sample_time_sec", 0.0),
                    adaptive_repeat=compile_config.get("adaptive_repeat", False),
                    adaptive_probe_runs=compile_config.get("adaptive_probe_runs", 1),
                    adaptive_repeat_cap=compile_config.get("adaptive_repeat_cap", 0),
                    pin_core=compile_config.get("pin_core"),
                )
            )
    else:
        baseline.update(
            {
                "status": baseline_compile_status,
                "first_output": "",
                "repeat": repeat,
                "runs": [],
                "mean_time_sec": 0.0,
                "median_time_sec": 0.0,
                "stdev_time_sec": 0.0,
                "min_time_sec": 0.0,
                "max_time_sec": 0.0,
                "trimmed_runs": [],
                "max_drift_percent": 0.0,
                "reproducible": False,
            }
        )

    interpreted_ok, interpreted_value, expected_value = compare_with_expected(interpret["first_output"], expected)
    native_ok, native_value, _ = compare_with_expected(native["first_output"], expected)
    baseline_ok, baseline_value, _ = compare_with_expected(baseline["first_output"], expected)

    output_match = bool(
        interpret["status"] == 0
        and native["status"] == 0
        and baseline["status"] == 0
        and parse_numeric_token(interpret["first_output"]) == parse_numeric_token(native["first_output"]) == parse_numeric_token(baseline["first_output"])
    )

    outputs_match_with_tolerance = bool(interpreted_ok and native_ok and baseline_ok)

    interpreted_unit_time = interpret["median_time_sec"] / max(1, interpret["repeat"])
    native_unit_time = native["median_time_sec"] / max(1, native["repeat"])
    baseline_unit_time = baseline["median_time_sec"] / max(1, baseline["repeat"])

    speedup = None
    if interpreted_unit_time > 0.0 and native_unit_time > 0.0:
        speedup = interpreted_unit_time / native_unit_time

    speedup_vs_baseline = None
    if baseline_unit_time > 0.0 and native_unit_time > 0.0:
        speedup_vs_baseline = baseline_unit_time / native_unit_time

    band_lower = compile_config.get("band_lower", 0.9)
    band_upper = compile_config.get("band_upper", 1.2)
    within_band_vs_baseline = speedup_vs_baseline is not None and band_lower <= speedup_vs_baseline <= band_upper
    reproducible = bool(
        interpret["reproducible"] and native["reproducible"] and baseline["reproducible"]
    )
    require_reproducible = compile_config.get("require_reproducible", False)

    return {
        "name": entry["name"],
        "source": source,
        "baseline_source": baseline_source,
        "expected": str(expected),
        "expected_value": expected_value,
        "interpreted": interpret,
        "native": native,
        "baseline": baseline,
        "interpreted_value": interpreted_value,
        "native_value": native_value,
        "baseline_value": baseline_value,
        "interpreted_ok": interpreted_ok,
        "native_ok": native_ok,
        "baseline_ok": baseline_ok,
        "output_match": output_match,
        "outputs_match_with_tolerance": outputs_match_with_tolerance,
        "speedup": speedup,
        "speedup_vs_baseline": speedup_vs_baseline,
        "within_band_vs_baseline": within_band_vs_baseline,
        "reproducible_gate": bool(not require_reproducible or reproducible),
        "reproducible": reproducible,
        "pass": bool(
            interpreted_ok
            and native_ok
            and baseline_ok
            and output_match
            and within_band_vs_baseline
            and (not require_reproducible or reproducible)
        ),
        "pass_no_perf_gate": bool(
            interpreted_ok
            and native_ok
            and baseline_ok
            and output_match
        ),
        "pass_no_perf_gate_repro": bool(
            interpreted_ok
            and native_ok
            and baseline_ok
            and output_match
            and (not require_reproducible or reproducible)
        ),
    }


def write_results(results, compile_config):
    os.makedirs(RESULTS_DIR, exist_ok=True)
    output_json = os.path.join(RESULTS_DIR, f"{RESULT_BASENAME}.json")
    output_csv = os.path.join(RESULTS_DIR, f"{RESULT_BASENAME}.csv")

    with open(output_json, "w", encoding="utf-8") as handle:
        json.dump({"config": compile_config, "benchmarks": results}, handle, indent=2)

    with open(output_csv, "w", encoding="utf-8") as handle:
        handle.write(
            "name,pass,pass_no_perf_gate,pass_no_perf_gate_repro,within_band_vs_baseline,expected,"
            "interpreted_median_sec,native_median_sec,baseline_median_sec,"
            "speedup_interp,speedup_vs_baseline,interpreted_value,native_value,"
            "baseline_value,interpreted_reproducible,native_reproducible,"
            "baseline_reproducible,reproducible,reproducible_gate,output_match,interpreted_repeat,"
            "native_repeat,baseline_repeat\n"
        )
        for record in results:
            handle.write(
                ",".join(
                    [
                        record["name"],
                        "PASS" if record["pass"] else "FAIL",
                        "PASS" if record["pass_no_perf_gate"] else "FAIL",
                        "PASS" if record["pass_no_perf_gate_repro"] else "FAIL",
                        str(int(record["within_band_vs_baseline"])),
                        record["expected"],
                        f"{record['interpreted']['median_time_sec']}",
                        f"{record['native']['median_time_sec']}",
                        f"{record['baseline']['median_time_sec']}",
                        f"{record['speedup']}" if record["speedup"] is not None else "",
                        f"{record['speedup_vs_baseline']}" if record["speedup_vs_baseline"] is not None else "",
                        str(record["interpreted_value"]),
                        str(record["native_value"]),
                        str(record["baseline_value"]),
                        str(int(record["interpreted"]["reproducible"])),
                        str(int(record["native"]["reproducible"])),
                        str(int(record["baseline"]["reproducible"])),
                        str(int(record["reproducible"])),
                        str(int(record.get("reproducible_gate", record["reproducible"]))),
                        str(bool(record["output_match"])),
                        str(record["interpreted"]["repeat"]),
                        str(record["native"]["repeat"]),
                        str(record["baseline"]["repeat"]),
                    ]
                )
                + "\n"
            )


def run_tools(runs, warmup_runs, repeat, compile_config):
    results = []

    with tempfile.TemporaryDirectory() as cache_dir:
        for entry in BENCHMARKS:
            source = os.path.join(PROGRAM_DIR, entry["source"])
            if not os.path.exists(source):
                raise FileNotFoundError(source)
            baseline_source = os.path.join(BASELINE_DIR, entry["baseline"])
            if not os.path.exists(baseline_source):
                raise FileNotFoundError(baseline_source)
            results.append(run_program(entry, runs, warmup_runs, repeat, cache_dir, compile_config))

    write_results(results, compile_config)
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--phase",
        type=int,
        choices=[4, 5],
        default=4,
        help="benchmark phase (4 for phase4 scalar, 5 for phase5 containers)",
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=int(os.environ.get("SPARK_PHASE4_RUNS", "7")),
    )
    parser.add_argument(
        "--warmup-runs",
        type=int,
        default=int(os.environ.get(WARMUP_ENV_KEY, "1")),
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=int(os.environ.get(REPEAT_ENV_KEY, "8")),
        help="repeat each timed run N times and sum wall time for one sample",
    )
    parser.add_argument(
        "--min-sample-time-sec",
        type=float,
        default=float(os.environ.get(MIN_SAMPLE_TIME_ENV_KEY, "0.1")),
        help="increase per-sample work so wall time is at least this value when --adaptive-repeat is enabled",
    )
    parser.add_argument(
        "--adaptive-repeat",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="adapt repeat count to reduce timer jitter when a sample is too fast",
    )
    parser.add_argument(
        "--allow-t5",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="allow T5 tier codegen for benchmarked builds (phase5 default: true)",
    )
    parser.add_argument(
        "--adaptive-probe-runs",
        type=int,
        default=int(os.environ.get(ADAPTIVE_REPEAT_PROBE_ENV_KEY, "1")),
        help="number of uncounted warm-up runs used before adaptive repeat probing",
    )
    parser.add_argument(
        "--adaptive-repeat-cap",
        type=int,
        default=int(os.environ.get(MAX_ADAPTIVE_REPEAT_ENV_KEY, "0")),
        help="maximum effective repeat value when adaptive repeat is enabled (0 = unlimited)",
    )
    parser.add_argument(
        "--pin-core",
        type=int,
        default=int(os.environ.get(PIN_CORE_ENV_KEY, "-1")),
        help="pin run commands to a CPU core index via taskset when available",
    )
    parser.add_argument("--native-cxx", default=NATIVE_COMPILER)
    parser.add_argument("--baseline-cxx", default=BASELINE_CXX)
    parser.add_argument("--native-cflags", default="")
    parser.add_argument("--baseline-cflags", default="")
    parser.add_argument(
        "--native-profile",
        default=None,
        choices=list(PROFILE_FLAGS.keys()),
        help="native compile profile (phase4 default: aggressive, phase5 default: aggressive): portable, native, aggressive, max",
    )
    parser.add_argument(
        "--baseline-profile",
        default=BASELINE_PROFILE_DEFAULT,
        choices=list(PROFILE_FLAGS.keys()),
        help="baseline compile profile: portable (default), native, aggressive, max",
    )
    parser.add_argument(
        "--native-lto",
        default=None,
        choices=["off", "thin", "full"],
        help="native LTO mode: off, thin, full",
    )
    parser.add_argument(
        "--baseline-lto",
        default="off",
        choices=["off", "thin", "full"],
        help="baseline LTO mode: off, thin, full",
    )
    parser.add_argument(
        "--native-pgo",
        default="off",
        choices=["off", "instrument", "use"],
        help="native PGO mode: off, instrument, use (requires --pgo-profile)",
    )
    parser.add_argument(
        "--baseline-pgo",
        default="off",
        choices=["off", "instrument", "use"],
        help="baseline PGO mode: off, instrument, use (requires --pgo-profile)",
    )
    parser.add_argument(
        "--pgo-profile",
        default="",
        help="profile path for native/baseline PGO --*-pgo=use",
    )
    parser.add_argument(
        "--auto-native-pgo",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="auto-generate native PGO profile per benchmark (instrument -> run -> merge -> use)",
    )
    parser.add_argument(
        "--auto-native-pgo-runs",
        type=int,
        default=2,
        help="number of profiling runs per benchmark when --auto-native-pgo is enabled",
    )
    parser.add_argument("--show-config", action="store_true", help="print active config summary")
    parser.add_argument(
        "--band-lower",
        type=float,
        default=None,
        help="lower speedup band threshold for native vs baseline",
    )
    parser.add_argument(
        "--band-upper",
        type=float,
        default=None,
        help="upper speedup band threshold for native vs baseline",
    )
    parser.add_argument(
        "--drift-limit",
        type=float,
        default=3.0,
        help="reproducibility drift limit in percent",
    )
    parser.add_argument(
        "--require-reproducible",
        action="store_true",
        default=env_bool(REQUIRE_REPRODUCIBLE_ENV_KEY, False),
        help="fail pass criteria when any bench exceeds drift limit",
    )
    parser.add_argument(
        "--stability-profile",
        default=None,
        choices=["fast", "stable"],
        help="measurement preset: fast (default), stable (higher repeat + longer samples)",
    )
    args = parser.parse_args()

    phase_key = f"phase{args.phase}"
    if phase_key not in PHASE_DEFAULTS:
        raise SystemExit(f"Unknown phase: {args.phase}")
    phase_config = PHASE_DEFAULTS[phase_key]
    PROGRAM_DIR = phase_config["program_dir"]
    BASELINE_DIR = os.path.join(PROGRAM_DIR, "c_baselines")
    ACTIVE_BENCHMARKS = phase_config["benchmarks"]
    BENCHMARKS = ACTIVE_BENCHMARKS
    RESULT_BASENAME = phase_config["result_basename"]

    if args.native_profile is None:
        args.native_profile = "aggressive" if args.phase == 5 else NATIVE_PROFILE_DEFAULT
    if args.native_lto is None:
        args.native_lto = "thin" if args.phase == 5 else "off"
    if args.band_lower is None:
        args.band_lower = 0.85 if args.phase == 5 else 0.9
    if args.band_upper is None:
        args.band_upper = 1.15 if args.phase == 5 else 1.2
    if args.stability_profile is None:
        args.stability_profile = "stable" if args.phase == 5 else os.environ.get(STABILITY_PROFILE_ENV_KEY, "fast")

    if args.native_pgo == "use" and not args.pgo_profile:
        raise SystemExit("native --native-pgo=use requires --pgo-profile")
    if args.baseline_pgo == "use" and not args.pgo_profile:
        raise SystemExit("baseline --baseline-pgo=use requires --pgo-profile")
    if args.auto_native_pgo and args.native_pgo != "off":
        raise SystemExit("--auto-native-pgo requires --native-pgo=off")
    if args.band_lower <= 0.0 or args.band_upper <= 0.0 or args.band_lower >= args.band_upper:
        raise SystemExit("--band-lower must be > 0 and smaller than --band-upper")
    if args.drift_limit < 0.0:
        raise SystemExit("--drift-limit must be >= 0")

    if args.repeat <= 0:
        raise SystemExit("--repeat must be >= 1")
    if args.adaptive_probe_runs < 0:
        raise SystemExit("--adaptive-probe-runs must be >= 0")
    if args.adaptive_repeat_cap < 0:
        raise SystemExit("--adaptive-repeat-cap must be >= 0")
    if args.min_sample_time_sec < 0.0:
        raise SystemExit("--min-sample-time-sec must be >= 0")
    if args.pin_core < -1:
        raise SystemExit("--pin-core must be >= -1")
    if args.auto_native_pgo_runs <= 0:
        raise SystemExit("--auto-native-pgo-runs must be >= 1")
    if args.stability_profile == "stable":
        args.runs = max(args.runs, 9)
        args.repeat = max(args.repeat, 4)
        args.min_sample_time_sec = max(args.min_sample_time_sec, 0.2)
        if args.adaptive_repeat_cap == 0:
            args.adaptive_repeat_cap = 64
        args.adaptive_repeat = True
        args.warmup_runs = max(args.warmup_runs, 1)
        if args.show_config:
            print("stability profile: stable mode enabled")

    if args.stability_profile == "fast" and args.adaptive_repeat and args.min_sample_time_sec < 0.05:
        args.min_sample_time_sec = 0.1

    compile_config = {
        "runs": args.runs,
        "warmup_runs": args.warmup_runs,
        "repeat": args.repeat,
        "native_cxx": args.native_cxx,
        "baseline_cxx": args.baseline_cxx,
        "native_cflags": args.native_cflags,
        "baseline_cflags": args.baseline_cflags,
        "native_default_cflags": NATIVE_CFLAGS_ENV,
        "baseline_default_cflags": BASELINE_CFLAGS_ENV,
        "native_profile": args.native_profile,
        "baseline_profile": args.baseline_profile,
        "native_lto": args.native_lto,
        "baseline_lto": args.baseline_lto,
        "native_pgo": args.native_pgo,
        "baseline_pgo": args.baseline_pgo,
        "pgo_profile": args.pgo_profile,
        "auto_native_pgo": args.auto_native_pgo,
        "auto_native_pgo_runs": args.auto_native_pgo_runs,
        "band_lower": args.band_lower,
        "band_upper": args.band_upper,
        "drift_limit": args.drift_limit,
        "min_sample_time_sec": args.min_sample_time_sec,
        "adaptive_repeat": args.adaptive_repeat,
        "adaptive_probe_runs": args.adaptive_probe_runs,
        "adaptive_repeat_cap": args.adaptive_repeat_cap,
        "pin_core": args.pin_core if args.pin_core >= 0 else None,
        "require_reproducible": args.require_reproducible,
        "allow_t5": args.allow_t5 if args.allow_t5 is not None else (args.phase == 5),
    }

    if args.show_config:
        print("phase4 benchmark config:")
        for key, value in compile_config.items():
            print(f"  {key}: {value}")
    results = run_tools(args.runs, args.warmup_runs, args.repeat, compile_config)
    passed = sum(1 for rec in results if rec["pass"])
    passed_no_perf = sum(1 for rec in results if rec["pass_no_perf_gate"])
    passed_no_perf_repro = sum(1 for rec in results if rec["pass_no_perf_gate_repro"])
    within_band = sum(1 for rec in results if rec["within_band_vs_baseline"])
    reproducible = sum(1 for rec in results if rec["reproducible"])
    reproducible_gate = sum(1 for rec in results if rec["reproducible_gate"])
    native_speedups = [rec["speedup"] for rec in results if rec["speedup"] is not None]
    baseline_speedups = [rec["speedup_vs_baseline"] for rec in results if rec["speedup_vs_baseline"] is not None]
    median_interp = (
        statistics.median(native_speedups)
        if native_speedups
        else 0.0
    )
    median_baseline = (
        statistics.median(baseline_speedups)
        if baseline_speedups
        else 0.0
    )
    min_baseline = min(baseline_speedups) if baseline_speedups else 0.0
    max_baseline = max(baseline_speedups) if baseline_speedups else 0.0
    print(f"phase{args.phase} benchmark pass (with C baseline band): {passed}/{len(results)}")
    print(f"phase{args.phase} benchmark pass (correctness gate only): {passed_no_perf}/{len(results)}")
    print(f"phase{args.phase} benchmark pass (correctness + reproducibility): {passed_no_perf_repro}/{len(results)}")
    print(f"benchmarks within {args.band_lower}x-{args.band_upper}x baseline band: {within_band}/{len(results)}")
    print(f"benchmarks passing reproducibility gate: {reproducible_gate}/{len(results)}")
    print(f"benchmarks fully reproducible (interpreted/native/baseline): {reproducible}/{len(results)}")
    if native_speedups:
        print(f"native vs interpreted speedup: geometric mean={geometric_mean(native_speedups):.3f}x, median={median_interp:.3f}x")
    if baseline_speedups:
        print(
            f"native vs baseline speedup: geometric mean={geometric_mean(baseline_speedups):.3f}x, "
            f"median={median_baseline:.3f}x, range={min_baseline:.3f}x-{max_baseline:.3f}x"
        )
    print(f"results json: {os.path.join(RESULTS_DIR, f'{RESULT_BASENAME}.json')}")
    print(f"results csv: {os.path.join(RESULTS_DIR, f'{RESULT_BASENAME}.csv')}")
