#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-0.10.0-rc1}"
OUT_DIR="${ROOT_DIR}/build/release"
STAGE_DIR="${OUT_DIR}/kozmikaDil-${VERSION}"
ARCHIVE="${OUT_DIR}/kozmikaDil-${VERSION}.tar.gz"

mkdir -p "${OUT_DIR}"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/bin" "${STAGE_DIR}/docs"

if [[ ! -x "${ROOT_DIR}/build/compiler/sparkc" ]]; then
  echo "missing build/compiler/sparkc; run cmake --build build first" >&2
  exit 1
fi

cp "${ROOT_DIR}/build/compiler/sparkc" "${STAGE_DIR}/bin/"
cp "${ROOT_DIR}/k" "${STAGE_DIR}/bin/"
cp -R "${ROOT_DIR}/docs"/* "${STAGE_DIR}/docs/"
cp "${ROOT_DIR}/README.md" "${STAGE_DIR}/"
if [[ -f "${ROOT_DIR}/CHANGELOG.md" ]]; then
  cp "${ROOT_DIR}/CHANGELOG.md" "${STAGE_DIR}/"
fi

tar -czf "${ARCHIVE}" -C "${OUT_DIR}" "kozmikaDil-${VERSION}"
echo "release package: ${ARCHIVE}"
