#!/usr/bin/env bash
# Run the phase-one configuration/recovery/WS regressions, including real businessd children.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-${ROOT}/build}"
if [[ ! -f "${BUILD}/CTestTestfile.cmake" ]]; then
  echo "FAIL: no configured test build at ${BUILD}; build with -DAIBOX_BUILD_TESTS=ON" >&2
  exit 2
fi
inventory="$(ctest --test-dir "${BUILD}" -N -L businessd_phase1)"
if ! [[ "${inventory}" =~ Total\ Tests:\ [1-9][0-9]* ]]; then
  echo "FAIL: no businessd_phase1 tests registered in ${BUILD}" >&2
  exit 2
fi
echo "Phase-one smoke: businessd + SQLite + FakeEngine + MockZlm + TCP WebSocket clients"
ctest --test-dir "${BUILD}" -L businessd_phase1 --output-on-failure
bash "${ROOT}/scripts/check-headers.sh"
echo "SKIP: physical board/RKNN and real camera/ZLM integration (requires target equipment)"
echo "PASS: automated phase-one smoke; see the acceptance matrix for scenarios still uncovered"
