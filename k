#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

declare -a CANDIDATES=(
  "${SCRIPT_DIR}/build_local_ci/compiler/sparkc"
  "${SCRIPT_DIR}/build/compiler/sparkc"
  "${SCRIPT_DIR}/build_local_full/compiler/sparkc"
  "${SCRIPT_DIR}/build_local/compiler/sparkc"
  "${SCRIPT_DIR}/build_host/compiler/sparkc"
)

SELECTED=""
for candidate in "${CANDIDATES[@]}"; do
  if [[ -x "${candidate}" ]]; then
    if [[ -z "${SELECTED}" || "${candidate}" -nt "${SELECTED}" ]]; then
      SELECTED="${candidate}"
    fi
  fi
done

if [[ -z "${SELECTED}" ]]; then
  echo "No sparkc binary found. Build first (e.g. cmake --build build -j)." >&2
  exit 1
fi

if [[ "${1:-}" == "run" && "${2:-}" == "--interpret" ]]; then
  shift 2
  exec "${SELECTED}" run "$@"
fi

if [[ "${1:-}" == "build" ]]; then
  if [[ $# -lt 2 ]]; then
    echo "usage: ./k build <file.k> [-o output.bin] [--profile <name>]" >&2
    exit 2
  fi

  shift
  SOURCE="${1:-}"
  shift || true

  OUTPUT=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -o|--output)
        OUTPUT="${2:-}"
        shift 2
        ;;
      --profile)
        shift 2
        ;;
      *)
        shift
        ;;
    esac
  done

  if [[ -z "${SOURCE}" ]]; then
    echo "build requires source file" >&2
    exit 2
  fi

  if [[ -z "${OUTPUT}" ]]; then
    OUTPUT="${SOURCE%.*}.bin"
  fi

  TMP_C="$(mktemp -t kozmika_build_XXXXXX.c)"
  trap 'rm -f "${TMP_C}"' EXIT

  "${SELECTED}" compile "${SOURCE}" --emit-c --emit-c-out "${TMP_C}"

  DEFAULT_CFLAGS="-O3 -DNDEBUG"
  CFLAGS_RAW="${SPARK_CFLAGS:-${DEFAULT_CFLAGS}}"
  LDFLAGS_RAW="${SPARK_LDFLAGS:-}"
  CFLAGS_ARR=()
  LDFLAGS_ARR=()
  if [[ -n "${CFLAGS_RAW}" ]]; then
    read -r -a CFLAGS_ARR <<< "${CFLAGS_RAW}"
  fi
  if [[ -n "${LDFLAGS_RAW}" ]]; then
    read -r -a LDFLAGS_ARR <<< "${LDFLAGS_RAW}"
  fi

  cc ${CFLAGS_ARR[@]+"${CFLAGS_ARR[@]}"} -x c "${TMP_C}" -o "${OUTPUT}" ${LDFLAGS_ARR[@]+"${LDFLAGS_ARR[@]}"} -lm
  trap - EXIT
  rm -f "${TMP_C}"
  exit 0
fi

exec "${SELECTED}" "$@"
