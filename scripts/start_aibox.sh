#!/usr/bin/env bash
# AIBox 常驻启动脚本（板上）：core-engine + businessd → 启动完成后浏览器可访问
#
# 用法：
#   ./scripts/start_aibox.sh                # 启动（默认板上真件模式：RtspPuller/MPP/RGA/RKNN 回环拉流）
#   ./scripts/start_aibox.sh --fake         # 启动（fake 仿真模式：合成流+假推理，无摄像头/ZLM 即可出页看框）
#   ./scripts/start_aibox.sh stop           # 停止两个进程
#   ./scripts/start_aibox.sh restart        # 重启
#   ./scripts/start_aibox.sh status         # 状态 + 访问地址
#
# 环境（可覆盖默认）：
#   AIBOX_CONF=<conf 路径>            外部配置（默认自动生成 run/businessd.conf）
#   AIBOX_HTTP_PORT=80  AIBOX_WS_PORT=8081  AIBOX_HOOK_PORT=8090
#   AIBOX_ZLM_HOST=127.0.0.1  AIBOX_ZLM_PORT=80  AIBOX_ZLM_SECRET=xxx（MediaServer config.ini [api]secret）
#   AIBOX_ZLM_RTSP_PORT=554   （MediaServer config.ini [rtsp]port；板上实测 8554 时必须显式传，
#                              否则事件快照 getSnap 回环拉流打错端口、全部"截图失败"）
#   AIBOX_PACKAGES=/data/algorithms   （board 模式算法包目录）
#   AIBOX_DB=/var/lib/aibox/businessd.db
#
# 运行文件（日志/PID）在 run/ 下：tail -f run/engine.log run/biz.log
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
RUN_DIR="$ROOT/run"
mkdir -p "$RUN_DIR"
ENGINE_BIN="$ROOT/build/core-engine"
BIZ_BIN="$ROOT/build/businessd"
PID_E="$RUN_DIR/core-engine.pid"
PID_B="$RUN_DIR/businessd.pid"
LOG_E="$RUN_DIR/engine.log"
LOG_B="$RUN_DIR/biz.log"
UDS="/tmp/aibox-engine.sock"
MODE="board"         # board | fake（--fake 切换）
CMD="start"

for a in "$@"; do
  case "$a" in
    --fake)  MODE="fake" ;;
    --board) MODE="board" ;;
    start|stop|restart|status) CMD="$a" ;;
    *) echo "未知参数: $a（--fake/--board/start/stop/restart/status）"; exit 1 ;;
  esac
done

alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

# ---- 配置：外部传入 > 已存在 > 首次自动生成 ----
CONF="${AIBOX_CONF:-$RUN_DIR/businessd.conf}"
if [ ! -f "$CONF" ]; then
  HTTP_PORT="${AIBOX_HTTP_PORT:-80}"; WS_PORT="${AIBOX_WS_PORT:-8081}"; HOOK_PORT="${AIBOX_HOOK_PORT:-8090}"
  DB_PATH="${AIBOX_DB:-$RUN_DIR/businessd.db}"
  REC_DIR="${AIBOX_RECORDINGS:-/data/recordings}"
  ZLM_HOST="${AIBOX_ZLM_HOST:-127.0.0.1}"
  ZLM_PORT="${AIBOX_ZLM_PORT:-8080}"
  ZLM_SECRET="${AIBOX_ZLM_SECRET:-}"
  ZLM_RTSP_PORT="${AIBOX_ZLM_RTSP_PORT:-8554}"
  cat > "$CONF" <<EOF
{
  "http_port": $HTTP_PORT, "ws_port": $WS_PORT, "hook_port": $HOOK_PORT,
  "bind_host": "0.0.0.0",
  "uds_path": "$UDS",
  "db_path": "$DB_PATH",
  "static_dir": "$ROOT/web",
  "recordings_dir": "$REC_DIR",
  "keep_days": 30,
  "zlm": {"host": "$ZLM_HOST", "port": $ZLM_PORT, "rtsp_port": $ZLM_RTSP_PORT, "secret": "$ZLM_SECRET"}
}
EOF
  mkdir -p "$REC_DIR" 2>/dev/null
  echo "[conf] 已生成默认配置：$CONF（端口 $HTTP_PORT/$WS_PORT/$HOOK_PORT；zlm=$ZLM_HOST:$ZLM_PORT rtsp=$ZLM_RTSP_PORT；recordings=$REC_DIR）"
  [ -z "$ZLM_SECRET" ] && echo "[conf] ★ 提示：未配置 ZLM secret——添加摄像头会 4002。编辑 $CONF 补 zlm.secret"
fi
HTTP_PORT=$(python3 -c "import json;print(json.load(open('$CONF')).get('http_port',80))" 2>/dev/null || echo 80)

# ---- stop / status 不依赖模式 ----
do_stop() {
  for pf in "$PID_B" "$PID_E"; do
    if alive "$pf"; then
      P=$(cat "$pf"); echo "[stop] kill $P（$(basename "$pf" .pid)）"
      kill "$P" 2>/dev/null
    fi
  done
  for _ in $(seq 1 30); do
    alive "$PID_E" || alive "$PID_B" || break
    sleep 0.2
  done
  for pf in "$PID_E" "$PID_B"; do
    if alive "$pf"; then P=$(cat "$pf"); echo "[stop] 强杀 $P"; kill -9 "$P" 2>/dev/null; fi
    rm -f "$pf"
  done
  echo "[stop] 完成"
}

show_status() {
  if alive "$PID_E"; then echo "[status] core-engine 运行中（pid $(cat "$PID_E")，mode 见日志）"
  else echo "[status] core-engine 未运行"; fi
  if alive "$PID_B"; then
    IP=$(hostname -I 2>/dev/null | awk '{print $1}')
    READY=$(curl -s --max-time 2 "http://127.0.0.1:$HTTP_PORT/api/system/info" 2>/dev/null \
            | python3 -c 'import sys,json
try: print(json.load(sys.stdin)["data"]["ready"])
except Exception: print("")' 2>/dev/null)
    echo "[status] businessd 运行中（pid $(cat "$PID_B")，ready=${READY:-未知}）"
    echo "[status] 浏览器访问：http://${IP:-<板IP>}:$HTTP_PORT/"
  else
    echo "[status] businessd 未运行"
  fi
}

# ---- start 前置检查 ----
[ -x "$ENGINE_BIN" ] || { echo "[FAIL] $ENGINE_BIN 不存在（./scripts/build.sh --hw --tests）"; exit 1; }
[ -x "$BIZ_BIN" ]    || { echo "[FAIL] $BIZ_BIN 不存在（./scripts/build.sh --tests）"; exit 1; }
command -v curl >/dev/null || { echo "[FAIL] curl 缺失"; exit 1; }
command -v python3 >/dev/null || { echo "[FAIL] python3 缺失"; exit 1; }

do_start() {
  # 已在运行 → 幂等退出
  if alive "$PID_E" || alive "$PID_B"; then
    echo "[start] 已有实例在运行（幂等退出；如需重启用 restart）"
    show_status
    exit 0
  fi
  # 端口被占 → BLOCK（不擅杀，同 smoke 纪律）
  if ss -ltn 2>/dev/null | grep -q ":$HTTP_PORT "; then
    echo "[BLOCK] 端口 $HTTP_PORT 已被占用：$(ss -ltnp 2>/dev/null | grep ":$HTTP_PORT " | head -1)"
    exit 1
  fi

  # ---- engine ----
  if [ "$MODE" = "fake" ]; then
    VIDEO="${AIBOX_VIDEO:-$RUN_DIR/synthetic.nv12}"
    if [ ! -f "$VIDEO" ]; then
      python3 scripts/gen_test_assets.py --w 640 --h 480 --frames 300 --out-dir "$RUN_DIR" >/dev/null 2>&1 \
        && echo "[start] 已生成合成流 $VIDEO" \
        || { echo "[FAIL] 合成流生成失败（scripts/gen_test_assets.py）"; exit 1; }
    fi
    PKGS="${AIBOX_PACKAGES:-$ROOT/tests/assets/algo_pkgs}"
    [ -d "$PKGS" ] || PKGS=""   # 无包也可启动（算法库空，页面置灰）
    ENGINE_ARGS="--fake --video $VIDEO --frame-w 640 --frame-h 480 --fake-fps 15"
    [ -n "$PKGS" ] && ENGINE_ARGS="$ENGINE_ARGS --packages $PKGS"
  else
    ENGINE_ARGS="--packages ${AIBOX_PACKAGES:-/data/algorithms}"
  fi
  # M4 接 businessd 后开空闲心跳（README M3 注意：无 businessd 时用 0）
  # 日志 > 截断：防上一次启动的 "core-engine up" 残留行导致假就绪
  nohup "$ENGINE_BIN" $ENGINE_ARGS --uds "$UDS" --uds-idle-ms 5000 > "$LOG_E" 2>&1 &
  echo $! > "$PID_E"
  UP=""
  for _ in $(seq 1 100); do
    grep -q "core-engine up" "$LOG_E" 2>/dev/null && UP=1 && break
    alive "$PID_E" || break
    sleep 0.2
  done
  if [ -z "$UP" ]; then
    echo "[FAIL] core-engine 启动失败（tail -5 $LOG_E）："
    tail -5 "$LOG_E" 2>/dev/null
    do_stop
    exit 1
  fi
  echo "[start] core-engine up（mode=$MODE pid $(cat "$PID_E")，UDS=$UDS）"

  # ---- businessd → ready ----
  nohup "$BIZ_BIN" --config "$CONF" > "$LOG_B" 2>&1 &
  echo $! > "$PID_B"
  READY=""
  for _ in $(seq 1 150); do
    READY=$(curl -s --max-time 2 "http://127.0.0.1:$HTTP_PORT/api/system/info" 2>/dev/null \
            | python3 -c 'import sys,json
try: print(json.load(sys.stdin)["data"]["ready"])
except Exception: print("")' 2>/dev/null)
    [ "$READY" = "True" ] && break
    alive "$PID_B" || break
    sleep 0.2
  done
  if [ "$READY" != "True" ]; then
    echo "[FAIL] businessd 未 ready（tail -8 $LOG_B）："
    tail -8 "$LOG_B" 2>/dev/null
    do_stop
    exit 1
  fi
  IP=$(hostname -I 2>/dev/null | awk '{print $1}')
  echo "[start] businessd ready（pid $(cat "$PID_B")）"
  echo ""
  echo "======== AIBox 已就绪 ========"
  echo "  浏览器访问：http://${IP:-<板IP>}:$HTTP_PORT/"
  echo "  日志：tail -f $LOG_E $LOG_B"
  echo "  停止：./scripts/start_aibox.sh stop"
  echo "=============================="
}

case "$CMD" in
  start)   do_start ;;
  stop)    do_stop ;;
  restart) do_stop; do_start ;;
  status)  show_status ;;
esac
