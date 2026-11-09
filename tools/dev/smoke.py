"""Exercise the running learning stack; leaves its alarm history for inspection."""
import json
import time
import urllib.request

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def api(path, body=None, method=None):
    request = urllib.request.Request("http://127.0.0.1:8080" + path,
        data=None if body is None else json.dumps(body).encode(), method=method,
        headers={"Content-Type": "application/json"})
    with opener.open(request, timeout=10) as response:
        result = json.load(response)
    if result["code"]:
        raise RuntimeError(result)
    return result["data"]


info = api("/api/system/info")
assert info["ready"] and info["engine_state"] == "ONLINE", info
camera = api("/api/cameras", {"name": "Learning smoke", "rtsp_main": "rtsp://learning/main",
                             "rtsp_sub": "rtsp://learning/sub"})["id"]
task = api("/api/tasks", {"name": "Learning smoke", "enabled": False, "camera_ids": [camera],
    "algorithms": [{"algo_id": "person", "fps": 5, "threshold": .6}],
    "rules": [{"key": "entry", "algo_id": "person", "capability_id": "region_in",
               "roi": [[0, 0], [1, 0], [1, 1], [0, 1]]}]})
enabled = api(f"/api/tasks/{task['id']}/enabled", {"revision": task["revision"], "enabled": True}, "PUT")
assert enabled["applied"], enabled
for _ in range(50):
    events = api(f"/api/events?camera_id={camera}")
    if any(event["task_id"] == task["id"] for event in events):
        break
    time.sleep(.1)
else:
    raise RuntimeError("No task alarm received")
api(f"/api/tasks/{task['id']}/enabled", {"revision": enabled["revision"], "enabled": False}, "PUT")
api(f"/api/cameras/{camera}", method="DELETE")
print("PASS: HTTP -> draft -> activation -> fake inference -> source alarm -> SQLite -> disable")
