# AIBox 远程访问与扫描部署说明 V1.0

日期：2026-11-09。适用本批 `businessd`、Web 前端及 `tools/deploy/` 模板。

2026-11-10 补充：模板已通过 Nginx 1.28 的 `nginx -t`，以及本机自签 HTTPS/WSS、Cookie 授权、任务操作和媒体信令访问控制的 7 组浏览器检查。正式域名、隧道和真实 WebRTC 媒体仍待部署验收。

本批交付远程访问所需的会话授权、对外地址适配、受限相机发现和任务交互修复。当前尚无正式域名和公网服务器；本文的外网部署步骤需要在取得资源后执行。

## 1. 访问与认证

局域网开发模式保留现有默认行为：`auth_token` 为空时不要求登录。配置非空 `auth_token` 后，浏览器先显示设备登录界面。

管理员在受限配置文件中设置足够长、随机生成的凭据。浏览器提交后换取随机会话 Cookie；该凭据不写入 URL，也不由登录页面保存到 localStorage。

| 接口 | 行为 |
| --- | --- |
| `GET /api/auth/status` | 公开返回 `auth_required`、`authenticated`，不返回凭据 |
| `POST /api/auth/login` | JSON `{ "token": "设备访问凭据" }`；成功写入会话 Cookie |
| `POST /api/auth/logout` | 撤销当前会话，并清除 Cookie |
| `GET /api/auth/check` | 已授权返回 200，未授权返回 401，供反向代理子请求使用 |

会话默认 30 分钟硬过期，使用 HttpOnly、SameSite=Strict；配置 HTTPS 公网入口后增加 Secure。会话保存在内存中，业务服务重启后需要重新登录。

直接使用局域网 HTTP 时，浏览器入口端口需与配置的 `http_port` 一致。代理到不同外部端口的正式入口应配置 `public_base_url` 并使用 HTTPS；当前未提供任意 HTTP 代理 Origin 白名单。

API、截图、`/recordings` 和 WebSocket 共用会话。Cookie 写请求及 WS 握手检查 Origin；命令行可继续使用 `Authorization: Bearer <auth_token>`。退出或过期后，已有 Cookie WS 连接会被关闭。

当前实现是“配置凭据换取设备管理会话”，尚不是完整的管理员初始化、多人账号、角色权限或密码找回系统。已有 localStorage Token 兼容接口保留，但新登录流程不使用它持久保存凭据。

## 2. 公网入口配置

配置样例：`tools/deploy/businessd.remote.json.example`。

```json
{
  "auth_token": "替换为随机设备访问凭据",
  "public_base_url": "https://aibox.example.com",
  "public_ws_url": "",
  "public_media_signaling_path": "/media/index/api/webrtc"
}
```

- `public_base_url` 必须是 HTTPS 原点，可带端口，不允许用户名、密码、查询、片段和业务路径。
- 启用 `public_base_url` 时必须配置非空 `auth_token`，否则启动失败并给出原因。
- `public_ws_url` 留空时通过入口同源 `/ws/` 访问；如显式填写，必须是同主机的 WSS 原点。
- 对外地址只使用明确配置，不根据客户端可伪造的转发头动态选择。
- 前端根据 HTTPS 使用 WSS；局域网 HTTP 默认仍使用独立 WS 端口。
- 视频信令和录像 URL 使用配置的外部入口，信令不包含 ZLM 管理密钥。

## 3. Nginx 与隧道连接

模板：`tools/deploy/nginx-aibox.conf.example`。放入 Nginx 的 `http {}` 上下文，替换域名、证书和 upstream 后，执行 `nginx -t` 再加载。

模板按以下公网服务器本机端口转发：

| 公网服务器本机 upstream | 通过私有连接或隧道到现场 |
| --- | --- |
| `127.0.0.1:18080` | businessd HTTP，例如盒子 `127.0.0.1:8080` |
| `127.0.0.1:18081` | businessd WS，例如盒子 `127.0.0.1:8081` |
| `127.0.0.1:18082` | ZLM HTTP，例如盒子 `127.0.0.1:8088` |

如果 Nginx 直接运行在盒子或专网网关，按实际拓扑修改 upstream。隧道产品、服务器和正式证书尚未选定，本批没有部署隧道。

模板通过 `/api/auth/check` 保护唯一开放的 ZLM WebRTC 信令路径，显式阻断其余 `/media/` 和 `/index/api/` 入口。不要把公开的 `/api/auth/status` 用于 `auth_request`，该接口本来就返回 200。

WS 转发 Upgrade、Connection、Cookie 与 Origin。API 和证据转发到 businessd，由业务服务校验。登录入口设置请求频率限制。内部业务端口、Hook、引擎 UDS 和 ZLM 管理接口保持在受控网络内。

**WebRTC 信令代理不等于媒体中继。** 需要另行配置并验证实际媒体可达地址、端口映射或选定中继方案；仅提供三个 HTTP/WS 隧道不能保证外网视频出画面。

## 4. 摄像头发现

```text
选择盒子网卡 → 可选指定 CIDR → 选择等待时间 → 扫描 → 选择候选设备
→ 验证账号、获取取流地址 → 保存并等待应用 → 验证视频
```

| 接口 | 行为 |
| --- | --- |
| `GET /api/cameras/scan/interfaces` | 返回本机启用的非回环 IPv4 网卡 |
| `POST /api/cameras/scan` | 可选 JSON `{ "interface": "eth0", "cidr": "192.168.1.0/24", "timeout_ms": 3000 }` |

显式 CIDR 必须选择网卡，并完全位于该网卡的直连子网内。默认扫描每网卡所在 `/24` 与实际子网的交集，排除本机地址和适用的网络/广播地址。最多 1024 个候选地址，32 个并发探测，总等待范围为 500–10000 ms；目标过多应缩小范围。

同一业务进程只执行一项扫描，其余请求返回忙。返回值包含 `devices`、`target_count`、`attempted_count`、`elapsed_ms`、`timed_out`、`warnings`。界面显示实际等待时间，不使用虚假的百分比进度。

`confirmed` 表示发现时的协议确认，**不代表凭据正确、品牌确认或视频已连接**。HTTP 401/403 和 SOAP Fault 只表示候选；RTSP 及开放端口也不能单独证明摄像头型号。扫描发生在盒子网络中，不在远程用户的电脑上。

## 5. 本地验证与边界

现有开发入口仍是 `scripts/dev.ps1`。若旧 businessd 没有重启，前端会明确提示认证接口版本不一致；请重新构建，并通过原开发工具正常停止和启动演示。

本批另提供独立验证工具，不接触既有演示数据库：

- `tools/dev/verify_remote_stack.py`：Linux 内启动临时 fake 引擎、模拟媒体和业务服务，端口 18092/18093，使用临时库与测试凭据。
- `tools/dev/browser_remote_check.py`：Python Playwright 浏览器验证，需要本机已安装 Playwright 与对应 Chromium；`--output` 指定证据目录。

浏览器验证脚本拦截扫描返回，不扫描真实网段。假媒体服务不提供真实视频、截图或录像；证据授权使用测试文件验证，不能作为视频回放效果证明。

尚待现场验收：真实 ONVIF/RTSP 兼容、真实视频和模型效果、公网 TLS/隧道/WebRTC 媒体、实际网络下的扫描时延，以及完整多用户账号体系。
