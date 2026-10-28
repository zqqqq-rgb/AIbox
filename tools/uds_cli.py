#!/usr/bin/env python3
# uds_cli —— core-engine UDS 手动联调客户端（JSON-lines over AF_UNIX）
#
# 用途：M4 businessd 落地前替代 UDS 客户端；stdin 逐行转发给 engine，
#       stdout 实时打印 ack/事件流（--loop 模式自动生成 16 通道命令流并保温）。
#
# 用法一（手动交互，等价 socat）：
#   python3 uds_cli.py
#   > {"msg_id":1,"cmd":"add_cam","id":1,"rtsp_sub":"tests/assets/synthetic.nv12"}
#
# 用法二（交互模式注入命令流：stdin 打完后自动按 --hold 保活收流）：
#   for i in $(seq 1 16); do
#     echo "{\"msg_id\":$i,\"cmd\":\"add_cam\",\"id\":$i,\"rtsp_sub\":\"tests/assets/synthetic.nv12\"}"
#     echo "{\"msg_id\":100$i,\"cmd\":\"set_algos\",\"id\":$i,\"algos\":[{\"algo\":\"person-det\",\"thr\":0.5,\"fps\":3}]}"
#     echo "{\"msg_id\":200$i,\"cmd\":\"sub_meta\",\"id\":$i}"
#   done | python3 uds_cli.py --hold 1800
#
# 用法三（16 通道一键稳定性测试，T3.4 DoD）：
#   python3 uds_cli.py --loop 16 --hold 1800
#   （add_cam + set_algos + sub_meta × 16 通道，之后保持 30min 收流；
#     Ctrl+C 退出并打印本次会话 detections 计数/丢帧统计）
#
# 退出打印：收到的 detections 总数、tail 队列丢弃数（uds_dropped 应为 0——
# 慢消费者计数，板上 16 通道 30min 稳定性验收红线之一）
import argparse
import json
import socket
import sys
import threading
import time

def log(msg: str):
    """stderr 输出统一带墙钟时间戳（与 engine 的 [MM-DD HH:MM:SS.mmm] 对表）"""
    print(f"[{time.strftime('%m-%d %H:%M:%S')}][uds_cli] {msg}", file=sys.stderr)

ap = argparse.ArgumentParser(description="core-engine UDS 客户端")
ap.add_argument("--sock", default="/tmp/aibox-engine.sock", help="UDS 路径")
ap.add_argument("--loop", type=int, metavar="N", help="自动生成 1..N 通道命令流")
ap.add_argument("--algo", default="person-det", help="loop 模式算法 id")
ap.add_argument("--fps", type=int, default=3, help="loop 模式检测帧率")
ap.add_argument("--hold", type=int, default=0, metavar="SEC",
                help="stdin 用尽后保持收流秒数（0=交互模式：stdin 结束即退出）")
ap.add_argument("--show", action="store_true",
                help="实时打印 detections（默认静默计数，仅退出时出统计）")
args = ap.parse_args()

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(args.sock)
log(f"connected: {args.sock}")

det_count = 0
nonempty_count = 0   # 含目标的 detections（空目标 = 推理链路静默失败的信号）
first_sample = None
det_lock = threading.Lock()
last_seq = {}  # cam_id -> 上一条 detections 的 frame_seq（保序按"同通道"核验）
seq_back = 0   # 同通道 frame_seq 回退次数（正常恒 0；跨通道交错不是回退）
seq_back_worst = None  # 最严重回退样本 (cam, prev, cur)


def rx_loop():
    global det_count, nonempty_count, first_sample, seq_back, seq_back_worst
    buf = ""
    while True:
        try:
            d = s.recv(65536)
        except OSError:
            return
        if not d:
            log("连接已断开")
            return
        buf += d.decode(errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            if not line.strip():
                continue
            try:
                m = json.loads(line)
            except json.JSONDecodeError:
                print(line)
                continue
            if m.get("evt") == "detections":
                with det_lock:
                    det_count += 1
                    tg = m.get("targets") or []
                    if tg:
                        nonempty_count += 1
                        if first_sample is None:
                            first_sample = m
                            log("首条含目标 detections: "
                                + json.dumps(m, ensure_ascii=False))
                    cam = m.get("id", 0)
                    seq = m.get("frame_seq", -1)
                    prev = last_seq.get(cam)
                    if prev is not None and seq < prev:
                        seq_back += 1
                        seq_back_worst = (cam, prev, seq)
                    last_seq[cam] = seq
                    if args.show:
                        print(line, flush=True)  # 实时看框（冒烟用；长跑勿开，48帧/s刷屏）
                continue  # detections 默认不逐条打印
            print(line, flush=True)  # ack / camera_status / degrade 等照常打印


def report():
    with det_lock:
        n_ch = args.loop if args.loop else len(last_seq)
        # 速率用真实墙钟时长（交互模式 hold=0，除以 hold 会显示成 589 帧/s 的假象）
        elapsed = max(time.time() - t_start, 1.0)
        log(f"会话统计: detections={det_count} 通道数={len(last_seq)} "
            f"时长={elapsed:.0f}s ≈{det_count / elapsed:.1f} 帧/s"
            f"（期望 {n_ch * args.fps}）")
        log(f"各通道 last_frame_seq: {dict(sorted(last_seq.items()))}")
        if det_count and nonempty_count == 0:
            log("!! 全部 detections 无目标：检查 preproc/infer 链路"
                "（engine stderr 应有 sched fail 告警）或画面内无目标")
        if seq_back:
            log(f"!! 同通道 frame_seq 回退 {seq_back} 次"
                f"（最严重 {seq_back_worst}）：同通道结果失序，检查调度保序")
        else:
            log("同通道保序 OK（seq 回退=0）")


threading.Thread(target=rx_loop, daemon=True).start()
t_start = time.time()  # 会话统计的速率分母（含交互模式）

if args.loop:
    # 命令流：按通道串行发送（通道级因果序：同通道 add→set→sub 依次）
    msg_id = 0
    for cam in range(1, args.loop + 1):
        for payload in (
                {"cmd": "add_cam", "id": cam,
                 "rtsp_sub": "tests/assets/synthetic.nv12"},
                {"cmd": "set_algos", "id": cam,
                 "algos": [{"algo": args.algo, "thr": 0.5, "fps": args.fps}]},
                {"cmd": "sub_meta", "id": cam},
        ):
            msg_id += 1
            payload["msg_id"] = msg_id
            s.sendall(json.dumps(payload).encode() + b"\n")
            time.sleep(0.02)
    log(f"{args.loop} 通道命令流已发完")

try:
    if args.loop:
        end = time.time() + max(args.hold, 1)
        while time.time() < end:
            time.sleep(1)
    else:
        for line in sys.stdin:
            if line.strip():
                s.sendall(line.strip().encode() + b"\n")
        if args.hold > 0:
            time.sleep(args.hold)
except KeyboardInterrupt:
    pass
finally:
    report()
