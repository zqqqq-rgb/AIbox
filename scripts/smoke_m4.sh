#!/usr/bin/env bash
# M4 三进程联调冒烟（板上）：core-engine + businessd（+ 手动启动的 ZLM）
#
# Tier A（无 ZLM 可跑）：启动/UDS 同步/只读 API/DB 重同步 add_cam/WS detections/
#                       engine 重启全量重建（engine --fake 合成流）
# Tier B（ZLM 已手动启动）：POST/PUT/DELETE 摄像头全链路（ZLM 代理+预检+逻辑删）
# Tier C（ZLM+ffmpeg）：真件全链路——ffmpeg 推合成流→ZLM→engine 真件模式
#                       （RtspPuller/MPP/RGA/RKNN）回环拉流→WS 出框
#
# 用法：
#   ./scripts/smoke_m4.sh                    # Tier A
#   # ZLM 手动启动后重跑即得 Tier B/C（secret 须匹配，或 SMOKE_ZLM_SECRET 传入）：
#   cd /root/aibox/ZLMediaKit/release/linux/Release && ./MediaServer -d &
# 前置：./scripts/build.sh --hw --tests；端口 18080/18081/18090 空闲；
#       无残留 core-engine/businessd 进程（脚本启动前检测并 BLOCK）。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
WORK="$(mktemp -d /tmp/aibox_smoke.XXXXXX)"
ENGINE_BIN="$ROOT/build/core-engine"
BIZ_BIN="$ROOT/build/businessd"
HTTP_PORT=18080; WS_PORT=18081; HOOK_PORT=18090
ZLM_PORT="${SMOKE_ZLM_PORT:-8080}"
ZLM_RTSP_PORT="${SMOKE_ZLM_RTSP_PORT:-8554}"  # Tier C 推流/回环用（须与 ZLM config [rtsp] port 一致）
# 板上实测基线（topeet）：secret；可用 SMOKE_ZLM_SECRET 覆盖。
# ZLM 由使用者手动启动（老版 MediaServer 固定读同目录 config.ini，脚本不代拉起）
ZLM_SECRET="${SMOKE_ZLM_SECRET:-Q8BeLYbhdxhBeym08U0OZar2KQbpSVgN}"
PASS=0; FAIL=0
ENGINE_PID=""; BIZ_PID=""; FFMPEG_PID=""

say() { printf '%s\n' "$*"; }
ok()  { PASS=$((PASS+1)); say "[PASS] $*"; }
bad() { FAIL=$((FAIL+1)); say "[FAIL] $*"; }
# JSON 取值：curl -s URL | jget 'py表达式'（d=json 根）
jget() { python3 -c 'import sys,json
try:
    print(eval(sys.argv[1], {"d": json.load(sys.stdin)}))
except Exception:
    print("")' "$1"; }

cleanup() {
  if [ "${SMOKE_KEEP:-0}" = "1" ]; then
    # 保留模式：进程与目录都不动，供板上交互排查（人工清理）
    say "[KEEP] 现场保留：WORK=$WORK（engine=$ENGINE_PID biz=$BIZ_PID ffmpeg=$FFMPEG_PID）"
    say "       复现：curl -sv -X PUT http://127.0.0.1:$HTTP_PORT/api/cameras/<id>/algos -H 'Content-Type: application/json' -d '{\"algos\":[{\"algo_id\":\"person-det\",\"fps\":3}]}'"
    say "       日志：$WORK/biz.log / $WORK/engine.log；清场：kill $ENGINE_PID $BIZ_PID $FFMPEG_PID 2>/dev/null; rm -rf $WORK"
    return
  fi
  [ -n "$FFMPEG_PID" ] && kill "$FFMPEG_PID" 2>/dev/null
  [ -n "$BIZ_PID" ] && kill "$BIZ_PID" 2>/dev/null
  [ -n "$ENGINE_PID" ] && kill "$ENGINE_PID" 2>/dev/null
  wait 2>/dev/null
  # ZLM 代理清理：businessd 已死无法走 API——直接 delStreamProxy 本次创建的
  # 代理（临时 DB 自增 id 固定为 1/2/3 → cam1~cam3 的 main/sub 共 6 个 key）。
  # 不清理则 retry_count=-1 的孤儿代理会在 ZLM 里无限重试死流（板上实测）。
  if [ -n "$ZLM_OK" ]; then
    for id in 1 2 3; do
      for suf in main sub; do
        curl -s --max-time 3 \
          "http://127.0.0.1:$ZLM_PORT/index/api/delStreamProxy?secret=$ZLM_SECRET&key=__defaultVhost__/live/cam${id}_${suf}" \
          >/dev/null 2>&1
      done
    done
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# ---- 0. 依赖与产物 ----
[ -x "$ENGINE_BIN" ] || { say "[FAIL] core-engine 不存在（./scripts/build.sh --hw --tests）"; exit 1; }
[ -x "$BIZ_BIN" ]    || { say "[FAIL] businessd 不存在（./scripts/build.sh --tests）"; exit 1; }
command -v python3 >/dev/null || { say "[FAIL] python3 缺失"; exit 1; }
command -v curl >/dev/null    || { say "[FAIL] curl 缺失"; exit 1; }

# 前置环境卫生：残留进程/端口占用会让冒烟出假 FAIL——只报告不擅杀
# （板上可能跑着生产实例，自动 kill 太危险；按提示人工处理后重跑）
DIRTY=0
for pat in "core-engine" "businessd" "ffmpeg.*aibox_smoke"; do
  PIDS=$(pgrep -f "$pat" 2>/dev/null || true)
  if [ -n "$PIDS" ]; then
    say "[BLOCK] 检测到残留进程 [$pat]: $PIDS"
    say "        处理：kill $PIDS  （确认非生产实例后再执行）"
    DIRTY=1
  fi
done
for port in $HTTP_PORT $WS_PORT $HOOK_PORT; do
  if ss -ltn 2>/dev/null | grep -q ":$port "; then
    say "[BLOCK] 端口 $port 已被占用：$(ss -ltnp 2>/dev/null | grep ":$port " | head -1)"
    DIRTY=1
  fi
done
[ "$DIRTY" = "1" ] && exit 1

python3 scripts/gen_test_assets.py --w 640 --h 480 --frames 300 --out-dir "$WORK" >/dev/null 2>&1 \
  && ok "NV12 测试流生成（640x480x300）" || { bad "NV12 生成"; exit 1; }

cat > "$WORK/businessd.conf" <<EOF
{
  "http_port": $HTTP_PORT, "ws_port": $WS_PORT, "hook_port": $HOOK_PORT,
  "bind_host": "127.0.0.1",
  "uds_path": "$WORK/engine.sock",
  "db_path": "$WORK/biz.db",
  "keep_days": 0,
  "zlm": {"host": "127.0.0.1", "port": $ZLM_PORT, "secret": "$ZLM_SECRET"}
}
EOF
B="http://127.0.0.1:$HTTP_PORT"

# ---- 1. engine 启动（fake 模式）----
"$ENGINE_BIN" --fake --video "$WORK/synthetic.nv12" --frame-w 640 --frame-h 480 \
  --fake-fps 15 --packages "$ROOT/tests/assets/algo_pkgs" --uds "$WORK/engine.sock" \
  > "$WORK/engine.log" 2>&1 &
ENGINE_PID=$!
for _ in $(seq 1 40); do grep -q "core-engine up" "$WORK/engine.log" 2>/dev/null && break; sleep 0.2; done
grep -q "core-engine up" "$WORK/engine.log" \
  && ok "engine 启动（fake，UDS=$WORK/engine.sock）" \
  || { bad "engine 启动"; tail -5 "$WORK/engine.log"; exit 1; }

# ---- 2. businessd 启动 → ready ----
"$BIZ_BIN" --config "$WORK/businessd.conf" > "$WORK/biz.log" 2>&1 &
BIZ_PID=$!
READY=""
for _ in $(seq 1 60); do
  READY=$(curl -s --max-time 2 "$B/api/system/info" 2>/dev/null | jget 'd["data"]["ready"]')
  [ "$READY" = "True" ] && break
  sleep 0.2
done
[ "$READY" = "True" ] && ok "businessd ready（/api/system/info）" \
  || { bad "businessd ready"; tail -8 "$WORK/biz.log"; exit 1; }
grep -q "state SYNCING -> ONLINE" "$WORK/biz.log" \
  && ok "UDS 状态机 SYNCING→ONLINE（空库）" || bad "UDS 状态机（见 biz.log）"

# ---- 3. 只读 API ----
NALGO=$(curl -s "$B/api/algorithms" | jget 'len([x for x in d["data"] if not x["disabled"]])')
[ "${NALGO:-0}" -ge 1 ] 2>/dev/null && ok "registry_ready→algorithms 表（可用=$NALGO）" \
  || bad "registry 同步（可用=$NALGO）"
ON=$(curl -s "$B/api/resources" | jget 'd["data"]["engine_online"]')
[ "$ON" = "True" ] && ok "get_metrics 透传（engine_online=true）" || bad "get_metrics 透传"

# ---- 4. DB 直插摄像头（无 ZLM 的 add 路径）→ 重启 businessd → SYNCING 全量同步 ----
python3 - "$WORK/biz.db" <<'PYEOF' >/dev/null 2>&1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT INTO cameras(name, rtsp_main, rtsp_sub, enabled) "
           "VALUES('smoke','rtsp://smoke/main','rtsp://smoke/sub',1)")
cid = db.execute("SELECT id FROM cameras LIMIT 1").fetchone()[0]
db.execute("INSERT INTO camera_algos(camera_id, algo_id, threshold, detect_fps, enabled) "
           "VALUES(?,'person-det',0.45,3,1)", (cid,))
db.commit()
PYEOF
kill "$BIZ_PID"; wait "$BIZ_PID" 2>/dev/null; BIZ_PID=""
"$BIZ_BIN" --config "$WORK/businessd.conf" >> "$WORK/biz.log" 2>&1 &
BIZ_PID=$!
SYNCED=""
for _ in $(seq 1 60); do
  grep -q "add ok/fail=1/0" "$WORK/biz.log" 2>/dev/null && SYNCED=1 && break
  sleep 0.2
done
[ -n "$SYNCED" ] && ok "SYNCING 全量同步（DB→engine add_cam ok=1）" \
  || { bad "SYNCING 全量同步"; tail -8 "$WORK/biz.log"; }
CNT=$(curl -s "$B/api/cameras" | jget 'len(d["data"])')
[ "$CNT" = "1" ] && ok "/api/cameras 列表（1 台）" || bad "/api/cameras 列表（n=$CNT）"
ONC=""
for _ in $(seq 1 60); do
  ONC=$(curl -s "$B/api/cameras" | jget 'd["data"][0]["online"]')
  [ "$ONC" = "True" ] && break
  sleep 0.2
done
[ "$ONC" = "True" ] && ok "camera_status 落库（online=true）" || bad "camera_status 落库"

# ---- 5. WS detections 探针 ----
CAM_ID=$(curl -s "$B/api/cameras" | jget 'd["data"][0]["id"]')
if python3 scripts/ws_probe.py --port "$WS_PORT" --path "/ws/meta/$CAM_ID" \
     --seconds 6 --min 3; then
  ok "WS /ws/meta/$CAM_ID detections 流（≥3 条/6s）"
else
  bad "WS detections 流（probe 输出见上）"
fi

# ---- 6. stream 信令 ----
SIG=$(curl -s "$B/api/cameras/$CAM_ID/stream?quality=sub" | jget 'd["data"]["signaling"]')
case "$SIG" in
  *"/index/api/webrtc"*) ok "stream 信令 URL（WebRTC）" ;;
  *) bad "stream 信令 URL（$SIG）" ;;
esac

# ---- 7. engine 重启 → businessd 全量重建 ----
DISC0=$(grep -c "state ONLINE -> DISCONNECTED" "$WORK/biz.log" 2>/dev/null || true)
kill "$ENGINE_PID"; wait "$ENGINE_PID" 2>/dev/null; ENGINE_PID=""
DETECTED=""
for _ in $(seq 1 60); do
  DISC1=$(grep -c "state ONLINE -> DISCONNECTED" "$WORK/biz.log" 2>/dev/null || true)
  [ "${DISC1:-0}" -gt "${DISC0:-0}" ] 2>/dev/null && DETECTED=1 && break
  sleep 0.2
done
[ -n "$DETECTED" ] && ok "engine 下线感知（心跳/EPOLLHUP→DISCONNECTED）" || bad "engine 下线感知"
"$ENGINE_BIN" --fake --video "$WORK/synthetic.nv12" --frame-w 640 --frame-h 480 \
  --fake-fps 15 --packages "$ROOT/tests/assets/algo_pkgs" --uds "$WORK/engine.sock" \
  >> "$WORK/engine.log" 2>&1 &
ENGINE_PID=$!
REBUILT=""
for _ in $(seq 1 80); do
  C=$(grep -c "add ok/fail=1/0" "$WORK/biz.log" 2>/dev/null || true)
  [ "${C:-0}" -ge 2 ] 2>/dev/null && REBUILT=1 && break
  sleep 0.2
done
[ -n "$REBUILT" ] && ok "engine 重启 → 全量重建（第二次 add ok=1）" \
  || { bad "engine 重启全量重建"; tail -8 "$WORK/biz.log"; }

# ---- 8. Tier B：ZLM 可用则全链路 ----
# ZLM 由使用者手动启动（板上老版 MediaServer 固定读同目录 config.ini，不支持
# -c 注入——脚本不代拉起，避免 config 覆盖生产文件的风险）。
# 探测失败 → Tier B/C SKIP 并提示启动方法。
zlm_alive() {
  # getServerConfig 应答大（板上实测 >2s 掐断风险）：超时放宽到 6s，只取头部判 code
  curl -s --max-time 6 \
    "http://127.0.0.1:$ZLM_PORT/index/api/getServerConfig?secret=$ZLM_SECRET" \
    | head -c 4096 | grep -q '"code" : 0\|"code":0'
}
ZLM_OK=""
if zlm_alive; then
  ZLM_OK=1
  ok "ZLM 预存实例复用（:$ZLM_PORT）"
else
  say "[SKIP] Tier B/C：ZLM 不可用（:$ZLM_PORT）。手动启动方法："
  say "  cd /root/aibox/ZLMediaKit/release/linux/Release && ./MediaServer -d &"
  say "  （secret 须为 $ZLM_SECRET，或用 SMOKE_ZLM_SECRET 传入实际值）"
fi

# ---- 8.5 Tier B 前置：真实码流准备（ZLM+ffmpeg 可用时）----
# 教训（板上实证）：ZLM addStreamProxy 对不可达源**立即返回 code=-1**（非阻塞
# 探测失败，31ms）——冒烟必须用真实可达的流，否则编排必失败且属脚本错误而非
# 产品缺陷。ffmpeg 推合成流→live/trtsp，B/C 两档共用。
if [ -n "$ZLM_OK" ] && command -v ffmpeg >/dev/null 2>&1; then
  FFMPEG_OK=0
  if ffmpeg -y -loglevel error -f rawvideo -pix_fmt nv12 -s 640x480 -r 15 \
      -i "$WORK/synthetic.nv12" -c:v libx264 -preset ultrafast -tune zerolatency \
      -pix_fmt yuv420p -g 30 -b:v 1M -f h264 "$WORK/test.h264" 2>"$WORK/ffmpeg_err"; then
    for _ in $(seq 1 40); do cat "$WORK/test.h264"; done > "$WORK/loop.h264"
    FFMPEG_OK=1
    ok "Tier C: 合成流转码 h264（640x480，~13min 时长等效）"
  else
    bad "Tier C: ffmpeg 转码失败（$(tail -2 "$WORK/ffmpeg_err" 2>/dev/null)）"
  fi
  if [ "$FFMPEG_OK" = "1" ]; then
    # 推流到 ZLM（RTSP 推流默认无鉴权）；-re 实时节拍 + 无限循环保流不断
    ffmpeg -loglevel error -re -stream_loop -1 -i "$WORK/loop.h264" \
      -c copy -f rtsp "rtsp://127.0.0.1:$ZLM_RTSP_PORT/live/trtsp" > "$WORK/ffpush.log" 2>&1 &
    FFMPEG_PID=$!
    STREAMED=""
    for _ in $(seq 1 50); do
      # 缩进式/紧凑式 JSON 双格式兼容（板上 ZLM 为缩进式："stream" : "trtsp"）
      curl -s --max-time 6 "http://127.0.0.1:$ZLM_PORT/index/api/getMediaList?secret=$ZLM_SECRET" \
        | grep -q '"stream" : "trtsp"\|"stream":"trtsp"' && STREAMED=1 && break
      sleep 0.2
    done
    [ -n "$STREAMED" ] && ok "Tier C: ffmpeg 推流已注册（live/trtsp）" \
      || { bad "Tier C: ffmpeg 推流未注册（$(tail -3 "$WORK/ffpush.log" 2>/dev/null)）"; }
  fi
  REAL_URL="rtsp://127.0.0.1:$ZLM_RTSP_PORT/live/trtsp"
else
  say "[SKIP] Tier C：需 ZLM（手动启动）+ 板上 ffmpeg"
fi

# ---- 9. Tier B：摄像头 REST 全链路（用真实可达流；无流则 SKIP）----
if [ -n "$ZLM_OK" ] && [ "${STREAMED:-}" = "1" ]; then
  R=$(curl -s --max-time 8 -X POST "$B/api/cameras" -H "Content-Type: application/json" \
      -d '{"name":"t2","rtsp_main":"'"$REAL_URL"'","rtsp_sub":"'"$REAL_URL"'"}')
  NEW_ID=$(printf '%s' "$R" | jget 'd["data"]["id"]')
  if [ -n "$NEW_ID" ] && [ "$NEW_ID" != "" ]; then
    ok "Tier B: POST /api/cameras（id=$NEW_ID：ZLM 代理+DB+engine）"
  else
    bad "Tier B: POST /api/cameras（$R）"
  fi
  R=$(curl -s --max-time 8 -X PUT "$B/api/cameras/$NEW_ID/algos" \
      -H "Content-Type: application/json" \
      -d '{"algos":[{"algo_id":"person-det","fps":3,"threshold":0.5}]}')
  printf '%s' "$R" | grep -q "npu_est" && ok "Tier B: PUT algos（预检+npu_est）" \
    || bad "Tier B: PUT algos（$R）"
  R=$(curl -s --max-time 8 -X DELETE "$B/api/cameras/$NEW_ID")
  printf '%s' "$R" | grep -q '"code" : 0\|"code":0' && ok "Tier B: DELETE（逻辑删）" || bad "Tier B: DELETE（$R）"
elif [ -n "$ZLM_OK" ]; then
  say "[SKIP] Tier B：真实码流未就绪（ffmpeg 推流失败）"
fi

# ---- 10. Tier C：真件 engine 拉回环流 → MPP/RGA/RKNN → detections WS ----
if [ -n "$ZLM_OK" ] && [ "${STREAMED:-}" = "1" ]; then
    # 9.3 停 fake engine → 真件 engine（无 --fake；rtsp 直指 ZLM 回环流）
    kill "$ENGINE_PID"; wait "$ENGINE_PID" 2>/dev/null; ENGINE_PID=""
    "$ENGINE_BIN" --packages "$ROOT/tests/assets/algo_pkgs" --uds "$WORK/engine.sock" \
      > "$WORK/engine_real.log" 2>&1 &
    ENGINE_PID=$!
    UP=""
    for _ in $(seq 1 60); do
      grep -q "core-engine up" "$WORK/engine_real.log" 2>/dev/null && UP=1 && break
      sleep 0.2
    done
    [ -n "$UP" ] && ok "Tier C: engine 真件模式启动（mode=board）" \
      || { bad "Tier C: engine 真件启动（$(tail -5 "$WORK/engine_real.log" 2>/dev/null)）"; }
    # 9.4 经 REST 建"真流"摄像头（URL=ZLM 回环流）——SYNCING 前代理已存在→ensure 幂等
    R=$(curl -s --max-time 8 -X POST "$B/api/cameras" -H "Content-Type: application/json" \
        -d '{"name":"realcam","rtsp_main":"rtsp://127.0.0.1:'$ZLM_RTSP_PORT'/live/trtsp","rtsp_sub":"rtsp://127.0.0.1:'$ZLM_RTSP_PORT'/live/trtsp"}')
    RCAM_ID=$(printf '%s' "$R" | jget 'd["data"]["id"]')
    if [ -n "$RCAM_ID" ] && [ "$RCAM_ID" != "" ]; then
      ok "Tier C: 摄像头编排（id=$RCAM_ID，主/子同指回环流）"
      # 9.5 等真件链路出框：拉流→解码→RGA→RKNN→detections
      #     FakeInfer 之外首验：真包 yolov8n 出框可能为 0（合成画面无人形）——
      #     判据改为"engine 无 crash + dec meter 有出帧"（媒体面健康）+ WS 通道活性
      REAL_LIVE=""
      for _ in $(seq 1 100); do
        DEC=$(curl -s --max-time 2 "$B/api/resources" | jget 'd["data"]["cams"]["'$RCAM_ID'"]["dec"]["out"]')
        [ -n "$DEC" ] && [ "$DEC" != "None" ] && [ "$DEC" != "" ] && [ "$DEC" -gt 30 ] 2>/dev/null && REAL_LIVE=1 && break
        sleep 0.3
      done
      if [ -n "$REAL_LIVE" ]; then
        ok "Tier C: 真件解码出帧（dec.out=$DEC ≥30，媒体面健康）"
      else
        bad "Tier C: 真件解码无出帧（dec.out=$DEC；$(grep -E 'ERROR|rtsp' "$WORK/engine_real.log" | tail -3)）"
      fi
      # 9.6 真件模式存活 10s（无 crash）+ WS 探针（出框数可能为 0，仅验通道）
      if python3 scripts/ws_probe.py --port "$WS_PORT" --path "/ws/meta/$RCAM_ID" \
           --seconds 8 --min 0; then
        ok "Tier C: WS 通道活性（真件模式， detections 数以画面内容为准）"
      else
        bad "Tier C: WS 通道异常"
      fi
      CRASH=$(grep -ci "Segmentation\|core dumped" "$WORK/engine_real.log" 2>/dev/null || true)
      ALIVE=$(kill -0 "$ENGINE_PID" 2>/dev/null && echo 1 || echo 0)
      [ "$ALIVE" = "1" ] && [ "${CRASH:-0}" = "0" ] \
        && ok "Tier C: 真件 10s+ 稳定（无 crash）" || bad "Tier C: 真件稳定性（alive=$ALIVE）"
    else
      bad "Tier C: 摄像头编排失败（$R）"
    fi
fi

say ""
say "======== M4 冒烟结果: PASS=$PASS FAIL=$FAIL ========"
[ "$FAIL" = "0" ]
