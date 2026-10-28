# AIBox 新板部署指南 V1.0

> 文档日期：2026-10-14
> 适用：将 aibox_proj（core-engine + businessd + web）+ MediaServer(ZLMediaKit) 部署到一台全新的 RK3588 板（RK3576 迁移见 §8）
> 依据：架构方案 §13/§14-V0/§15.2、板上实测记录（topeet 板）排障结论
> 验证基线：M0~M5 全绿（单测 23/23、前端 22/22、三进程冒烟 Tier A/B/C、报警-离开-回归复测、快照延迟 0.25~0.76s）

---

## 目录

```
1  部署总览（交付物清单 + 拓扑）
2  硬件与 BSP 环境检查（V0 清单）
3  系统依赖安装（apt）
4  MediaServer(ZLM) 部署与配置基线
5  源码编译与单测（板上原生）
6  算法包部署（npu_bench 实测回填）
7  启动与验证（start_aibox.sh + 冒烟 + 浏览器清单）
8  产线常驻（systemd 三 unit）
9  常见坑速查（板上实证）
10 版本锁定表（产线对齐用）
```

---

## 1. 部署总览

### 1.1 交付物清单

| 组件                   | 形态                                       | 落位                                                            |
| ---------------------- | ------------------------------------------ | --------------------------------------------------------------- |
| core-engine            | 板上编译可执行                             | `<工程>/build/core-engine`                                    |
| businessd              | 板上编译可执行                             | `<工程>/build/businessd`                                      |
| web/                   | 静态文件（零构建）                         | conf 的`static_dir` 指向（默认工程内 `web/`）               |
| MediaServer            | ZLM 二进制 + config.ini                    | 独立目录（如`/root/aibox/ZLMediaKit/release/linux/Release/`） |
| 算法包                 | `<algo_id>/manifest.json + <model>.rknn` | `/data/algorithms/`（或 `AIBOX_PACKAGES` 指定）             |
| npu_bench / rga_golden | 板上验证工具                               | `build/`                                                      |

### 1.2 运行拓扑（三进程 + 存储）

```
摄像头 RTSP(main/sub) ──► MediaServer(:8080 http / :8554 rtsp)
                            ├─► MP4 录像 → /data/recordings（on_record_mp4 → businessd :8090）
                            ├─► WebRTC → 浏览器（媒体面直连，不过 businessd）
                            └─► RTSP 回环 → core-engine（拉流解码→NPU→检测）
core-engine ── UDS /tmp/aibox-engine.sock ──► businessd（:8080 REST/:8081 WS/:8090 hook）
                                              ├─► SQLite（run/businessd.db 或 /var/lib/aibox/）
                                              └─► 快照 getSnap → /data/recordings/snap/
```

> ★ 端口为板上实测基线：ZLM http=8080、rtsp=**8554**（新板部署必须核对实际端口，见 §4/§9-1）。

---

## 2. 硬件与 BSP 环境检查（V0 清单）

逐项核对，任一缺失先解决 BSP/系统问题再继续（架构 §14-V0）：

```bash
# 1) NPU：rknpu 驱动 ≥0.9.2
cat /sys/kernel/debug/rknpu/version
cat /sys/kernel/debug/rknpu/load        # 三列=三核负载（rk3588）

# 2) 解码/RGA/内存堆设备节点
ls /dev/dri /dev/mpp_service /dev/rga 2>/dev/null
ls /dev/dma_heap/system 2>/dev/null      # dma-buf 池分配来源

# 3) 板上运行库（三件套 .so 必须在）
ldconfig -p | grep -E "librockchip-mpp|librga|librknnrt"
# librknnrt 版本必须 2.3.2（与模型转换 toolkit 严格同版本）
strings /usr/lib/librknnrt.so | grep -i "librknnrt version"

# 4) NPU 温度节点（降级 L4 数据源）
ls /sys/class/thermal/thermal_zone*/temp

# 5) 网络/存储
ip addr                                   # 千兆网口
df -h /data                               # 录像/快照存储分区
```

| 检查项             | 不过时的处理                                        |
| ------------------ | --------------------------------------------------- |
| rknpu 版本旧       | 升级 BSP 内核/rknpu 驱动，**勿硬扛**          |
| librknnrt 非 2.3.2 | 替换 2.3.2 配套 .so，否则模型加载失败               |
| 缺 dma_heap 节点   | 检查内核 CONFIG；缺则 DmaHeap 分配失败，AI 池不可用 |
| mpp_service 不存在 | 内核缺 rockchip VDEC 驱动                           |

---

## 3. 系统依赖安装（apt）

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config python3 curl rsync
# 编译依赖（CMakeLists 显式探测，缺则 configure 报错）
sudo apt install -y libjsoncpp-dev libsqlite3-dev libcpp-httplib-dev
# 硬件头文件与 ffmpeg 开发库（aibox_hw 需要）
sudo apt install -y libavformat-dev libavcodec-dev libavutil-dev
# MPP/RGA 头文件若 apt 无对应 dev 包：
#   find / -name rk_mpi.h 定位；im2d.h 缺则从 thirdparty_component/librga/include 拷到 /usr/include/rga/
# rknn_api.h 从 rknn-toolkit2 仓库 include/ 拷到 /usr/include/
```

| 头文件     | CMake 探测路径                             |
| ---------- | ------------------------------------------ |
| rk_mpi.h   | `/usr/include/rockchip` `/usr/include` |
| im2d.h     | `/usr/include/rga` `/usr/include`      |
| rknn_api.h | `/usr/include` `/usr/include/rknn`     |

库文件：`librockchip_mpp`、`librga`、`librknnrt`（find_library，缺则 FATAL_ERROR）。

---

## 4. MediaServer(ZLM) 部署与配置基线

### 4.1 获取与启动

- 版本**锁 commit**（记录到 §10 版本锁定表），交叉编译或拷贝 aarch64 二进制（OpenSSL≥1.1 + libsrtp2）。
- 老版 MediaServer 固定读**同目录 config.ini**——放一起启动：

```bash
cd /root/aibox/ZLMediaKit/release/linux/Release
./MediaServer -d &          # -d 守护进程化
```

### 4.2 config.ini 基线（板上实测对齐）

| 配置项                | 值                                 | 说明                                                                                          |
| --------------------- | ---------------------------------- | --------------------------------------------------------------------------------------------- |
| [http] port           | 8080                               | businessd REST 编排/getSnap 走此端口                                                          |
| [rtsp] port           | **8554**                     | ★ 回环拉流与快照 getSnap 用；**非 554 时必须同步进 businessd conf（§7.2，踩过的坑）** |
| [api] secret          | 部署时随机生成                     | 所有 /index/api 鉴权；businessd conf 与之匹配                                                 |
| [hook] enable         | 1                                  | on_stream_changed / on_record_mp4                                                             |
| [hook] timeoutSec     | 5                                  | hook 慢不拖媒体面                                                                             |
| hook 接收地址         | `http://127.0.0.1:8090/hook/...` | businessd HookServer                                                                          |
| [record] enableMp4    | 按代理开启                         | 只录主码流                                                                                    |
| [record] mp4MaxSecond | 300                                | 5min 切片                                                                                     |
| [record] mp4SavePath  | /data/recordings                   | **须与 businessd conf recordings_dir 一致**（回放直链 + Janitor 清理都按此目录）        |
| [rtc] port            | 8000/udp                           | WebRTC；局域网 externIP 留空自动取网卡 IP                                                     |

### 4.3 部署自检

```bash
curl -s "http://127.0.0.1:8080/index/api/getServerConfig?secret=<secret>" | head -c 200   # 有 JSON 应答
ss -ltnp | grep -E ':(8080|8554)'        # 端口核对（记下实际值）
```

---

## 5. 源码编译与单测（板上原生）

```bash
# 1) 源码同步到板（Windows 来源先治 CRLF）
rsync -a --exclude build --exclude .git <host>/aibox_proj/ board:~/aibox_proj/
# 若从 Windows 拷贝：
find . -name "*.cpp" -o -name "*.h" -o -name "*.txt" | xargs sed -i 's/\r$//'

# 2) 全量编译（hw + engine + tools + tests）
cd ~/aibox_proj
./scripts/build.sh --hw --tests          # 预期：探测 MPP/RGA/RKNN/FFMPEG 成功，编译全绿

# 3) 单元测试（预期 23/23）
ctest --test-dir build --output-on-failure

# 4) 纯逻辑门禁（可选，x86/板上均可）
./scripts/check-headers.sh               # aibox_core 无硬件头文件 → OK
```

产物：`build/core-engine`、`build/businessd`、`build/npu_bench`、`build/rga_golden`。

---

## 6. 算法包部署

### 6.1 包结构（目录即算法）

```
/data/algorithms/
└── person-det/
    ├── manifest.json
    └── yolov8n.rknn
```

manifest.json 关键字段（`platform` 必须匹配编译目标 AIBOX_TARGET_PLATFORM，默认 rk3588）：

```json
{
  "algo_id": "person-det", "version": "1.0.0", "platform": "rk3588",
  "model_file": "yolov8n.rknn", "input_w": 640, "input_h": 640,
  "num_cls": 80, "default_threshold": 0.45, "core": "steady",
  "classes": [0], "class_names": {"0": "person"}
}
```

校验不过的包 → disabled 置灰（registry_ready 上报原因），**不拖垮启动**。

### 6.2 npu_cost_ms 实测回填（口径纪律）

```bash
# 只测 rknn_run 一段（纯 NPU），非端到端——预算口径勿混（坑 K12）
./build/npu_bench --model /data/algorithms/person-det/yolov8n.rknn --core steady --loop 200
# 输出 manifest 回填片段 → 回写 manifest.json 的 npu_cost_ms → 重启 engine 生效
```

### 6.3 RGA 黄金对照（换板/换 BSP 后必做一次）

```bash
python3 scripts/gen_test_assets.py --w 640 --h 480 --frames 1 --out-dir /tmp
./build/rga_golden --frame /tmp/synthetic.nv12 --w 640 --h 480 --model-w 640 --model-h 640 --dma
# 几何判据 scale/pad 差 ≤0.5px 即 PASS（exit 0）；失败 = RGA 用法/BSP 异常
```

---

## 7. 启动与验证

### 7.1 快速启动（开发/演示模式）

```bash
./scripts/start_aibox.sh                 # 默认 board 模式（真件回环拉流）
./scripts/start_aibox.sh --fake          # 无摄像头/ZLM 也能出页看框（合成流+假推理）
./scripts/start_aibox.sh status|stop|restart
tail -f run/engine.log run/biz.log       # 日志
```

### 7.2 环境变量（首次生成 conf 前设置）

| 变量                          | 默认                        | 说明                                                                                                        |
| ----------------------------- | --------------------------- | ----------------------------------------------------------------------------------------------------------- |
| AIBOX_ZLM_SECRET              | 空                          | ★ 必配，否则添加摄像头 4002                                                                                |
| AIBOX_ZLM_PORT                | 8080                        | ZLM http 端口                                                                                               |
| **AIBOX_ZLM_RTSP_PORT** | 554                         | ★★ 板上 ZLM 用 8554 时**必须显式传**——否则快照 getSnap 回环打错端口全部"截图失败"（本板踩坑实证） |
| AIBOX_PACKAGES                | board 模式 /data/algorithms | 算法包目录                                                                                                  |
| AIBOX_HTTP_PORT / WS / HOOK   | 80 / 8081 / 8090            | businessd 三端口                                                                                            |
| AIBOX_RECORDINGS              | /data/recordings            | 录像+快照目录（自动建）                                                                                     |
| AIBOX_DB                      | run/businessd.db            | SQLite 路径                                                                                                 |

conf 生成于 `run/businessd.conf`（已有 conf 时不覆盖，改端口/secret 直接编辑后 restart）。

### 7.3 冒烟验证（DoD）

```bash
./scripts/build.sh --hw --tests

# Tier A（无 ZLM）：同步/API/WS/重启重建 12 项断言
./scripts/smoke_m4.sh
# Tier B/C（ZLM 在跑）：摄像头全链路 + ffmpeg 推流真件回环
SMOKE_ZLM_SECRET=<secret> SMOKE_ZLM_RTSP_PORT=8554 ./scripts/smoke_m4.sh

# 前端契约冒烟（Tier A）
./scripts/smoke_m5.sh                    # 静态托管/SYNCING/stream 信令/WS meta D3 契约等
```

> smoke 用临时端口 18080/18081/18090 与临时 DB，不污染生产 conf；ZLM 代理由脚本自清理。

### 7.4 浏览器验收（人工 11 项，web/README.md 全表）

核心路径：

```
http://<板IP>:<http_port>/  →  #/grid 宫格
1. 摄像头页：扫描网络/手工添加（自动建 ZLM 代理 + 下发 engine）
2. 配置页：勾选 person-det、画 ROI（截图底图 + 顶点拾取）→ 保存热生效
3. 宫格：子码流 WebRTC 出画面 + 检测框压人（换网络免校准）
4. 事件页：触发报警 → 列表实时头插 → ~2s 快照徽标自动回填"有快照" → 详情大图
5. 算法页：disabled 置灰 / registry_ready 推送刷新
6. 回放页：segments 检索 + MP4 直链播放
```

### 7.5 报警链路专项复测（本版修复的验收线）

```bash
# 人走出画面 ≥3s 再走回 ROI：应再次报警且快照落盘
grep "reappear" run/engine.log           # 重现重置日志
ls -t /data/recordings/snap/ | head      # 快照文件
# 快照延迟（文件名 ts vs mtime）：
python3 -c "
import os, glob, datetime
for f in sorted(glob.glob('/data/recordings/snap/cam*_*.jpg'))[-5:]:
    ts = int(os.path.basename(f)[:-4].split('_')[-1])
    d = (datetime.datetime.fromtimestamp(os.path.getmtime(f))
         - datetime.datetime.fromtimestamp(ts/1000)).total_seconds()
    print(f'{os.path.basename(f)}  延迟={d:.2f}s')     # 基线 0.25~0.76s
"
```

---

## 8. 产线常驻（systemd 三 unit）

`/etc/systemd/system/` 下三个 unit（架构 §13.1；开发期用 start_aibox.sh，产线切 systemd）：

```ini
# zlm.service —— 版本锁定，绝不随业务迭代升级
[Unit]
After=network-online.target
[Service]
WorkingDirectory=/root/aibox/ZLMediaKit/release/linux/Release
ExecStart=/root/aibox/ZLMediaKit/release/linux/Release/MediaServer -d
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target

# aibox-engine.service
[Unit]
After=zlm.service
[Service]
WorkingDirectory=/root/aibox_proj
ExecStart=/root/aibox_proj/build/core-engine --packages /data/algorithms --uds /tmp/aibox-engine.sock --uds-idle-ms 5000
Restart=always
RestartSec=3
[Install]
WantedBy=multi-user.target

# aibox-business.service
[Unit]
After=zlm.service aibox-engine.service
[Service]
WorkingDirectory=/root/aibox_proj
ExecStart=/root/aibox_proj/build/businessd --config /root/aibox_proj/run/businessd.conf
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now zlm aibox-engine aibox-business
```

要点：不使用 Requires/BindsTo（三进程独立自愈）；进程崩 = 其余按能力继续服务，systemd 兜底拉起；businessd 重启后 UDS 全量重同步（engine 无持久状态）。

---

## 9. 常见坑速查（板上实证）

| #  | 症状                             | 根因 → 处理                                                                             |
| -- | -------------------------------- | ---------------------------------------------------------------------------------------- |
| 1  | 报警正常但快照"截图失败"         | ZLM rtsp 实际 8554，conf 缺`zlm.rtsp_port` → 补 `"rtsp_port": 8554`（§7.2）        |
| 2  | 添加摄像头 4002                  | ZLM secret 未配/不匹配 → conf`zlm.secret` 与 config.ini 一致                          |
| 3  | 编译报 rk_mpi.h/im2d.h not found | 头文件路径 → §3 探测表，find 定位后补拷                                                |
| 4  | rknn_init fail / 段错误          | librknnrt 非 2.3.2，或 rknn 模型与 toolkit 版本不配套 → 换 2.3.2 配套                   |
| 5  | NPU 只用 1/3 算力                | ctx 未显式绑核（K11）→ 工程已内建 set_core_index，勿删                                  |
| 6  | 页面白屏/JS 旧逻辑               | 浏览器缓存 → rsync 后 Ctrl+F5 强刷                                                      |
| 7  | smoke 报端口占用/残留进程        | 上次冒烟未清场 → `pkill -f core-engine                                                  |
| 8  | ZLM 出现无限重试的死代理         | retry_count=-1 孤儿代理 → delStreamProxy 清理（smoke 已自清理）                         |
| 9  | 事件页 WS 行一直"无快照"         | 正常现象（截图晚于广播 ~0.5s）→ 页面 2s 自动回填；一直无 → 查 biz.log 截图失败原因     |
| 10 | getSnap 返回 PNG 占位图          | 拉的流不存在（cam id/代理名错）→ getMediaList 核对代理名`cam{id}_sub`                 |
| 11 | Windows 拷入源码编译全屏报错     | CRLF →`sed -i 's/\r$//'`（§5）                                                       |
| 12 | 长期运行磁盘涨满                 | Janitor 按 keep_days + 90% 水位自动清理（60s 巡检）；确认 conf recordings_dir 指向大分区 |

## 10. 版本锁定表（产线对齐用）

| 组件                     | 版本                                   | 校验方式                                          |
| ------------------------ | -------------------------------------- | ------------------------------------------------- |
| rknn-toolkit2（PC 转换） | 2.3.2                                  | pip freeze                                        |
| librknnrt.so（板端）     | 2.3.2 配套                             | cat /sys/kernel/debug/rknpu/version + strings .so |
| rknn_model_zoo           | 2.3.2 tag                              | git tag                                           |
| ZLMediaKit / MediaServer | 固定 commit（部署时记录）              | git rev-parse + 二进制 md5                        |
| MPP / librga             | 板卡 BSP 系统库（不自编译）            | ldconfig -p                                       |
| 内核                     | 5.10/6.1-rockchip BSP                  | uname -r                                          |
| manifest.platform        | = AIBOX_TARGET_PLATFORM（默认 rk3588） | manifest.json                                     |

---

## 附：一键部署序（新板从零到出框）

```bash
# ① V0 检查（§2）→ ② apt 依赖（§3）→ ③ ZLM 部署+config（§4）
# ④ 同步源码→治 CRLF→build --hw --tests→ctest 23/23（§5）
# ⑤ 算法包→npu_bench 回填→rga_golden（§6）
# ⑥ AIBOX_ZLM_SECRET=xxx AIBOX_ZLM_RTSP_PORT=8554 ./scripts/start_aibox.sh（§7）
# ⑦ smoke_m4 Tier A→B/C → 浏览器 11 项 → 报警复测（§7.3~7.5）
# ⑧ systemd 三 unit 常驻（§8）
```
