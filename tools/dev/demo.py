"""Start/stop an isolated learning stack. Requires Linux, Python standard library only."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build-dev"
RUNTIME = BUILD / "runtime"
PID = RUNTIME / "supervisor.pid"


def prepare():
    package = RUNTIME / "packages/person"
    package.mkdir(parents=True, exist_ok=True)
    (package / "manifest.json").write_text(json.dumps({
        "algo_id": "person", "version": "1.0.0", "platform": "rk3588",
        "model_file": "model.rknn", "input_w": 64, "input_h": 64,
        "num_cls": 1, "core": "steady", "default_threshold": 0.45, "npu_cost_ms": 20
    }))
    (package / "model.rknn").write_bytes(b"FAKE ONLY - not a real RKNN model")
    if not (RUNTIME / "video.nv12").exists():
        (RUNTIME / "video.nv12").write_bytes(bytes([128]) * (64 * 64 * 3 // 2) * 600)
    (RUNTIME / "businessd.json").write_text(json.dumps({
        "db_path": str(RUNTIME / "business.db"), "uds_path": str(RUNTIME / "engine.sock"),
        "http_port": 8080, "ws_port": 8081, "hook_port": 8090, "bind_host": "0.0.0.0",
        "static_dir": str(ROOT / "web"), "recordings_dir": "", "mqtt_enabled": False,
        "zlm": {"host": "127.0.0.1", "port": 18080, "rtsp_port": 8554, "secret": "learning-only"}
    }, indent=2))


def owned_pid():
    try:
        pid = int(PID.read_text())
        command = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        if os.fsencode(str(Path(__file__).resolve())) in command and (b"serve" in command or b"serve-services" in command):
            return pid
    except (OSError, ValueError):
        pass
    return None


def serve(services_only=False):
    import fcntl
    prepare()
    with (RUNTIME / "supervisor.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        PID.write_text(str(os.getpid()))
        children, logs = [], []
        stopping = False
        def stop_signal(*_):
            nonlocal stopping
            stopping = True
        signal.signal(signal.SIGTERM, stop_signal)
        signal.signal(signal.SIGINT, stop_signal)
        commands = [
            ("media", [sys.executable, str(ROOT / "tools/dev/mock_media.py")]),
            ("engine", [str(BUILD / "core-engine-fake"), "--fake", "--video", str(RUNTIME / "video.nv12"),
                        "--packages", str(RUNTIME / "packages"), "--uds", str(RUNTIME / "engine.sock"),
                        "--frame-w", "64", "--frame-h", "64", "--fake-fps", "10"]),
            ("businessd", [str(BUILD / "businessd"), "--config", str(RUNTIME / "businessd.json")])
        ]
        if services_only:
            commands = commands[:2]
        try:
            for name, command in commands:
                if name == "businessd":
                    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                    for attempt in range(50):
                        if stopping or any(child.poll() is not None for child in children):
                            raise RuntimeError("A debug service exited before businessd startup")
                        try:
                            with opener.open("http://127.0.0.1:18080/index/api/getMediaList", timeout=1) as response:
                                if json.load(response)["code"] == 0 and (RUNTIME / "engine.sock").exists():
                                    break
                        except (OSError, ValueError, KeyError):
                            pass
                        time.sleep(.2)
                    else:
                        raise RuntimeError("Media/engine readiness timed out")
                log = (RUNTIME / f"{name}.log").open("a")
                logs.append(log)
                children.append(subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT))
            while not stopping and all(child.poll() is None for child in children):
                time.sleep(0.2)
        finally:
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            for log in logs:
                log.close()
            PID.unlink(missing_ok=True)


def start(services_only=False):
    prepare()
    current = owned_pid()
    if current:
        command = Path(f"/proc/{current}/cmdline").read_bytes().split(b"\0")
        if (b"serve-services" in command) != services_only:
            raise SystemExit("Stop the existing demo before switching debug/full-stack modes.")
    else:
        for binary in ("businessd", "core-engine-fake"):
            if not (BUILD / binary).exists():
                raise SystemExit("Build first: cmake --build build-dev -j4")
        with (RUNTIME / "supervisor.log").open("a") as log:
            subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "serve-services" if services_only else "serve"], cwd=ROOT,
                             stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    for _ in range(100):
        try:
            if services_only:
                with opener.open("http://127.0.0.1:18080/index/api/getMediaList", timeout=1) as response:
                    if json.load(response)["code"] == 0 and (RUNTIME / "engine.sock").exists():
                        print("Debug services ready; launch businessd from VS Code F5.")
                        return
            with opener.open("http://127.0.0.1:8080/api/system/info", timeout=1) as response:
                data = json.load(response)["data"]
                if data.get("ready") and data.get("engine_state") == "ONLINE" and data.get("pending_operations") == 0 and owned_pid():
                    print("Ready: http://localhost:8080 (fake inference, mock media; no real video)")
                    return
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(0.2)
    raise SystemExit(f"Startup failed. Read logs under {RUNTIME}")


def stop():
    pid = owned_pid()
    if pid:
        os.kill(pid, signal.SIGTERM)
        for _ in range(150):
            if not owned_pid():
                print("Learning stack stopped; database retained.")
                return
            time.sleep(0.2)
        raise SystemExit("Shutdown is still pending; inspect supervisor.log")
    print("Learning stack is not running.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["prepare", "start", "stop", "serve", "start-services", "serve-services"])
    action = parser.parse_args().action
    if action == "start-services":
        start(True)
    elif action == "serve-services":
        serve(True)
    else:
        globals()[action]()
