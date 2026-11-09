"""Local learning double for ZLM control APIs; does not serve video or recordings."""
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

streams = {}
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.handle_api()

    def do_POST(self):
        self.handle_api()

    def handle_api(self):
        url = urlsplit(self.path)
        params = parse_qs(url.query)
        length = int(self.headers.get("Content-Length", 0))
        if length > 65536:
            self.send_error(413)
            return
        if length:
            params.update(parse_qs(self.rfile.read(length).decode()))
        def get(key):
            return params.get(key, [""])[0]
        with lock:
            if url.path == "/index/api/addStreamProxy":
                key = "__defaultVhost__/" + get("app") + "/" + get("stream")
                streams[key] = {"app": get("app"), "stream": get("stream")}
                result = {"code": 0, "data": {"key": key}}
            elif url.path == "/index/api/delStreamProxy":
                streams.pop(get("key"), None)
                result = {"code": 0}
            elif url.path == "/index/api/getMediaList":
                result = {"code": 0, "data": list(streams.values())}
            elif url.path == "/index/api/getServerConfig":
                result = {"code": 0, "data": [{}]}
            else:
                result = {"code": -1, "msg": "Learning mock: video, snapshots and recordings are unavailable"}
        data = json.dumps(result).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", 18080), Handler).serve_forever()
