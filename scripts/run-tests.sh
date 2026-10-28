#!/usr/bin/env bash
# 本地单测入口（任务分解 T0.1 占位，T1.x 起有实质用例）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
  echo "[run-tests] 尚未 configure，请先执行 ./scripts/build.sh --tests" >&2
  exit 1
fi

ctest --test-dir "$BUILD_DIR" --output-on-failure
