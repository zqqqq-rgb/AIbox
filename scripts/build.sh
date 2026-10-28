#!/usr/bin/env bash
# AIBox 本地构建脚本（任务分解 T0.1，替代 CI 的本地入口）
# 用法：
#   ./scripts/build.sh              # x86：只编 aibox_core + businessd（纯逻辑可编译）
#   ./scripts/build.sh --hw         # 板上：额外编译 aibox_hw + core-engine
#   ./scripts/build.sh --tests      # 额外编译单元测试
#   ./scripts/build.sh --clean      # 先清空 build 目录再编译
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"

cmake_args=()
for a in "$@"; do
  case "$a" in
    --hw)    cmake_args+=("-DAIBOX_BUILD_HW=ON" "-DAIBOX_BUILD_ENGINE=ON") ;;
    --tests) cmake_args+=("-DAIBOX_BUILD_TESTS=ON") ;;
    --clean) rm -rf "$BUILD_DIR" ;;
    *) echo "未知参数: $a" >&2; exit 1 ;;
  esac
done

cmake -S "$ROOT" -B "$BUILD_DIR" "${cmake_args[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo "[build] OK -> $BUILD_DIR"
