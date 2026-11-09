#!/usr/bin/env bash
set -euo pipefail
cd /workspace
cmake -S . -B build-dev -G Ninja -DAIBOX_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-dev -j4
npm ci --prefix web --no-audit --no-fund
python3 tools/dev/demo.py prepare
