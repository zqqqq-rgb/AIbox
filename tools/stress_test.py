#!/usr/bin/env python3
# stress_test —— core-engine 真件压力测试（单摄像头 × 16 通道同流满载）
#
# 原理：ZLM 一路代理（cam1_sub）支持多订阅者，16 个通道全部 add_cam 同一
#       回环流 URL → 摄像头只占 1 个 RTSP 会话，engine 侧构造真实 16 通道满载：
#       16×MPP 解码实例并发 + RGA 48 次/s + NPU 双桶调度（各算法一桶）。
#
# 阶段（--stage，all 依序跑 s1→s2→s4→s5→churn；s5 需第二个算法包，缺失自动跳过）：
#   s1    基线        1ch@3fps              期望 3 det/s
#   s2    单通道拉满   1ch@25fps             Core0≈43%（25×17.19ms）
#   s3    双桶双核    1ch 双 algo@25fps      Core0/Core1 各≈43%（需双包）
#   s4    16 通道     同流×16ch@fps          单桶天花板 ≈42 det/s（单 worker 72%+
#                                            解码/RGA 抢 CPU）；双桶上双核需 s3/s5
#   s5    过载降级    16ch×2algo@5fps        需求≈165%>核预算 → 降级①自动压 fps
#   churn 生命周期   remove/add ×loops      ack 同步逐条（防命令积压——remove 在
#                                            engine UDS 线程同步 stop，积压会事后
#                                            长时间消化，v1 板上实测教训）
#
# 用法（板上，engine 已启动、registry 已含 person-det）：
#   python3 tools/stress_test.py --stage all
#   python3 tools/stress_test.py --stage s4 --seconds 1800
#   python3 tools/stress_test.py --stage s5 --seconds 300
#   python3 tools/stress_test.py --stage churn --loops 50
import argparse
import json
import socket
import subprocess
import sys
import threading
import time

ap = argparse.ArgumentParser(description="core-engine 真件压力测试")
ap.add_argument("--sock", default="/tmp/aibox-engine.sock")
ap.add_argument("--url", default="rtsp://127.0.0.1:8554/live/cam1_sub",
                help="ZLM 回环流地址（所有通道共用）")
ap.add_argument("--algo", default="person-det", help="主算法（steady 桶）")
ap.add_argument("--algo2", default="person-det-2", help="第二算法（s3/s5 双桶用）")
ap.add_argument("--fps", type=int, default=3, help="s4 通道检测帧率")
ap.add_argument("--channels", type=int, default=16, help="s4/s5 通道数")
ap.add_argument("--seconds", type=int, default=300, help="观测阶段持续秒数")
ap.add_argument("--stage", default="all",
                choices=["s1", "s2", "s3", "s4", "s5", "churn", "all"])
ap.add_argument("--loops", type=int, default=50, help="churn remove/add 次数")
args = ap.parse_args()


def log(msg: str):
    """stderr 输出统一带墙钟时间戳（与 engine 的 [MM-DD HH:MM:SS.mmm] 对表）"""
    print(f"[{time.strftime('%m-%d %H:%M:%S')}][stress] {msg}", file=sys.stderr)


s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(args.sock)
log(f"connected: {args.sock}")

msg_id = 0
send_lock = threading.Lock()
ack_lock = threading.Lock()
acks = {}              # msg_id -> ret（rx 线程填，send_wait 消费）

det_total = 0          # 全程累计
nonempty_total = 0
ch_dets = {}           # 当前阶段 cam -> 计数（阶段边界清零，避免跨阶段同 id 假失衡）
ch_seq = {}            # 当前阶段 cam -> 上一条 frame_seq（保序核验，阶段边界清零）
seq_back = 0
degrade_events = []
conn_alive = True

snap_lock = threading.Lock()


def rx_loop():
    global det_total, nonempty_total, seq_back, conn_alive
    buf = ""
    while True:
        try:
            d = s.recv(65536)
        except OSError:
            conn_alive = False
            log("!! 连接异常断开（engine 崩溃）")
            return
        if not d:
            conn_alive = False
            log("!! 连接关闭（engine 退出）")
            return
        buf += d.decode(errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            if not line.strip():
                continue
            try:
                m = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "ack" in m:  # ack → 唤醒 send_wait
                with ack_lock:
                    acks[m["ack"]] = m.get("ret", -1)
                continue
            with snap_lock:
                if m.get("evt") == "detections":
                    det_total += 1
                    if m.get("targets"):
                        nonempty_total += 1
                    cam = m.get("id", 0)
                    ch_dets[cam] = ch_dets.get(cam, 0) + 1
                    seq = m.get("frame_seq", -1)
                    prev = ch_seq.get(cam)
                    # frame_seq 是"采样通过帧"的计数：降级/恢复切换 Sampler 令牌后，
                    # 同通道相邻 detections 的 seq 可合法回落（采样窗口换挡）——
                    # 大幅负跳（>100）才记失序候选（v2 板上 S5 降级震荡教训：
                    # 换挡期合法负跳约 1~2，真实失序需结合 pts 回退判定）
                    if prev is not None and seq < prev and (prev - seq) > 100:
                        seq_back += 1
                    ch_seq[cam] = seq
                elif m.get("evt") == "degrade":
                    degrade_events.append(m)
                    log(f"降级事件: {json.dumps(m, ensure_ascii=False)}")


def send_wait(payload: dict, timeout: float = 10.0):
    """同步命令：发送后等对应 ack（remove_cam 在 engine 侧同步 stop 较慢，勿缩小超时）。
    返回 ret（None=超时）。"""
    global msg_id
    with send_lock:
        msg_id += 1
        payload["msg_id"] = msg_id
        want = msg_id
        s.sendall(json.dumps(payload).encode() + b"\n")
    deadline = time.time() + timeout
    while time.time() < deadline:
        with ack_lock:
            if want in acks:
                return acks.pop(want)
        time.sleep(0.02)
    return None


def new_stage():
    """阶段边界清零：cam_id 跨阶段复用（重建通道 seq 归零）会让保序/均衡统计
    产生假阳性——v1 板上实测教训。"""
    with snap_lock:
        ch_dets.clear()
        ch_seq.clear()


def setup_channel(cam: int, fps: int, algos: list) -> bool:
    """建通道（ack 同步）。set_algos 失败（如算法包缺失 1002）返回 False。"""
    if send_wait({"cmd": "add_cam", "id": cam, "rtsp_sub": args.url}) != 0:
        log(f"add_cam cam={cam} 失败/超时")
        return False
    if algos:
        ret = send_wait({"cmd": "set_algos", "id": cam,
                         "algos": [{"algo": a, "thr": 0.45, "fps": fps} for a in algos]})
        if ret != 0:
            log(f"set_algos cam={cam} ret={ret}（1002=算法包缺失）")
            send_wait({"cmd": "remove_cam", "id": cam})
            return False
    send_wait({"cmd": "sub_meta", "id": cam})
    return True


def cleanup(channels):
    for cam in channels:
        send_wait({"cmd": "remove_cam", "id": cam})


def npu_load() -> str:
    try:
        r = subprocess.run(["cat", "/sys/kernel/debug/rknpu/load"],
                           capture_output=True, text=True, timeout=2)
        return " ".join(r.stdout.split())
    except Exception:
        return "load=?"


def monitor(label: str, seconds: int, expect_rate: float):
    """每 10s 快照（阶段内速率 + NPU 负载），返回阶段平均 det/s。"""
    print(f"\n[{time.strftime('%m-%d %H:%M:%S')}][stress] === {label}: 观测 {seconds}s"
          f"（期望 ≈{expect_rate:.1f} det/s）===", file=sys.stderr)
    with snap_lock:
        base = det_total
    t_end = time.time() + seconds
    last = base
    while time.time() < t_end and conn_alive:
        time.sleep(10)
        with snap_lock:
            now = det_total
        d10 = (now - last) / 10.0
        last = now
        log(f"{label} rate={d10:.2f}/s cum={now} | NPU {npu_load()}")
    elapsed = max(time.time() - (t_end - seconds), 1.0)
    with snap_lock:
        avg = (det_total - base) / elapsed
    log(f"{label} 阶段均速 {avg:.2f} det/s")
    return avg


th = threading.Thread(target=rx_loop, daemon=True)
th.start()
time.sleep(0.3)

stages = [args.stage] if args.stage != "all" else ["s1", "s2", "s4", "s5", "churn"]
report_rows = []

try:
    for st in stages:
        if not conn_alive:
            break
        new_stage()

        if st == "s1":
            if setup_channel(1, 3, [args.algo]):
                avg = monitor("S1基线(1ch@3fps)", 60, 3.0)
                report_rows.append(("S1 基线", avg, 3.0, ch_dets.copy()))
                cleanup([1])

        elif st == "s2":
            if setup_channel(1, 25, [args.algo]):
                avg = monitor("S2单通道拉满(1ch@25fps)", 120, 25.0)
                report_rows.append(("S2 拉满", avg, 25.0, ch_dets.copy()))
                cleanup([1])

        elif st == "s3":
            if setup_channel(1, 25, [args.algo, args.algo2]):
                avg = monitor("S3双桶双核(1ch×2algo@25fps)", 120, 50.0)
                report_rows.append(("S3 双桶", avg, 50.0, ch_dets.copy()))
                cleanup([1])
            else:
                log(f"S3 跳过：{args.algo2} 包缺失（复制 person-det 目录改 algo_id 即可）")
                report_rows.append(("S3 双桶", -1, -1, {}))

        elif st == "s4":
            n = args.channels
            log(f"S4: 建 {n} 通道（全部订阅 {args.url}）…")
            built = [c for c in range(1, n + 1) if setup_channel(c, args.fps, [args.algo])]
            if built:
                avg = monitor(f"S4-{len(built)}通道@{args.fps}fps", args.seconds,
                              len(built) * args.fps)
                report_rows.append((f"S4 {len(built)}ch", avg,
                                    len(built) * args.fps, ch_dets.copy()))
                cleanup(built)

        elif st == "s5":
            n = args.channels
            # 探测双包是否就绪（v1 教训：缺失时 16 通道 set_algos 全 1002，阶段空转）
            send_wait({"cmd": "add_cam", "id": 1, "rtsp_sub": args.url})
            probe = send_wait({"cmd": "set_algos", "id": 1,
                               "algos": [{"algo": args.algo2, "thr": 0.45, "fps": 1}]})
            if probe != 0:
                log(f"S5 跳过：{args.algo2} 包缺失（ret={probe}）"
                    f"——复制 person-det 目录、manifest 改 algo_id 后重试")
                send_wait({"cmd": "remove_cam", "id": 1})
                report_rows.append(("S5 降级", -1, -1, {}))
                continue
            send_wait({"cmd": "set_algos", "id": 1,
                       "algos": [{"algo": args.algo, "thr": 0.45, "fps": 5},
                                 {"algo": args.algo2, "thr": 0.45, "fps": 5}]})
            for cam in range(2, n + 1):
                setup_channel(cam, 5, [args.algo, args.algo2])
            monitor(f"S5过载降级({n}ch×2@5fps)", args.seconds, -1)
            if degrade_events:
                log(f"✓ 降级闭环成立（{len(degrade_events)} 次 degrade）")
            else:
                log("⚠ 未观测到 degrade 事件"
                    "（过载被 newest-wins 吸收/水位未达①阈值，非失败）")
            report_rows.append(("S5 降级", -1, -1, ch_dets.copy()))
            cleanup(list(range(1, n + 1)))

        elif st == "churn":
            log(f"churn: remove/add ×{args.loops}（ack 同步，每轮等 engine 完成 stop）")
            done = 0
            for i in range(args.loops):
                if not conn_alive:
                    break
                if send_wait({"cmd": "add_cam", "id": 1, "rtsp_sub": args.url}) != 0:
                    log(f"churn add 失败 @ {i+1}")
                    break
                if send_wait({"cmd": "remove_cam", "id": 1}) != 0:
                    log(f"churn remove 失败 @ {i+1}")
                    break
                done += 1
                if done % 20 == 0:
                    log(f"churn {done}/{args.loops} 引擎存活")
            report_rows.append((f"churn×{done}", 1 if conn_alive else 0, 1, {}))
            log(f"churn 完成 {done}/{args.loops}，"
                f"engine 存活 = {'是' if conn_alive else '否'}")

except KeyboardInterrupt:
    log("手动中断")
finally:
    log("===== 压测总账 =====")
    for name, avg, exp, balance in report_rows:
        mark = ""
        if exp > 0:
            ratio = avg / exp
            mark = ("✓" if 0.8 <= ratio <= 1.2 else f"⚠ 达成率 {ratio*100:.0f}%")
        line = f"{name:<24} 均速 {avg:>8.2f} det/s  {mark}"
        if balance:
            counts = sorted(balance.values())
            line += (f" | 均衡 min={counts[0]} max={counts[-1]} "
                     f"通道={len(counts)}")
        log(line)
    log(f"detections={det_total} 含目标={nonempty_total}"
        f"({100*nonempty_total/max(det_total,1):.0f}%)")
    log(f"同通道保序(阶段内): "
        f"{'OK' if seq_back == 0 else f'!! 回退 {seq_back} 次'}")
    log(f"降级事件: {len(degrade_events)}")
    log(f"engine 存活: {'是' if conn_alive else '否（压测失败）'}")
    if conn_alive:
        send_wait({"cmd": "get_metrics"})  # 收尾 metrics（ack 同步确认 engine 可用）
        log("收尾完成，engine 可继续服务")
