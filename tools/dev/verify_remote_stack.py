"""Isolated fake stack for browser verification; never uses the learning/production DB.

Run inside the development image. Ports 18092/18093 must be published to loopback
when the browser runs on Windows. The credential below is test-only.
"""
import json
import os
import signal
import subprocess
import tempfile
import threading
import time
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

from mock_media import Handler

ROOT = Path(__file__).resolve().parents[2]
TOKEN = "local-browser-verification-only"
STOP = threading.Event()
signal.signal(signal.SIGTERM, lambda *_: STOP.set())
signal.signal(signal.SIGINT, lambda *_: STOP.set())


def main():
    with tempfile.TemporaryDirectory(prefix="aibox_browser_") as directory:
        runtime = Path(directory)
        package = runtime / "packages/person"
        package.mkdir(parents=True)
        (package / "manifest.json").write_text(json.dumps({
            "algo_id": "person", "version": "1.0.0", "platform": "rk3588",
            "model_file": "model.rknn", "input_w": 64, "input_h": 64,
            "num_cls": 1, "core": "steady", "default_threshold": .45, "npu_cost_ms": 20,
        }))
        (package / "model.rknn").write_bytes(b"FAKE TEST ONLY")
        (runtime / "video.nv12").write_bytes(bytes([128]) * (64 * 64 * 3 // 2) * 300)
        recordings = runtime / "recordings"
        recordings.mkdir()
        (recordings / "proof.mp4").write_bytes(b"private test evidence, not real video")
        media_port = int(os.environ.get("AIBOX_VERIFY_MEDIA_PORT", "0"))
        media = ThreadingHTTPServer(("0.0.0.0" if media_port else "127.0.0.1", media_port), Handler)
        threading.Thread(target=media.serve_forever, daemon=True).start()
        config = {"http_port": 18092, "ws_port": 18093, "hook_port": 18094,
                  "bind_host": "0.0.0.0", "uds_path": str(runtime / "engine.sock"),
                  "db_path": str(runtime / "business.db"), "static_dir": str(ROOT / "web"),
                  "recordings_dir": str(recordings), "auth_token": TOKEN,
                  "zlm": {"host": "127.0.0.1", "port": media.server_port,
                          "rtsp_port": 8554, "secret": "test-private-media-secret"}}
        public_origin = os.environ.get("AIBOX_VERIFY_PUBLIC_ORIGIN", "")
        if public_origin:
            config["public_base_url"] = public_origin
        (runtime / "businessd.json").write_text(json.dumps(config))
        processes, logs = [], []
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def api(path, body=None):
            request = urllib.request.Request("http://127.0.0.1:18092" + path,
                data=None if body is None else json.dumps(body).encode(),
                headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"})
            with opener.open(request, timeout=5) as response:
                value = json.load(response)
            if value["code"]:
                raise RuntimeError(value)
            return value["data"]

        try:
            commands = [
                ("engine", [str(ROOT / "build-dev/core-engine-fake"), "--fake", "--video", str(runtime / "video.nv12"),
                            "--frame-w", "64", "--frame-h", "64", "--fake-fps", "10",
                            "--packages", str(runtime / "packages"), "--uds", str(runtime / "engine.sock")]),
                ("businessd", [str(ROOT / "build-dev/businessd"), "--config", str(runtime / "businessd.json")]),
            ]
            for name, command in commands:
                log = (runtime / (name + ".log")).open("w")
                logs.append(log)
                processes.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT))
            for _ in range(100):
                if STOP.is_set() or any(process.poll() is not None for process in processes):
                    raise RuntimeError("verification service stopped before ready")
                try:
                    if api("/api/system/info")["engine_state"] == "ONLINE":
                        break
                except (OSError, RuntimeError):
                    pass
                time.sleep(.1)
            else:
                raise RuntimeError("verification services not ready")
            api("/api/cameras", {"name": "浏览器验证相机", "rtsp_main": "rtsp://fake/main", "rtsp_sub": "rtsp://fake/sub"})
            print(json.dumps({"ready": True, "http_port": 18092, "ws_port": 18093}), flush=True)
            while not STOP.wait(.2):
                if any(process.poll() is not None for process in processes):
                    raise RuntimeError("verification service exited")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            media.shutdown()
            for log in logs:
                log.close()


if __name__ == "__main__":
    main()
