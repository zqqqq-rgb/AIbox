# AIBox 板端 MediaServer(ZLMediaKit) 部署与使用手册 V1.0

> 文档日期：2026-09-23
> 适用环境：RK3588 板卡（topeet iTOP-3588，Ubuntu 22.04 / glibc 2.35 / GCC 11.4 / aarch64）
> 编译主机：Ubuntu 20.04 VM（交叉编译）或板卡原生编译
> 上游文档：《AIBox 软件方案 · 独立式进程架构 V1.1》§13（部署基线）、《AIBox 软件详细设计_独立式_V1.0》§4（MediaServer 集成）
> 定位：**实操手册**——从零到"摄像头→板端 ZLM→网页预览"跑通的全步骤，含已踩坑记录。对应架构 §14 V0~V1 阶段的 MediaServer 部分。

---

## 目录

```
1  部署形态总览
2  准备工作（环境确认）
3  获取 MediaServer 的两条路径（交叉编译 / 板上原生编译）
4  板上部署与配置
5  启动与拉摄像头（编排序列实操）
6  播放验证（ffplay / VLC / 网页）
7  已踩坑记录（问题→原因→解法）
8  下一步：从手工验证到产品化部署
附录A  常用 API 速查
附录B  config.ini 关键配置项说明
```

---

## 1. 部署形态总览

当前验证阶段的目标形态（架构 §3.1 的三进程之一，businessd/core-engine 尚未开发，由手工操作替代）：

```
海康摄像头 192.168.1.64 ──RTSP──► 板 192.168.1.8 (MediaServer)
                                      ├─ HTTP-FLV :8080 ──► 浏览器/ffplay
                                      ├─ RTSP     :8554 ──► ffplay/(未来)core-engine
                                      └─ MP4录像  (V1验证项, 暂未开启)
```

**测试网络拓扑**（三机同网段）：

| 设备 | IP | 角色 |
|---|---|---|
| 海康摄像头 HK-H3S4M-P | 192.168.1.64 | 流源（主/子码流双 RTSP） |
| RK3588 板（topeet） | 192.168.1.8 | MediaServer 运行 |
| VM / PC | 192.168.1.7（VM） | 交叉编译机 + 浏览器/播放验证 |

---

## 2. 准备工作（环境确认）

### 2.1 板卡信息确认（已实测）

```bash
root@topeet:~# ldd --version      # glibc 2.35 (Ubuntu 22.04)
root@topeet:~# uname -a           # 6.1.141 aarch64
root@topeet:~# gcc -v             # 11.4.0 (板上原生工具链也可用)
```

**glibc 2.35 结论**：VM(Ubuntu20.04, glibc 2.31) 的交叉工具链产物**可正常运行**（glibc 向后兼容：2.31 ≤ 2.35）。产线推荐 docker ubuntu:22.04 容器编译（工具链与板严格同版）。

### 2.2 摄像头信息（已实测）

```
型号: HK-H3S4M-P (海康系, 4MP, 支持 ISAPI/SDK/GB28181/RTSP)
主码流: rtsp://admin:<密码>@192.168.1.64:554/Streaming/Channels/101
子码流: rtsp://admin:<密码>@192.168.1.64:554/Streaming/Channels/102
```

> 密码含特殊字符（如尾部 `-`）时，在 URL 中注意转义问题；实测将密码改为纯字母数字（如 mtnaibox1）最省事。

### 2.3 版本锁定登记（架构 §15.2 要求，V1 时回填）

| 项 | 值 | 备注 |
|---|---|---|
| ZLMediaKit 源码 | master 分支（进入 V1 前锁 commit 并记录 rev-parse） | VM 上 `~/ZLMediaKit` |
| 交叉工具链 | Ubuntu 20.04 gcc-aarch64-linux-gnu (GCC 9.4, glibc 2.31) | 产线换 docker 22.04 |
| MediaServer 二进制 | 待记录 md5 | 部署时 `md5sum MediaServer` |

---

## 3. 获取 MediaServer 的两条路径

### 3.1 路径 A：VM 交叉编译（产线形态，已验证）

**步骤 1：VM 安装交叉工具链**

```bash
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake make git perl
```

**步骤 2：编写工具链文件** `~/aarch64.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

**步骤 3：最小版编译**（不带 WebRTC，验证工具链）

```bash
cd ~/ZLMediaKit
mkdir -p build-aarch64-min && cd build-aarch64-min
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=~/aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_OPENSSL=OFF \
  -DENABLE_WEBRTC=OFF \
  -DENABLE_TESTS=OFF \
  -DENABLE_OBJCOPY=OFF          # ★关键：见坑1
make -j$(nproc) MediaServer
file release/linux/Release/MediaServer   # 期望: ARM aarch64 ELF
```

**步骤 4：传输到板**

```bash
# 注意方向: 从 VM 执行, 拷向板子(192.168.1.8)
scp -r release/linux/Release root@192.168.1.8:/tmp/zlm/
# 首次连接: 指纹确认输 yes, 再输板子密码
```

### 3.2 路径 B：板上原生编译（快速验证形态）

板子自带 GCC 11.4，编译几分钟完成，且天然无工具链/glibc/依赖部署问题：

```bash
# 板上执行(需联网)
sudo apt install -y g++ cmake make libssl-dev libsrtp2-dev
cd ~/ZLMediaKit && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF
make -j8 MediaServer
```

**两条路径的使用策略**：原生编译用于快速迭代验证；交叉编译为产线交付形态（锁 commit + md5 可追溯）。当前阶段用路径 A 产物继续。

### 3.3 完整版编译（含 WebRTC，V1 阶段需要时执行）

最小版验证通过后再做。依赖需先交叉编译到统一 sysroot（`~/aibox-cross/sysroot`）：

```bash
# OpenSSL 3.0.x
cd openssl-3.0.15
./Configure linux-aarch64 --prefix=$HOME/aibox-cross/sysroot \
  --cross-compile-prefix=aarch64-linux-gnu- shared no-tests
make -j$(nproc) && make install_sw

# libsrtp2 2.6.0
cd libsrtp-2.6.0
./configure --host=aarch64-linux-gnu --prefix=$HOME/aibox-cross/sysroot \
  --enable-openssl --with-openssl-dir=$HOME/aibox-cross/sysroot
make -j$(nproc) && make install

# ZLM 完整版
cd ~/ZLMediaKit && mkdir -p build-aarch64 && cd build-aarch64
cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=~/aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_OPENSSL=ON -DENABLE_WEBRTC=ON \
  -DOPENSSL_ROOT_DIR=$HOME/aibox-cross/sysroot \
  -DCMAKE_FIND_ROOT_PATH=$HOME/aibox-cross/sysroot \
  -DENABLE_TESTS=OFF -DENABLE_OBJCOPY=OFF \
  -DCMAKE_BUILD_RPATH='$ORIGIN/lib' -DCMAKE_INSTALL_RPATH='$ORIGIN/lib'
make -j$(nproc) MediaServer
# ★cmake 输出必须出现: "found library: libssl..." 和 "found library: libsrtp2..."
#  否则 WebRTC 没编进去，停止排查依赖

# 打包(二进制+依赖库随行)
mkdir -p ~/zlm-pkg/lib && cd ~/ZLMediaKit/build-aarch64/release/linux/Release
cp MediaServer config.ini default.pem -t ~/zlm-pkg/ && cp -r www ~/zlm-pkg/
cp ~/aibox-cross/sysroot/lib/libssl.so.3* ~/aibox-cross/sysroot/lib/libcrypto.so.3* \
   ~/aibox-cross/sysroot/lib/libsrtp2.so.1* ~/zlm-pkg/lib/
tar czf ~/zlm-aarch64.tar.gz -C ~/zlm-pkg .
```

---

## 4. 板上部署与配置

### 4.1 部署位置

```bash
# 当前验证期: /tmp/zlm/Release (临时, 重启丢失)
# 产品化: /opt/aibox/zlm/ (架构 §12 附录, systemd 管理, 见 §8)
```

### 4.2 首次启动与配置修改

```bash
cd /tmp/zlm/Release
./MediaServer          # 首次运行生成 config.ini, Ctrl+C 退出
vi config.ini
```

**验证期必改两项**（避开特权端口，root 也能绑但便于区分排障）：

```ini
[http]
port=8080        # 原值 80

[rtsp]
port=8554        # 原值 554
```

**其余配置**验证期保持默认。产品化基线（录像/hook 段）见附录 B，待 businessd 开发时启用。

### 4.3 配置文件红线（踩坑教训）

> **config.ini 必须与 MediaServer 同目录**（ZLM 按"可执行文件所在目录"查找）。
> **不要用 Windows 编辑后拷贝**——CRLF 行尾会让 secret 等值尾部带 `\r`，肉眼看不出但鉴权永远失败（见坑 3）。若已污染：`sed -i 's/\r$//' config.ini`。

---

## 5. 启动与拉摄像头（编排序列实操）

### 5.1 启动

```bash
cd /tmp/zlm/Release
./MediaServer          # 前台跑, 方便看日志(验证期推荐)
# 日志出现 http(8080)/rtsp(8554)/rtmp(1935) listen = 启动成功
```

### 5.2 添加摄像头代理（另开 SSH 终端）

这组命令就是架构 §4.3 编排序列的手工版（未来 businessd 自动执行）：

```bash
SECRET=$(grep -A3 '^\[api\]' /tmp/zlm/Release/config.ini | grep secret | cut -d= -f2)
echo "secret=$SECRET"    # ★先确认取到值(坑2)

# 主码流代理
curl -X POST "http://127.0.0.1:8080/index/api/addStreamProxy" \
  -d "secret=$SECRET" -d "vhost=__defaultVhost__" -d "app=live" \
  -d "stream=cam1_main" \
  -d "url=rtsp://admin:mtnaibox1@192.168.1.64:554/Streaming/Channels/101" \
  -d "rtp_type=0" -d "enable_hls=0" -d "enable_mp4=0" -d "retry_count=-1"

# 子码流代理
curl -X POST "http://127.0.0.1:8080/index/api/addStreamProxy" \
  -d "secret=$SECRET" -d "vhost=__defaultVhost__" -d "app=live" \
  -d "stream=cam1_sub" \
  -d "url=rtsp://admin:mtnaibox1@192.168.1.64:554/Streaming/Channels/102" \
  -d "rtp_type=0" -d "enable_hls=0" -d "enable_mp4=0" -d "retry_count=-1"
```

**参数语义**：

| 参数 | 值 | 说明 |
|---|---|---|
| rtp_type | 0 | 强制 TCP 拉流（UDP 易花屏） |
| retry_count | -1 | 摄像头断电后 ZLM 自动无限重连（含指数退避） |
| enable_mp4 | 0 | 验证期不录像；产品化时主码流置 1 |
| enable_hls | 0 | 不需要 HLS，省磁盘 IO |

**成功判据**：两条返回 `"code":0` 且 `"data":{"key":"__defaultVhost__/live/cam1_main"}`；MediaServer 日志出现"收到rtsp推流(代号:cam1_main)"。

### 5.3 确认流已注册

```bash
curl "http://127.0.0.1:8080/index/api/getMediaList?secret=$SECRET"
# JSON 中应看到 cam1_main / cam1_sub 两条 schema=rtsp/fmp4 的流
```

### 5.4 流地址总表（验证成果）

| 出口 | 主码流 | 子码流 |
|---|---|---|
| HTTP-FLV | `http://192.168.1.8:8080/live/cam1_main.live.flv` | `http://192.168.1.8:8080/live/cam1_sub.live.flv` |
| RTSP | `rtsp://192.168.1.8:8554/live/cam1_main` | `rtsp://192.168.1.8:8554/live/cam1_sub` |
| WS-FLV | `ws://192.168.1.8:8080/live/cam1_main.live.flv` | 同理 |

> 流命名遵守架构 §4.3 规范 `live/cam{id}_{main,sub}`——该名字是跨进程 trace_id，勿随意改。

---

## 6. 播放验证

### 6.1 ffplay（VM 或 PC 上）

```bash
ffplay "http://192.168.1.8:8080/live/cam1_sub.live.flv"                 # HTTP-FLV
ffplay -rtsp_transport tcp "rtsp://192.168.1.8:8554/live/cam1_sub"      # RTSP(低延迟)
```

### 6.2 VLC

媒体→打开网络串流，粘贴上述 URL。适合快速人工确认画面。

### 6.3 网页（player.html 部署）

```bash
# 1. 把 player.html 拷到板上 www 目录(VM 执行)
scp <player.html路径> root@192.168.1.8:/tmp/zlm/Release/www/
```

**部署前改一行**（页面里的流地址指向板上的流）：

```js
// player.html 中:
const streamUrl = '/live/cam1_sub.live.flv';   // 原为 /live/test.live.flv
```

浏览器访问 `http://192.168.1.8:8080/player.html` → 4 宫格显示摄像头实时画面。

> 注意：**浏览器直接打开 .live.flv 地址会变成"下载"**——这是正常现象（浏览器原生不播 FLV），必须经 player.html 的 flv.js 播放（详见 VM 阶段文档/讨论记录）。

### 6.4 分发能力验证（多消费者同流）

```bash
# 两个终端同时:
ffplay "http://192.168.1.8:8080/live/cam1_sub.live.flv" &
ffplay -rtsp_transport tcp "rtsp://192.168.1.8:8554/live/cam1_sub" &
```

两路同时播放，ZLM 对摄像头仍只保持一路 RTSP 会话（一次拉流多消费者分发，架构 §3.1 的实证）。

---

## 7. 已踩坑记录（按时间序）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | 交叉编译链接成功后报 `objcopy: Unable to recognise the format`，产物被删 | ZLM 链接后自动调**主机 x86 的 objcopy** 分离调试符号，不识别 aarch64 ELF | cmake 加 `-DENABLE_OBJCOPY=OFF`；如需 .debug 文件用 `aarch64-linux-gnu-objcopy` 手工处理 |
| 2 | addStreamProxy 返回 `code:-100 "Please login first"` | curl 的 `$SECRET` 变量与 config.ini 实际值不一致（变量未 export 到当前终端，或 config.ini 的 secret 非默认值） | `grep -A3 '^\[api\]' config.ini | grep secret` 查实际值；同终端赋值或明文写入；也可把 config.ini secret 改回默认值统一基线 |
| 3 | secret 看着对但鉴权仍失败（如后续遇到） | config.ini 从 Windows 拷贝带 CRLF，secret 尾部隐含 `\r` | `sed -i 's/\r$//' config.ini` 后重启；**config.ini 一律板上编辑或从 Linux 侧传** |
| 4 | 浏览器打开 .live.flv 变成下载 | 浏览器原生不支持 FLV 流，正常行为 | 用 player.html(flv.js)/VLC/ffplay 播放 |
| 5 | scp 首次连接卡在指纹确认 | SSH 首连的标准安全提示 | 输 `yes`；确认命令方向（VM→板）与目标 IP 是板子 |
| 6 | 摄像头密码含特殊字符（如尾部 `-`）时拉流异常 | RTSP URL userinfo 解析对特殊字符敏感 | 改用纯字母数字密码最省事；或对密码做 URL 编码 |

---

## 8. 下一步：从手工验证到产品化部署

当前验证完成后，向产品化迁移的事项清单（对应架构 §13 / 详设 §4）：

| # | 事项 | 时机 |
|---|---|---|
| 1 | 部署路径迁移：`/tmp/zlm` → `/opt/aibox/zlm`（持久化） | V1 开始时 |
| 2 | systemd 接管：`deploy/systemd/zlm.service`（CPUAffinity=0-3 小核，Restart=always）——架构 §13.1 | V1 |
| 3 | 配置基线落地：record 段（enableMp4 按代理开/mp4MaxSecond=300/mp4SavePath=/data/record）、hook 段（指向 127.0.0.1:8090）——附录 B | businessd 开发时 |
| 4 | 锁 commit：进入 V1 时 `git rev-parse --short HEAD` 记入版本锁定表 + 二进制 md5 | V1 |
| 5 | 完整版编译（WebRTC）：按 §3.3，供前端 WebRTC 预览 | V1 预览联调时 |
| 6 | 交叉编译产线化：docker ubuntu:22.04 容器 + Dockerfile 入库 | CI 搭建时 |
| 7 | addStreamProxy 由 businessd 自动执行（编排序列代码化，详设 §5.3） | M4 开发时 |

---

## 附录A：常用 API 速查（板上验证用）

```bash
SECRET=$(grep -A3 '^\[api\]' /tmp/zlm/Release/config.ini | grep secret | cut -d= -f2)
API=http://127.0.0.1:8080/index/api

# 查流列表(含在线状态/观看人数)
curl "$API/getMediaList?secret=$SECRET"

# 查服务器配置
curl "$API/getServerConfig?secret=$SECRET" | head -c 300

# 删除流代理(下线摄像头)
curl -X POST "$API/delStreamProxy" -d "secret=$SECRET" \
  -d "key=__defaultVhost__/live/cam1_main"

# 关闭流(仅断当前会话, 代理会自动重连)
curl -X POST "$API/close_streams" -d "secret=$SECRET" -d "stream=cam1_main"

# 断网重连测试后查看代理存活状态
curl "$API/getMediaList?secret=$SECRET" | grep -c cam1
```

## 附录B：config.ini 关键配置项（产品化基线，架构 §13.2）

| 配置项 | 验证期(当前) | 产品化 | 说明 |
|---|---|---|---|
| [http] port | 8080 | 80 | 产品化占标准端口（root/CAP_NET_BIND_SERVICE） |
| [rtsp] port | 8554 | 554 | 同上 |
| [rtmp] port | 1935 | 1935 | 不变 |
| [rtc] port | 8000/udp | 8000/udp | WebRTC（完整版才有意义），externIP 局域网留空 |
| [record] enableMp4 | 0 | 0（全局关，按代理开） | 只录主码流（addStreamProxy 处 enable_mp4=1） |
| [record] mp4MaxSecond | 300 | 300 | 5min 切片 |
| [record] mp4SavePath | 默认 | /data/record | SATA 挂载点 |
| [hook] enable | 0 | 1 | on_stream_changed/on_record_mp4 → http://127.0.0.1:8090/hook/... |
| [hook] timeoutSec | 默认 | 5 | hook 慢不拖媒体面 |
| [api] secret | 默认值 | 出厂随机化 | 开发期统一默认值减少变量 |

---

*（完）本文档随实操进展持续更新；踩坑记录 §7 是最有价值的部分，新环境部署前必读。*
