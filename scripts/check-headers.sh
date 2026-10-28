#!/usr/bin/env bash
# 门禁：aibox_core 纯逻辑层禁止 include 任何硬件头文件（任务分解 §1.2）
#   include rknn_api.h / mpp / rga 即判失败（对应 CI 红）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# aibox_core 的全部源码目录（纯逻辑层；hw/ 除外）
CORE_DIRS="${ROOT}/engine/core ${ROOT}/engine/common ${ROOT}/engine/media ${ROOT}/engine/ipc ${ROOT}/engine/infer ${ROOT}/engine/model ${ROOT}/engine/rules ${ROOT}/engine/arbiter ${ROOT}/engine/track"

# 硬件头文件特征：rknn_api.h、rockchip 下的 mpp/rga、rga.h / im2d.h
pattern='#include[[:space:]]*[<"](rknn_api\.h|rockchip/[a-z_]*(mpp|rga)[a-z_]*\.h|rga/|im2d\.h|rga\.h)'

violations="$(grep -rnE "$pattern" $CORE_DIRS || true)"

if [[ -n "$violations" ]]; then
  echo "[check-headers] FAIL —— aibox_core 检测到硬件头文件:" >&2
  echo "$violations" >&2
  exit 1
fi

echo "[check-headers] OK —— aibox_core 无硬件头文件"
