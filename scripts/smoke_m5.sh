#!/usr/bin/env bash
# M5 前端验收冒烟（板上）：businessd 静态托管 web/ + 前端契约自动断言
#
# 覆盖（前端设计 §5/§6/§10 的"脚本可自动验证"部分）：
#   Tier A（无 ZLM 可跑）：静态托管 8 文件 + 内容点 / system-info / DB 直插→SYNCING
#                         全量同步 / stream 信令(sub|main) / WS meta D3 契约
#                         （detections 含 frame_w:frame_h）/ PUT algos npu_est /
#                         events+recordings envelope / engine 重启→WS events
#                         registry_ready（前端自愈链服务端侧）
#   浏览器可视项（框压人/宫格切换/事件弹窗等）无法脚本化——按 web/README.md
#   手工验收清单过一遍，本脚本全过是手工清单的前置条件。
#
# 用法：
#   ./scripts/smoke_m5.sh
#   SMOKE_KEEP=1 ./scripts/smoke_m5.sh    # 保留现场供排查（人工清理）
# 前置：./scripts/build.sh --tests；端口 18082/18083/18091 空闲；
#       无残留 core-engine/businessd 进程（脚本启动前检测并 BLOCK）。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"
WORK="$(mktemp -d /tmp/aibox_smoke_m5.XXXXXX)"
ENGINE_BIN="$ROOT/build/core-engine"
BIZ_BIN="$ROOT/build/businessd"
WEB_DIR="$ROOT/web"
HTTP_PORT="${SMOKE_M5_HTTP_PORT:-18082}"; WS_PORT="${SMOKE_M5_WS_PORT:-18083}"; HOOK_PORT="${SMOKE_M5_HOOK_PORT:-18091}"
PASS=0; FAIL=0
ENGINE_PID=""; BIZ_PID=""

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
    say "[KEEP] 现场保留：WORK=$WORK（engine=$ENGINE_PID biz=$BIZ_PID）"
    say "       浏览器打开 http://127.0.0.1:$HTTP_PORT/ 验收手工清单项"
    say "       日志：$WORK/biz.log / $WORK/engine.log；清场：kill $ENGINE_PID $BIZ_PID 2>/dev/null; rm -rf $WORK"
    return
  fi
  [ -n "$BIZ_PID" ] && kill "$BIZ_PID" 2>/dev/null
  [ -n "$ENGINE_PID" ] && kill "$ENGINE_PID" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

# ---- 0. 依赖与产物 ----
[ -x "$ENGINE_BIN" ] || { say "[FAIL] core-engine 不存在（./scripts/build.sh --hw --tests）"; exit 1; }
[ -x "$BIZ_BIN" ]    || { say "[FAIL] businessd 不存在（./scripts/build.sh --tests）"; exit 1; }
[ -f "$WEB_DIR/index.html" ] || { say "[FAIL] web/index.html 不存在（T5.2 交付缺失）"; exit 1; }
command -v python3 >/dev/null || { say "[FAIL] python3 缺失"; exit 1; }
command -v curl >/dev/null    || { say "[FAIL] curl 缺失"; exit 1; }

# 前置环境卫生（同 smoke_m4：只报告不擅杀）
DIRTY=0
for pat in "core-engine" "businessd"; do
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
  "static_dir": "$WEB_DIR",
  "uds_path": "$WORK/engine.sock",
  "db_path": "$WORK/biz.db",
  "keep_days": 0
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

# ---- 2. businessd 启动 → ready → ONLINE ----
"$BIZ_BIN" --config "$WORK/businessd.conf" > "$WORK/biz.log" 2>&1 &
BIZ_PID=$!
READY=""
for _ in $(seq 1 60); do
  READY=$(curl -s --max-time 2 "$B/api/system/info" 2>/dev/null | jget 'd["data"]["ready"]')
  [ "$READY" = "True" ] && break
  sleep 0.2
done
[ "$READY" = "True" ] && ok "businessd ready" \
  || { bad "businessd ready"; tail -8 "$WORK/biz.log"; exit 1; }
grep -q "state SYNCING -> ONLINE" "$WORK/biz.log" \
  && ok "UDS 状态机 SYNCING→ONLINE（空库）" || bad "UDS 状态机（见 biz.log）"

# ---- 3. 静态托管：9 文件 200 + 内容点 ----
FILES="index.html app.js api.js ws.js player.js ui.js overlay.js overlay_core.js vendor/ZLMRTCClient.js"
MISS=0
for f in $FILES; do
  CODE=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "$B/$f")
  [ "$CODE" = "200" ] || { say "  [FAIL-detail] GET /$f → $CODE"; MISS=$((MISS+1)); }
done
[ "$MISS" = "0" ] && ok "静态托管 9 文件全 200（含 vendor/ZLMRTCClient.js）" \
  || bad "静态托管缺 $MISS 个文件"
# 内容点：确认不是 404 页/空壳（工控色 token / 关键函数 / 播放内核存在）
H=$(curl -s --max-time 3 "$B/")
printf '%s' "$H" | grep -q "16181d" && ok "index.html 工控深色 token（#16181d）" || bad "index.html 缺色 token"
printf '%s' "$H" | grep -q "ZLMRTCClient.js" && ok "index.html 引用播放内核 vendor" || bad "index.html 缺 vendor 引用"
curl -s --max-time 3 "$B/app.js" | grep -q "pageGrid" \
  && ok "app.js 路由装配（pageGrid）" || bad "app.js 缺 pageGrid"
curl -s --max-time 3 "$B/overlay_core.js" | grep -q "computeAnchor" \
  && ok "overlay_core.js（computeAnchor）" || bad "overlay_core.js 缺 computeAnchor"

# ---- 4. system/info 契约 ----
V=$(curl -s "$B/api/system/info" | jget 'd["data"]["version"]')
WSP=$(curl -s "$B/api/system/info" | jget 'd["data"]["ws_port"]')
[ -n "$V" ] && [ "$WSP" = "$WS_PORT" ] \
  && ok "system/info（version=$V ws_port=$WSP）" || bad "system/info（v=$V ws=$WSP）"

# ---- 5. DB 直插摄像头 → 重启 businessd → SYNCING 全量同步 ----
python3 - "$WORK/biz.db" <<'PYEOF' >/dev/null 2>&1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT INTO cameras(name, rtsp_main, rtsp_sub, enabled) "
           "VALUES('m5smoke','rtsp://m5/main','rtsp://m5/sub',1)")
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
CAM_ID=$(curl -s "$B/api/cameras" | jget 'd["data"][0]["id"]')
[ -n "$CAM_ID" ] && ok "/api/cameras（id=$CAM_ID）" || bad "/api/cameras 空"

# ---- 6. stream 信令（businessd 纯拼接，无 ZLM 依赖）----
for Q in sub main; do
  SIG=$(curl -s "$B/api/cameras/$CAM_ID/stream?quality=$Q" | jget 'd["data"]["signaling"]')
  case "$SIG" in
    *"app=live&stream=cam${CAM_ID}_${Q}&type=play"*)
      ok "stream 信令 quality=$Q（webrtc URL 形态）" ;;
    *) bad "stream 信令 quality=$Q（$SIG）" ;;
  esac
done
T=$(curl -s "$B/api/cameras/$CAM_ID/stream?quality=sub" | jget 'd["data"]["type"]')
[ "$T" = "webrtc" ] && ok "stream type=webrtc（前端 zlmsdpUrl 直喂前提）" || bad "stream type=$T"

# ---- 7. WS meta：Detections 流量 + D3 契约（frame_w/frame_h）----
# 内联探针（ws_probe.py 只计数不断言内容）：解析 text 帧，断言 detections 含
# "frame_w":640 与 "frame_h":480（紧凑序列化；= --frame-w/--frame-h 池分辨率）
WSRES=$(python3 - "$WS_PORT" "/ws/meta/$CAM_ID" 8 <<'PYEOF'
import socket, base64, os, sys, time, json
port, path, seconds = int(sys.argv[1]), sys.argv[2], float(sys.argv[3])
key = base64.b64encode(os.urandom(16)).decode()
req = (f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
except OSError as e:
    print(f"connect fail: {e}"); sys.exit(2)
s.sendall(req.encode())
buf = b""
while b"\r\n\r\n" not in buf:
    c = s.recv(4096)
    if not c: print("handshake closed"); sys.exit(2)
    buf += c
head, _, payload = buf.partition(b"\r\n\r\n")
if b"101" not in head.split(b"\r\n")[0]:
    print("handshake rejected"); sys.exit(2)

def need(n):
    global payload
    while len(payload) < n:
        try:
            s.settimeout(max(0.1, seconds - (time.time() - t0)))
            c = s.recv(4096)
        except (socket.timeout, OSError):
            return False
        if not c: return False
        payload += c
    return True

t0 = time.time()
det = fw = fh = 0
while time.time() - t0 < seconds:
    if not need(2): break
    op = payload[0] & 0x0F; ln = payload[1] & 0x7F; off = 2
    if ln == 126:
        if not need(4): break
        ln = int.from_bytes(payload[2:4], "big"); off = 4
    elif ln == 127:
        if not need(10): break
        ln = int.from_bytes(payload[2:10], "big"); off = 10
    if not need(off + ln): break
    body = payload[off:off + ln]; payload = payload[off + ln:]
    if op != 0x1: continue
    try:
        j = json.loads(body.decode(errors="replace"))
    except Exception:
        continue
    if j.get("evt") != "detections": continue
    det += 1
    if j.get("frame_w") == 640: fw += 1
    if j.get("frame_h") == 480: fh += 1
print(f"detections={det} frame_w_ok={fw} frame_h_ok={fh}")
sys.exit(0 if det >= 3 and fw >= 3 and fh >= 3 else 1)
PYEOF
)
if [ $? = 0 ]; then
  ok "WS meta D3 契约（$WSRES）"
else
  bad "WS meta D3 契约（$WSRES；期望 detections≥3 且 frame_w=640/frame_h=480 各≥3）"
fi

# ---- 8. PUT algos：热生效 + npu_est 契约（前端配置页数据源）----
R=$(curl -s --max-time 8 -X PUT "$B/api/cameras/$CAM_ID/algos" \
    -H "Content-Type: application/json" \
    -d '{"algos":[{"algo_id":"person-det","fps":3,"threshold":0.5}]}')
CP=$(printf '%s' "$R" | jget 'd["data"]["npu_est"]["compute_pct"]')
MP=$(printf '%s' "$R" | jget 'd["data"]["npu_est"]["mem_pct"]')
if [ -n "$CP" ] && [ "$CP" != "None" ] && [ -n "$MP" ] && [ "$MP" != "None" ]; then
  ok "PUT algos npu_est（compute=$CP% mem=$MP%）"
else
  bad "PUT algos npu_est（$R）"
fi

# ---- 9. events / recordings envelope ----
C1=$(curl -s "$B/api/events?limit=5" | jget 'd["code"]')
C2=$(curl -s "$B/api/recordings?limit=5" | jget 'd["code"]')
[ "$C1" = "0" ] && ok "GET /api/events envelope code=0" || bad "/api/events code=$C1"
[ "$C2" = "0" ] && ok "GET /api/recordings envelope code=0" || bad "/api/recordings code=$C2"

# ---- 10. WS events + engine 重启自愈链（服务端侧）----
# 探针先连 /ws/events → kill engine → 重启 → engine 扫包后必发 registry_ready
#（前端对应：顶栏 engine 灯红→绿、算法库整表刷新、meta 重锚）
python3 - "$WS_PORT" "$WORK" <<'PYEOF' > "$WORK/evprobe.log" 2>&1 &
import socket, base64, os, sys, time, json
port, work = int(sys.argv[1]), sys.argv[2]
key = base64.b64encode(os.urandom(16)).decode()
req = ("GET /ws/events HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s = socket.create_connection(("127.0.0.1", port), timeout=3)
s.sendall(req.encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
head, _, payload = buf.partition(b"\r\n\r\n")
assert b"101" in head.split(b"\r\n")[0], "handshake"
deadline = time.time() + 25
evts = set()
def need(n):
    global payload
    while len(payload) < n:
        try:
            s.settimeout(max(0.1, deadline - time.time()))
            c = s.recv(4096)
        except (socket.timeout, OSError):
            return False
        if not c: return False
        payload += c
    return True
while time.time() < deadline:
    if not need(2): break
    op = payload[0] & 0x0F; ln = payload[1] & 0x7F; off = 2
    if ln == 126:
        if not need(4): break
        ln = int.from_bytes(payload[2:4], "big"); off = 4
    elif ln == 127:
        if not need(10): break
        ln = int.from_bytes(payload[2:10], "big"); off = 10
    if not need(off + ln): break
    body = payload[off:off + ln]; payload = payload[off + ln:]
    if op != 0x1: continue
    try:
        j = json.loads(body.decode(errors="replace"))
    except Exception:
        continue
    if "evt" in j:
        evts.add(j["evt"])
        open(os.path.join(work, "ev_events.txt"), "a").write(j["evt"] + "\n")
        if j["evt"] == "registry_ready":
            print("registry_ready received")
            sys.exit(0)
sys.exit(1)
PYEOF
EVPID=$!
sleep 1
kill "$ENGINE_PID"; wait "$ENGINE_PID" 2>/dev/null; ENGINE_PID=""
"$ENGINE_BIN" --fake --video "$WORK/synthetic.nv12" --frame-w 640 --frame-h 480 \
  --fake-fps 15 --packages "$ROOT/tests/assets/algo_pkgs" --uds "$WORK/engine.sock" \
  >> "$WORK/engine.log" 2>&1 &
ENGINE_PID=$!
wait "$EVPID"
if [ $? = 0 ]; then
  ok "WS events：engine 重启 → registry_ready 推达（前端算法库刷新源）"
else
  bad "WS events：25s 内未收到 registry_ready（$(tail -2 "$WORK/evprobe.log" 2>/dev/null)）"
fi
grep -q "state ONLINE -> DISCONNECTED" "$WORK/biz.log" \
  && ok "engine 下线感知（ONLINE→DISCONNECTED，前端顶栏红灯数据源）" \
  || bad "engine 下线感知"

say ""
say "======== M5 冒烟结果: PASS=$PASS FAIL=$FAIL ========"
say "（脚本项全过后，浏览器打开 http://<板IP>:${HTTP_PORT}/ 过 web/README.md 手工清单）"
[ "$FAIL" = "0" ]
