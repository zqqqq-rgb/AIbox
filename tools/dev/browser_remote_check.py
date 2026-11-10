"""Browser checks against verify_remote_stack.py. Requires Python Playwright.

Scan responses are intercepted: this script never scans a real subnet.
"""
import argparse
import json
import time
from pathlib import Path

from playwright.sync_api import sync_playwright, expect


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:18092")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    checks, errors = [], []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, executable_path=playwright.chromium.executable_path)
        context = browser.new_context(viewport={"width": 1440, "height": 1000}, ignore_https_errors=True)
        context.add_init_script("localStorage.setItem('aibox_token', 'local-browser-verification-only')")
        page = context.new_page()
        page.set_default_timeout(8000)
        page.on("pageerror", lambda error: errors.append(str(error)))
        page.goto(args.base + "/#/tasks")
        expect(page.get_by_role("heading", name="登录 AIBox")).to_be_visible()
        page.get_by_label("访问凭据").fill("incorrect-test-credential")
        page.get_by_role("button", name="登录", exact=True).click()
        expect(page.locator(".access-card [role=alert]")).not_to_be_empty()
        checks.append("unauthorized shell and incorrect credential")
        bootstrap_failed = []

        def bootstrap(route):
            if not bootstrap_failed:
                bootstrap_failed.append(True)
                route.fulfill(status=503, json={"code": 5001, "msg": "temporary bootstrap failure"})
            else:
                route.continue_()

        page.route("**/api/system/info", bootstrap)
        page.get_by_label("访问凭据").fill("local-browser-verification-only")
        page.get_by_role("button", name="登录", exact=True).click()
        expect(page.locator(".access-card [role=alert]")).to_contain_text("temporary bootstrap failure")
        page.get_by_role("button", name="重新连接", exact=True).click()
        expect(page.get_by_role("heading", name="算法任务", exact=True)).to_be_visible()
        expect(page.locator('#ws-banner')).to_be_hidden(timeout=10000)
        checks.append("authenticated bootstrap failure recovers via retry")
        cookie = next(cookie for cookie in context.cookies() if cookie["name"] == "aibox_session")
        assert cookie["httpOnly"] and cookie["sameSite"] == "Strict"
        assert page.evaluate("""async () => {
            const {websocketUrl} = await import('/connection_model.js');
            const info = (await (await fetch('/api/system/info')).json()).data;
            return new Promise(resolve => {
                const socket = new WebSocket(websocketUrl(info, '/ws/events', location));
                const timer = setTimeout(() => { socket.close(); resolve(false); }, 5000);
                socket.onopen = () => { clearTimeout(timer); socket.close(); resolve(true); };
                socket.onerror = () => { clearTimeout(timer); resolve(false); };
            });
        }""")
        if args.base.startswith("https:"):
            assert cookie["secure"]
            signaling = context.request.get(args.base + "/api/cameras/1/stream").json()["data"]["signaling"]
            assert signaling.startswith(args.base + "/media/index/api/webrtc?")
            assert context.request.post(signaling, data="test-sdp").status == 200
        assert page.evaluate("localStorage.getItem('aibox_token')") is None
        checks.append("cookie login and task page bootstrap")

        task_name = "浏览器闭环验证 " + str(int(time.time()))
        page.get_by_label("任务名称", exact=True).fill(task_name)
        page.get_by_label("浏览器验证相机 (#1)", exact=False).check()
        page.get_by_role("button", name="保存草稿", exact=True).click()
        expect(page.locator(".task-card", has_text=task_name)).to_be_visible()
        page.get_by_role("button", name="启用任务", exact=True).click()
        expect(page.locator(".task-card", has_text=task_name)).to_contain_text("运行中", timeout=10000)
        expect(page.get_by_label("任务名称", exact=True)).to_be_disabled()
        page.get_by_role("button", name="停用任务", exact=True).click()
        expect(page.get_by_label("任务名称", exact=True)).to_be_enabled(timeout=10000)
        page.get_by_role("button", name="加载 / 刷新底图", exact=True).first.click()
        expect(page.locator(".task-layout > .panel")).to_contain_text("快照不可用", timeout=10000)
        checks.append("draft creation, real REST enable/disable, snapshot failure fallback")
        page.screenshot(path=str(output / "tasks.png"), full_page=True)

        page.get_by_role("link", name="摄像头", exact=True).click()
        expect(page.get_by_role("heading", name="摄像头管理")).to_be_visible()
        interfaces = context.request.get(args.base + "/api/cameras/scan/interfaces").json()["data"]["interfaces"]
        assert interfaces
        interface = interfaces[0]
        subnet = interface["address"].rsplit(".", 1)[0]
        discovered = subnet + ".201"
        requests = []

        def scan(route):
            requests.append(route.request.post_data_json)
            route.fulfill(json={"code": 0, "data": {"devices": [{"ip": discovered,
                "xaddr": "http://" + discovered + "/onvif/device_service", "kind": "onvif",
                "confirmed": False, "source": "http_candidate"}], "target_count": 253,
                "attempted_count": 253, "elapsed_ms": 10, "timed_out": False, "warnings": []}})

        page.route("**/api/cameras/scan", scan)
        page.get_by_label("扫描网卡", exact=True).select_option(interface["name"])
        page.get_by_label("扫描范围（选填）", exact=True).fill(subnet + ".0/24")
        page.get_by_role("button", name="扫描网络", exact=True).click()
        expect(page.locator(".dev-card")).to_contain_text("候选设备，待认证确认")
        assert requests == [{"timeout_ms": 3000, "interface": interface["name"], "cidr": subnet + ".0/24"}]
        page.screenshot(path=str(output / "scan.png"), full_page=True)
        checks.append("interface/CIDR scan controls and unconfirmed candidate feedback (mock scan only)")

        response = page.evaluate("""async () => {
            const response = await fetch('/api/auth/logout', {method:'POST', headers:{'Content-Type':'application/json'}, body:'{}'});
            return response.status;
        }""")
        assert response == 200
        page.get_by_role("link", name="算法任务", exact=True).click()
        expect(page.get_by_role("heading", name="登录 AIBox")).to_be_visible(timeout=10000)
        page.get_by_label("访问凭据").fill("local-browser-verification-only")
        page.get_by_role("button", name="登录", exact=True).click()
        expect(page.get_by_role("heading", name="算法任务", exact=True)).to_be_visible()
        page.get_by_role("button", name="退出登录", exact=True).click()
        expect(page.get_by_role("heading", name="登录 AIBox")).to_be_visible()
        assert context.request.get(args.base + "/api/system/info").status == 401
        assert context.request.get(args.base + "/recordings/proof.mp4").status == 401
        if args.base.startswith("https:"):
            assert context.request.post(args.base + "/media/index/api/webrtc?app=live&stream=cam1_sub&type=play", data="test-sdp").status == 401
            assert context.request.get(args.base + "/index/api/getServerConfig").status == 404
            checks.append("TLS proxy cookie, protected media signaling and blocked media admin path")
        checks.append("revoked session gate, re-login, logout and evidence access denial")
        page.screenshot(path=str(output / "login.png"), full_page=True)
        assert not errors, errors
        browser.close()
    (output / "browser-results.json").write_text(json.dumps({"base": args.base, "checks": checks, "page_errors": errors,
        "scope": "isolated fake engine/media; scan mocked; no real video or public deployment"}, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"passed": len(checks), "page_errors": errors}, ensure_ascii=False))


if __name__ == "__main__":
    main()
