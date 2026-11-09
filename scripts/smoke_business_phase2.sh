#!/usr/bin/env bash
# V2 drafts, durable activation/recovery and the fake-engine/businessd event pipeline.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-${ROOT}/build}"
if [[ ! -f "${BUILD}/CTestTestfile.cmake" ]]; then
  echo "FAIL: configure and build with -DAIBOX_BUILD_TESTS=ON first" >&2
  exit 2
fi
inventory="$(ctest --test-dir "${BUILD}" -N -L businessd_phase2)"
if ! [[ "${inventory}" =~ Total\ Tests:\ [1-9][0-9]* ]]; then
  echo "FAIL: no businessd_phase2 tests registered" >&2
  exit 2
fi
ctest --test-dir "${BUILD}" -L businessd_phase2 --output-on-failure
bash "${ROOT}/scripts/check-headers.sh"
echo "PASS: V2 drafts, scheduled multi-camera activation, crash recovery and source events"
echo "SKIP: board integration and real media/model verification"
