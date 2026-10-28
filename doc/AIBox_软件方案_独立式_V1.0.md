# AIBox 软件方案 · 独立式进程架构 V1.1

> 文档日期：2026-09-22
> 本版本：V1.1 —— 将 V1.0 对《AIBox_软件方案_V1.0.md》（嵌入式基线，下称"嵌入式方案"）的全部引用内容合并进来，重组为**可独立阅读的完整方案**。两方案共享同一套对外契约，互切成本收敛于单一模块（§17）。
> 硬件验证平台：RK3588 开发板（量产目标 RK3576）
> 配套图：`AIBox_四路视频全流程.drawio` / `AIBox_4cam_flow.png`（对应 §12 心智模型）

**目录**

```
第一部分 总纲
  §1 设计原则：五条铁律      §2 技术选型      §3 架构总览（进程模型/选型结论/CPU分区）
第二部分 数据与通信
  §4 数据通路设计（通路/DDR账本/流编排）  §5 层间通信（边界/UDS协议）
第三部分 core-engine 设计
  §6 模块设计  §7 NPU 资源管理  §8 资源仲裁与降级  §9 数据链路追踪体系
第四部分 业务层
  §10 Web 应用层  §11 AI 算法方案（算法库/转换规范/算法包管理）
第五部分 工程保障
  §12 端到端数据流详解（心智模型）  §13 部署（systemd/config.ini）
  §14 验证计划 V0~V4  §15 RK3576 迁移与版本锁定  §16 风险登记
  §17 与嵌入式方案互切  §18 下一步
```

---

# 第一部分 总纲

## 1. 设计原则：五条铁律

1. **通路隔离**：录像（P0）、显示（P1）、AI（P2/P3）三条数据通路物理分离，任何一条拥塞/故障不影响其他通路；
2. **newest-wins**：所有视频队列有界（≤4 帧），满则丢最旧帧，永不反压生产者、永不无限增长；
3. **降级保底**：过载时按预定阶梯自降级，但**任何降级不触碰 P0 录像与拉流**——体验可以降，证据不能丢；
4. **零拷贝**：解码→RGA→NPU 全程 dma-buf，热路径零 malloc、零 CPU 内存搬运；
5. **进程边界即故障域**（独立式新增）：媒体可靠性（拉流/录像/分发）归 MediaServer，AI 迭代归 core-engine，Web/云归 businessd。任一进程崩溃，其余进程按自身能力继续服务，恢复由 systemd 兜底。

**零拷贝链澄清**（易误解点）：铁律 4 的 dma-buf 零拷贝指**解码后**的帧（VDEC→RGA→NPU），该链路完全位于 core-engine 进程内，独立式不受任何影响；跨进程传输的只是压缩 ES——它本来就要拷入 MPP 的 DMA 缓冲区，两种架构在此等价。

## 2. 技术选型（全部活跃维护组件）

| 层     | 组件                             | 版本锁定                                           | 职责                                               |
| ------ | -------------------------------- | -------------------------------------------------- | -------------------------------------------------- |
| 系统   | Rockchip BSP（Debian/Buildroot） | 内核 5.10/6.1-rockchip                             | rkvdec2 / rknpu 驱动                               |
| 媒体层 | ZLMediaKit（**独立进程**） | 固定 commit                                        | RTSP 拉流代理、码流分发、MP4 录像、WebRTC 网页播放 |
| 解码层 | Rockchip MPP                     | 板卡 BSP 自带库                                    | 硬件 VDEC，NV12 dma-buf 输出                       |
| 预处理 | librga                           | 板卡 BSP 自带库                                    | crop / letterbox / 格式转换                        |
| 推理层 | RKNN Runtime + rknn_model_zoo    | **2.3.2（toolkit 与 librknnrt 严格同版本）** | 模型推理                                           |
| 跟踪   | ByteTrack（Eigen 实现）          | 沿用 RK_VideoPipe 代码                             | track_id + Kalman 外推                             |
| 业务层 | 自研 C++17                       | cpp-httplib / SQLite3 / libmosquitto               | Web/事件/报警/OTA                                  |
| 前端   | 原生 JS 或 Vue CDN 版静态 SPA    | 无构建链                                           | 宫格预览/配置/事件中心                             |

> 弃用：RK_VideoPipe（停更 ~2 年）作为底座；仅复用其 ByteTrack 实现与 MPP→RGA→RKNN 零拷贝写法作参考。

## 3. 架构总览

### 3.1 三进程模型

```
                ┌─────────────────────────────────────────────┐
摄像头 RTSP ───►│ MediaServer（ZLMediaKit 独立进程，锁 commit）  │
（addStreamProxy │  拉流代理×2/摄像头 · MP4 录像切片 · RTSP/RTMP  │
  由 businessd   │  分发 · WebRTC 信令 · WebHook 回调源           │
  发起）         └───────┬──────────────────┬──────────────────┘
                       │MP4               │RTSP 回环 127.0.0.1
                       ▼                  │（子码流=AI；主码流=显示，可选）
                   SATA 存储               ▼
                                  ┌──────────────────────────────┐
                                  │ core-engine（AI 热路径进程）    │
                                  │ RtspPuller→MPP→RGA→NPU→Track  │
                                  │ →Rules · VOP 显示(可选)         │
                                  └──────┬───────────────────────┘
                                         │ UDS JSON-lines（§5.3）
                                         ▼
浏览器 ◄──WebRTC(直连 MediaServer)──┐  ┌──────────────────────────┐
浏览器 ◄──REST/WS────────────────────┼──┤ businessd（业务进程）      │
                                    └──┤ ZLM REST 唯一写者 · WebHook │
                                       │ 接收 · SQLite · MQTT · OTA │
                                       └──────────────────────────┘
```

| 进程        | 职责                                                              | 崩溃时损失                                               | 恢复                              |
| ----------- | ----------------------------------------------------------------- | -------------------------------------------------------- | --------------------------------- |
| MediaServer | 拉流代理、录像切片、协议分发、WebRTC 信令、WebHook 回调           | 录像缺口 ≤1 切片、预览断、AI 暂断流                     | Restart=always，<10s              |
| core-engine | 回环拉流、MPP 解码、RGA、NPU 推理、跟踪、规则、（可选）VOP 显示   | 检测框消失、（可选）HDMI 黑屏；**录像/预览零影响** | Restart，通道重建 <30s            |
| businessd   | ZLM REST 编排、WebHook 接收、UDS 北向、REST/WS、SQLite、MQTT、OTA | Web/云/编排不可用；**媒体与 AI 继续**              | Restart + UDS 全量重同步（§5.3） |

**单写者纪律**：所有 ZLM REST 写操作（addStreamProxy / delStreamProxy）**仅 businessd 发起**；core-engine 对 ZLM 是纯消费者（RTSP 拉流），不持有 ZLM 管理凭据。core-engine 保持"无持久状态、重启即白纸"性质，businessd/SQLite 是唯一权威。

**显示通路可选化**：无 HDMI 需求的部署形态，core-engine 只拉子码流（AI），不拉主码流；P1 显示开启时才按通道拉主码流。

### 3.2 选型结论：独立式换到了什么

| 维度                  | 嵌入式（对照方案）               | 独立式（本方案）                                                  |
| --------------------- | -------------------------------- | ----------------------------------------------------------------- |
| core-engine 崩溃      | 录像/预览中断至引擎重建          | **录像/预览零影响**，仅检测框消失，systemd 拉起即恢复       |
| MediaServer 崩溃      | 同上（同一进程）                 | 录像中断至拉起（<10s），当前切片可能损坏（两方案共有风险，§16）  |
| WebRTC 信令端点       | 需自建（WHEP 端点不在 C SDK 内） | **ZLM 原生端点零开发**                                      |
| WebHook / REST 管理面 | 需用 C API 重新实现等价物        | 原生（addStreamProxy / on_stream_changed / on_record_mp4 全家桶） |
| 升级耦合              | 业务迭代与 ZLM 同进程重编        | core-engine 高频迭代，MediaServer 锁 commit 不动                  |
| 回环开销              | 无                               | 压缩 ES 走本机 TCP：整机 CPU 约 +1~3%、DDR +0.25%（§4.2）        |
| 内存/日志归属         | RSS 混在一起，泄漏难定位         | 进程级天然隔离                                                    |
| 工程量                | 嵌入集成（事件/录像/信令自建）   | 回环拉流器 + 三进程 systemd 监管                                  |

**决策规则**：更看重"高频迭代的 AI 进程绝不拖累录像/预览"与"复用 ZLM 原生服务器能力" → 独立式；更看重"单进程统一编排、零回环" → 嵌入式。**性能不是分水岭**（差异 <3% 整机 CPU），V1 阶段用实测数据复核（§14）。

### 3.3 CPU 分区（RK3588 验证期按 RK3576 双大核预算跑）

| 工作负载                                          | 线程数 | 亲和                                          |
| ------------------------------------------------- | ------ | --------------------------------------------- |
| MediaServer 整进程（拉流/demux/录像/WebRTC 分发） | —     | **小核簇 0-3**（systemd CPUAffinity）   |
| core-engine：RtspPuller                           | 16     | 大核簇内（轻负载，V1 实测后可再细化到空闲核） |
| core-engine：MPP 解码回调                         | 16     | 大核簇（回调轻，VDEC 硬件干活）               |
| RGA 预处理（串行喂，控并发）                      | 2      | 大核                                          |
| NPU worker（每核一个）                            | 2      | 大核                                          |
| 后处理 NMS/decode                                 | 3      | 大核                                          |
| ByteTrack + 规则引擎                              | 1      | 大核                                          |
| Arbiter                                           | 1      | 大核（500ms tick，低频）                      |
| businessd                                         | —     | cgroup cpu.max ≤ 1 核                        |

---

# 第二部分 数据与通信

## 4. 数据通路设计

### 4.1 五条通路

| 通路                      | 优先级 | 数据路径（独立式）                                                    | 硬件                 | 抗阻塞特性                                                 |
| ------------------------- | ------ | --------------------------------------------------------------------- | -------------------- | ---------------------------------------------------------- |
| **P0 录像**         | 最高   | 主码流 ES → MediaServer MP4 切片(5min) → SATA                       | 仅 CPU(轻)/GMAC/SATA | **不解码不重编码**；core-engine/NPU/RGA 全挂录像照转 |
| **P1 显示**（可选） | 高     | 主码流 → RTSP 回环 → VDEC → NV12 dma-buf → VOP 平面直扫 → HDMI   | VDEC+VOP             | 不经 RGA/GPU 合成，省 ~1.5GB/s DDR                         |
| **P2 常驻检测**     | 中     | 子码流(480p) → RTSP 回环 → VDEC → RGA(crop+letterbox) → NPU Core0 | VDEC+RGA+NPU         | 有界队列丢旧，不反压                                       |
| **P3 触发模型**     | 中低   | 一级检测框 crop → NPU Core1                                          | NPU                  | 准入控制，深>8 丢低优先级                                  |
| **Web 预览**        | 复用   | 子码流 → MediaServer WebRTC → 浏览器；检测框走 WS 前端 canvas 叠加  | 网络（不解码）       | 服务端永不重编码                                           |

三个省带宽铁律：录像不解码、显示 VOP 直扫、AI 子码流+NV12 直入 NPU（RGA 仅 crop/letterbox）。

### 4.2 DDR 带宽账本（RK3576 32-bit LPDDR4X，有效 ~11GB/s）

| 消费者                                                                | 峰值                       |
| --------------------------------------------------------------------- | -------------------------- |
| VDEC 解码写（8 主+8 子；显示关闭时仅子码流 ~0.45GB/s）                | ~0.9 GB/s                  |
| VOP 显示读 + WebRTC 出流                                              | ~1.5 GB/s                  |
| RGA + NPU 读写                                                        | ~2.0 GB/s                  |
| CPU/网络/系统                                                         | ~0.3 GB/s                  |
| **回环 ES 拷贝（独立式新增）**：主+子 72Mbps=9MB/s × 约3次拷贝 | ~27 MB/s ≈**0.25%** |
| **合计 / 利用率**                                               | **~4.7 GB/s ≈ 43%** |

> **VDEC 能力结论**：RK3588 VDEC 标称可实时解 8K@30（~10 亿像素/秒）。16 路 640×480 子码流 = 16×640×480×25 ≈ 1.23 亿像素/秒，**只占约 12%**，解码侧富余极大，不是瓶颈。瓶颈在 **NPU 调度纪律（§7）与 DDR 带宽（本表）**；若各路换成 1080P 主码流（单路像素量×6.75）则需重新评估。

### 4.3 流编排（businessd 单写者）

**流命名规范**（架构互切不变量之一）：

```
rtsp://127.0.0.1:554/live/cam{id}_main    ← 主码流代理（录像源）
rtsp://127.0.0.1:554/live/cam{id}_sub     ← 子码流代理（AI 源）
```

**添加摄像头（编排序列）**：

```
1. businessd POST /index/api/addStreamProxy
     {app:"live", stream:"cam1_main", url:"rtsp://...Channels/101",
      enable_mp4:1, enable_hls:0, retry_count:-1}        → 记录 key_main
2. businessd POST /index/api/addStreamProxy
     {app:"live", stream:"cam1_sub",  url:"rtsp://...Channels/102",
      enable_mp4:0, enable_hls:0, retry_count:-1}        → 记录 key_sub
3. SQLite cameras 落库（新增 zlm_key_main/zlm_key_sub 两列）
4. UDS → core-engine：add_cam{id, rtsp_sub, rtsp_main?, algos[]}
5. core-engine RtspPuller 连接回环流，ZLM GOP 缓存秒开
```

`retry_count:-1` 交给 ZLM 无限重试源断线（含指数退避）；摄像头开机顺序不确定的场景无需自己编排。

**在线状态（双通道，互为印证）**：

- **权威显示**：MediaServer `on_stream_changed(regist=true/false)` WebHook → businessd 更新 `cameras.online` → UDS 推 `{"evt":"camera_status"}`
- **功能兜底**：core-engine 5s 无帧看门狗重建拉流（拉流失败 ≠ 摄像头离线——可能是流未就绪，按退避重连，不误报）

**录像与切片索引（独立式的免费收益）**：

- MediaServer `[record] mp4MaxSecond=300` 完成切片；`on_record_mp4` WebHook 携带 `file/start_time/time_len` → businessd 写 segments 表
- 嵌入式方案没有的收益：**回放检索数据天然产生**，无需为录像做任何二次开发

```sql
segments (id, camera_id, file_path, start_ts, end_ts, duration_s)
```

**RECONFIG 语义**：`set_algos` 是纯 UDS 操作，**不触及 MediaServer**——换算法配置对媒体面零交互、零中断，中断 <500ms（详见 §6.4）。

## 5. 层间通信

### 5.1 通信边界总览

全系统只有 7 种跨进程/跨设备通信边界，各自选用最合适的机制：

```
                ┌─────────────────────────────┐
摄像头 RTSP ───►│ MediaServer (ZLM)            │
                │ 拉流代理·录像·分发·WebRTC     │
                └──┬────────┬────────────────┘
              ③WebHook │     │ ②RTSP 回环(压缩ES)
              ④REST(单写者)  │
                ┌───▼────────▼─────────────┐
                │ core-engine               │
                │ MPP→RGA→NPU→Track→Rules   │
                └────────┬─────────────────┘
                    ①UDS │ JSON-lines
                ┌───────▼────────┐
                │  businessd     │
                │ SQLite·MQTT    │
                └──┬─────────┬───┘
            ⑤REST/WS│      ⑥MQTT/HTTPS
                ┌──▼───┐ ┌─▼────┐
                │浏览器 │ │云平台│
                └──────┘ └──────┘
   ⑦WebRTC：浏览器 ↔ MediaServer 直连（媒体面绕过 businessd）
   （另有 core-engine 进程内数据面：回调+队列+fd 传递，见 §6.2）
```

### 5.2 通信机制总表（开发对照实现）

| 边界                     | 机制              | 同步性        | 丢失容忍                | 有序性       | 背压                       |
| ------------------------ | ----------------- | ------------- | ----------------------- | ------------ | -------------------------- |
| MediaServer→core-engine | RTSP 回环（TCP）  | 流式          | 帧丢失容忍（GOP 恢复）  | RTP 序号     | 码率=源码率，不放大        |
| RtspPuller→解码         | 回调+函数调用     | 同步          | 不允许                  | 天然有序     | 无需                       |
| 解码→下游               | SPSC BoundedQueue | 异步          | 丢旧帧                  | FIFO         | 丢队头                     |
| →NPU                    | submit+回调       | 异步          | 按优先级丢              | 同通道保序   | 准入控制                   |
| NPU→后处理              | 结果队列          | 异步          | 丢旧                    | FIFO         | 丢队头                     |
| 后处理→事件             | 函数调用          | 同步          | 不允许                  | 串行         | 无需                       |
| engine↔businessd        | UDS JSON-lines    | 异步+ack      | 事件可丢/指令不可       | 通道级因果序 | 订阅制+发送队列上限        |
| businessd→MediaServer   | REST（127.0.0.1） | 请求-响应     | 指令重试至成功          | —           | 单写者；同名重添幂等       |
| MediaServer→businessd   | WebHook HTTP POST | 异步          | 事件可丢（可重推/补扫） | —           | hook 超时 5s，慢不拖媒体面 |
| MediaServer↔浏览器      | WebRTC            | 异步          | 帧丢失可容忍            | RTP 序号     | 带宽自适应                 |
| businessd↔前端          | REST/WS           | 请求响应/推送 | WS 断线重连补拉         | 事件带 ts    | 客户端节流                 |
| businessd↔云            | MQTT/HTTPS        | 异步 QoS1     | 不允许                  | broker 保证  | 本地缓存                   |

### 5.3 UDS 协议（core-engine ↔ businessd，JSON-lines）

**机制选择理由**：

- **UDS 而非 TCP 本地回环**：零网络栈开销、文件系统权限控制（同 uid）、对端崩溃立刻感知（EPOLLHUP）
- **JSON-lines 而非 protobuf**：事件频率低（每秒几十条）性能非瓶颈；`nc`/日志直接可读；字段演进无需两端重编译

**协议报文**：

```jsonc
// → 请求方向（businessd → core-engine），带 msg_id，core-engine 回 {"ack":msg_id,"ret":0}
{"cmd":"add_cam",    "id":1, "rtsp_sub":"rtsp://127.0.0.1:554/live/cam1_sub",
                            "rtsp_main":"rtsp://127.0.0.1:554/live/cam1_main"}  // main 可选(显示开启时)
{"cmd":"set_algos",  "id":1, "algos":[{"algo":"person-det","thr":0.5,"fps":3,"roi":[[x,y],...]},
                                      {"algo":"helmet-det","thr":0.45,"fps":3,"roi":null}]}
{"cmd":"remove_cam", "id":1}
{"cmd":"sub_meta",   "id":1}          // 订阅/退订该通道检测元数据（有 WS 客户端时）
{"cmd":"unsub_meta", "id":1}
{"cmd":"reload_algo","algo":"helmet-det"}   // 算法包升级后热加载（§11.4.4）
{"cmd":"get_metrics"}                       // 周期 1s 拉取资源指标
// ← 事件方向（core-engine → businessd），单向推送允许丢失（前端有事件对账）
{"evt":"camera_status","id":1,"online":true}
{"evt":"registry_ready","algos":[...]}          // 算法注册表就绪/变更（§11.4.2）
{"evt":"detections","id":1,"ts":...,"frame_seq":40817,
                     "targets":[{cls,conf,x,y,w,h,track_id,vx,vy}]}   // 原始分辨率坐标; vx/vy 供前端外推插值
{"evt":"event","id":1,"algo":"helmet-det","type":"no_helmet","snap":"/data/snaps/..."}
{"evt":"degrade","level":2,"reason":"ddr>70%"}
```

> 契约要点（源自架构对齐结论，§12）：detections 的 x/y/w/h **已是原始分辨率坐标**（板端完成逆 letterbox，前端零换算）；携带 `frame_seq`（帧身份，§12.6）与 `vx/vy`（Kalman 外推速度，前端 30fps 插值用，§12.5）。

**可靠性设计**：

- **可靠性分级**：请求-响应类指令带 msg_id 回 ack；事件流单向推送允许丢失
- **心跳**：双向 5s ping/pong，3 次超时判死
- **重连全量同步**：businessd 断线重连后从 SQLite 读出所有 enabled 摄像头 → 逐条 add_cam + set_algos 重建（core-engine 无持久状态，重启即白纸；businessd/SQLite 是唯一权威）
- **流控（订阅制）**：detections 是高频流（8路×3fps×N目标），仅当存在对应 WS 客户端时 businessd 才发 sub_meta，无订阅者则 core-engine 在 UDS 出口直接丢弃
- **通道级因果序**：同一通道的 set_algos 与其产生的 detections 按通道锁串行处理，不会出现"新配置生效前的旧结果在新配置 ack 之后到达"

### 5.4 媒体面与检测框流

**WebRTC 绕过 businessd 直出**：

```
浏览器 ←──WHEP 信令(HTTPS)──→ MediaServer 内置端点
浏览器 ←──SRTP 视频流──────→ MediaServer
businessd 的角色：只是"发地址的"——前端调 GET /api/cameras/{id}/stream → businessd 返回 ZLM 的 webrtc url
之后视频流量完全在 MediaServer ↔ 浏览器之间，businessd 不在路径上
```

收益：视频是全系统最大流量（8 路子码流预览 ≈ 4Mbps+），让业务进程彻底离开媒体路径——businessd 重启/卡顿时预览不断流；事件通道不会被视频流量堵塞。

**检测框流（WS 订阅制）**：

```
前端打开某路画面 → WS /ws/meta/{cam_id}（businessd）
    → businessd 发 {"cmd":"sub_meta","id":1} → core-engine 推该通道 detections
    → businessd 转发给所有订阅该通道的 WS 客户端
前端关闭画面 → unsub → core-engine 停止推送（省 UDS 带宽）
```

### 5.5 云平台：双通道分工

| 通道      | 用途                              | 理由                                                    |
| --------- | --------------------------------- | ------------------------------------------------------- |
| MQTT QoS1 | 事件上报、心跳、状态              | 断线缓存（SQLite WAL）+ broker 确认后删除，天然断点续传 |
| HTTPS     | 配置同步、OTA 触发、模型/固件下载 | 大文件断点续传、TLS 双向认证                            |

---

# 第三部分 core-engine 设计

## 6. 模块设计

### 6.1 目录与接口契约

```
core-engine/
├── common/
│   ├── DmaBuffer.h        # {fd, w, h, fmt=NV12, refcnt} + FramePool<N> 预分配帧池
│   ├── BoundedQueue.h     # SPSC ring, cap=4, push_drop_oldest() → drop_count++
│   └── Metrics.h          # EdgeMeter 原子计数器组（§9），Arbiter 采样
├── media/
│   ├── RtspPuller.h/.cpp  # 回环拉流线程（ffmpeg avformat），接口: run() 循环 → decoder_->feed(nalu, pts)
│   └── CameraChannel.h    # 通道生命周期 + 看门狗(5s无帧→teardown→指数退避重连64s封顶)
├── codec/
│   └── MppDecoder.h       # feed(nalu) → on_frame(DmaBuffer)；MPP_BUFFER_TYPE_DMA_BUF
├── infer/
│   ├── ModelRegistry.h    # load(algo)→ctx（引用计数，多通道共享）；npu_cost_ms 表
│   ├── NpuScheduler.h     # submit(job)→{ACCEPTED|DROPPED_LOW_PRI|DROPPED_BACKPRESSURE}（§7）
│   └── algos/             # 统一接口：preproc(DmaBuffer,roi)→RGA / run / postproc→Targets
│       yolov8.cc · yolo11.cc · retinaface.cc · lprnet.cc · pose.cc · cls.cc
├── track/TrackerHub.h     # 每通道 ByteTrack + Kalman 外推（供前端 OSD 插值）
├── rules/RuleEngine.h     # region_in/out · dwell · state-switch · 事件防抖窗(N秒合并)
├── arbiter/
│   ├── Monitor.h          # 500ms tick 采集指标
│   └── DegradeController.h# 五级降级阶梯 + 滞回恢复（§8）
├── ipc/UdsServer.h        # JSON-lines 协议（§5.3）
└── main.cpp               # 装配 + 信号处理 + 优雅退出（drain 全队列）
```

### 6.2 进程内数据面（热路径——传 fd 不传像素，传引用不传数据）

```
RtspPuller 线程 ──ES NALU──→ MppDecoder::feed(nalu)          [函数调用，拉流线程]
MppDecoder ──on_frame(DmaBuffer{fd,w,h,pts})──→ 两个 BoundedQueue
    ├→ 主码流队列 → VOP 显示线程(可选)     [容量4]
    └→ 子码流队列 → 抽帧器（令牌桶限速，按 detect_fps 采样）
抽帧器 ──RGA crop+letterbox──→ 新 DmaBuffer              [同步调用，<1ms]
    ──NpuScheduler::submit(job{model_id, dma_fd, roi, pri, cb})──→
NPU worker ──推理──→ job.cb(Targets)                      [回调进结果队列]
结果队列 → 后处理线程池(×3)：NMS → 逆letterbox → tracker.update() → rule.eval()  [串行完成]
    └→ 事件 → UdsServer 发送队列；检测元数据 → sub_meta 订阅者
```

| 设计点                  | 说明                                                                                                 |
| ----------------------- | ---------------------------------------------------------------------------------------------------- |
| DmaBuffer = fd + 元数据 | 队列里流动的只是 int fd + {w,h,fmt,pts}，引用计数管理生命周期，零拷贝                                |
| 每条边有明确背压语义    | 队列满→丢队头+计数；同步调用天然不积压；没有一条边会阻塞上游                                        |
| 时间戳链                | RTSP pts → DmaBuffer.pts → 推理结果 → detections.ts → 帧身份 frame_seq → 前端 pts 自动对齐（§12.5） |
| 推理异步、结果有序      | NPU submit 立即返回；同通道结果按 submit 顺序回调（单 context 串行推理保序）                         |

**控制面（配置/仲裁）**——进程内不用消息总线，直接函数调用：UdsServer 线程解析指令 → `ChannelPipeline::reconfigure()`；Arbiter 读各模块原子计数器（无锁快照）→ DegradeController → 各模块 `set_level(n)`（虚接口注册）。控制面操作全部发生在非热路径线程，通过 `std::atomic` 状态位影响热路径行为（如抽帧器读 `atomic<int> target_fps`），**绝不在热路径线程里加锁等配置**。

### 6.3 RtspPuller 关键参数（独立式最大性能坑 = 隐藏抖动缓冲）

| 参数                         | 值                 | 理由                                                                |
| ---------------------------- | ------------------ | ------------------------------------------------------------------- |
| rtsp_transport               | tcp                | 回环无重排收益，UDP 徒增乱序处理                                    |
| stimeout（新版本为 timeout） | 5s                 | 喂看门狗；卡死即重建                                                |
| max_delay / buffer_size      | 最小化             | **抑制 ffmpeg RTSP 客户端默认抖动缓冲引入的数百 ms 隐藏延迟** |
| 重连退避                     | 1→2→…→64s 封顶 | 与 ZLM 源侧重试上限对齐防风暴（§16-4）                             |

**时间戳链**：ZLM 转发保留源 RTP 时间戳 → 回环 pts → DmaBuffer.pts → detections.ts，全链路不断。

### 6.4 通道级管线状态机与 RECONFIG

**状态机**（支撑 Web"每路配算法"）：

```
INIT → LOADING(加载/复用模型context) → RUNNING ⇄ RECONFIG(drain→仅重建该通道AI部分) → STOPPED
```

RECONFIG 语义：Web 保存某通道算法配置 → 只 drain 该通道 AI 队列、增删 ModelRegistry 引用、重建抽帧/触发逻辑；**录像/显示/预览/其他通道零影响**，中断 <500ms。纯 UDS 操作，不触及 MediaServer。

## 7. NPU 资源管理

> NPU 是全流程最大的资源竞争点，同时具备三个"难管"特征：**多方竞争**（16 通道 × N 算法抢 2~3 个核）、**负载突发**（级联触发无规律）、**内存另算**（ctx 占 NPU 专属内存，与算力是两个独立瓶颈）。管理框架 = 需求侧砍需求 + 供给侧用好核 + 两道闸门。

### 7.1 竞争形态与核心洞察

```
16通道 ─┐
        ├─ 常驻检测 (持续/均匀/不可断——断了跟踪就断, 用户可见)  ┐
级联触发 ─┤  人脸/车牌/姿态/分类 (突发/可丢——晚一帧无所谓)      ├─► 2个NPU核 + NPU内存
模型内存 ─┘  每物理模型一个ctx常驻 (抢内存不抢算力)              ┘
```

核心洞察：**两类负载的"可丢性"完全不同**——整个调度设计围绕这个差异。

### 7.2 需求侧：五个杠杆（按性价比排序；多数人只盯供给侧，但需求侧更便宜且无副作用）

| 杠杆                   | 效果                                                          | 代价                           |
| ---------------------- | ------------------------------------------------------------- | ------------------------------ |
| 子码流 480p 输入       | RGA 后进 NPU 的数据量与 VDEC 开销同步降                       | 无（检测精度不受影响，§12.3） |
| 检测 fps 2~5           | **算力与 fps 严格线性，主旋钮**                         | 无（Kalman 外推补平滑）        |
| Kalman 外推 + 前端插值 | 3fps 检测呈现为 30fps 连续框                                  | 板端几十行 + 前端插值          |
| 级联触发               | 贵模型只在"有目标"时跑，无车时车牌模型 0 开销                 | 一级检测必须先出框             |
| 逻辑算法共享物理模型   | person/helmet/phone-det**一次推理全出**，勾选数不增算力 | 合训数据集（§11.4.5）         |

### 7.3 供给侧：静态分区 + 双队列 + ctx 共享

**核分区（突发隔离的根本）**：Core0=常驻检测（保底服务，round-robin 不可丢）；Core1=触发模型（机会服务，优先级+准入可丢）；Core2 默认禁用（按 RK3576 双核预算验证，留 16 路高配摸底开关）。核→队列映射由 **NpuCorePlan 配置面**承载（`--npu-cores steady=0,1:trig=2` 显式指定；缺省按 device-tree 识别 SoC：rk3588→steady=[0,1]/triggered=[2]，rk3576→[0]/[1]，未知→2 核兜底；校验失败回退双核默认并告警，不拒启）。突发全部由 Core1 准入控制吸收，Core0 节奏纹丝不动——比动态优先级算法简单可靠。配套纪律：**每个 rknn_context 创建后必须按 NpuCorePlan 显式 `rknn_set_core_mask` 绑到目标核**（steady 组内按模型构建序轮转取核，triggered 固定首核；仅单核掩码 `RKNN_NPU_CORE_0/1/2`）——不调用时默认 `RKNN_NPU_CORE_AUTO`（随机挑一个核，且是单核），等于只用 1/3 算力且随机核会直接破坏分区纪律；**禁用 RKNN 0_1/0_2 多核协同模式**（单次推理独占整个 NPU 会饿死常驻检测）；**分区双向不借道**（Core0 过载不溢 Core1，Core1 再闲不借 Core0，过载走降级不走借道）。

**双队列，两种"满"语义**（对接 §9 的 drop/sampled 分计）：

```cpp
struct Job {
    uint32_t cam_id; int64_t pts, frame_seq; int dma_fd; Roi roi;
    uint8_t pri;        // 仅触发队列: 车牌>人脸>姿态>分类
    Callback cb;
};
// rq_steady_:   满则丢最旧帧(newest-wins)——帧时效性优先
// rq_triggered_: 深度>8 → submit()直接返回DROPPED_LOW_PRI拒新——绝不排队堆积掩盖过载
enum Result { ACCEPTED, DROPPED_LOW_PRI, DROPPED_BACKPRESSURE };
```

**ctx 共享（内存侧）**：ModelRegistry 引用计数（§11.4.2）——多个逻辑算法指向同一物理模型时 NPU 里只有一个 ctx 一份权重；16 通道勾同一模型 ctx 仍是一份。factor2 内存预检**按物理模型去重求和**。

### 7.4 多常驻模型共存：分桶加权轮转（Core0 内部管理）

一个核一个时刻只跑一个 ctx 的推理（RKNN 串行执行），多常驻模型共存 = **把核抽象成"分桶加权轮转执行器"**——模型是桶、通道是桶内顾客：

```
rq_steady (Core0 执行队列)
├── 桶 yolov8n-base: [cam1帧][cam2帧]...   ← 全通道按各自fps产生任务
└── 桶 yolov8n-pose: [cam2帧][cam4帧]...   ← 只有勾了pose的通道进这桶
```

- **为什么按模型分桶而不是 FIFO 混排**：① fps 保障可计算（每桶约定服务率=Σ勾选通道fps，桶占核时间=次数×cost_ms）；② 同桶连续执行 ctx 切换最少（切换是微秒级指针/寄存器操作，权重不搬运）；③ 每通道推理间隔≈桶周期，节拍稳定供跟踪/外推依赖
- **桶间**：加权轮询（WRR），权重=桶约定服务率；**桶内**：通道 round-robin 保证公平
- **通道 fps 不同**：桶内每通道令牌桶限速，抽帧器拿到令牌才 submit——**提交率=配置 fps，超令牌帧在源头采样掉（§9 的 sampled），根本不进 NPU 队列**，队列保持浅
- **降级顺序**：阶梯①从**最贵的桶**开始降（同样降 1fps 贵模型省得多），固化顺序 pose→base（可配），保底 base 1fps 全通道——跟踪链路不能断
- worker 按桶切换 ctx（常驻不重载），`rknn_run` 直接执行

### 7.5 NPU 算力预算（INT8，按 40~50% 有效利用率折算；npu_cost_ms=28ms 实例）

> **口径纪律**：`npu_cost_ms` 只测 `rknn_run` 一段（纯 NPU 推理）——它才是算力预算的分子。**不要**把「拉流→解码→RGA→推理完成」的端到端耗时（demo 实测约 40ms）当成 `npu_cost_ms` 用，否则算力预检系统性偏低、误判过载。两者差值是 VDEC/RGA/搬运，属另一套指标（§9 环节耗时）。

| 配置          | 计算            | Core0 占用               | 结论                            |
| ------------- | --------------- | ------------------------ | ------------------------------- |
| 8 路 × 3fps  | 24 次/s × 28ms | 672ms/s =**67%**   | ✓ 甜蜜点                       |
| 8 路 × 5fps  | 40 × 28        | 1120ms/s =**112%** | ✗ 预检应拒绝                   |
| 16 路 × 3fps | 48 × 28        | 1344ms/s =**134%** | ✗ 降 2fps(90% 临界) 或开 Core2 |
| 16 路 × 2fps | 32 × 28        | 896ms/s =**90%**   | ⚠ 临界，降级阶梯兜底           |

三个推论：① 8 路 3fps 是甜蜜点；② 16 路满配必须"降 fps + Core2 摸底开关"；③ **预检必须逐通道算**——同一勾选在 8 路和 16 路下结论完全不同。

多模型数值示例（6 通道，base 全配 3fps、cam2/4/6 勾 pose 2fps@35ms）：base 桶 6×3×28=504ms/s(50%) + pose 桶 3×2×35=210ms/s(21%) = 71% ✓ 预检通过；base 桶时间轴 `[c1,c2,c3,c4,c5,c6, c1,c2,...]` 通道轮转，pose 桶 `[c2,c4,c6, c2,c4,c6]`。

全模型峰值需求（触发池含级联模型）：YOLOv8n 检测 ~1.0 + Pose ~0.5 + 人脸级联 ~0.2 + 车牌级联 ~0.2 ≈ **1.9 TOPS（占 6 TOPS 的 32%）**——预算充裕，瓶颈在调度纪律而非总量。

### 7.6 两道闸门

**闸门 1（配置时，双因子预检，§11.4.3）**：保存配置瞬间拒绝过载——factor1 算力 `Σ(去重物理模型 cost_ms × Σ勾选通道fps) < 核预算×80%`、factor2 内存 `Σ(去重ctx内存) < NPU总内存×80%`，超限警告+建议方案。npu_cost_ms 必须是**板端实测值**（loop 100 中位数，§11.3）——全部预算可信的根基；模型热加载后用 selftest 顺带刷新，防成本漂移。

**闸门 2（运行时，降级阶梯①③，§8.2）**：rq_steady 积压 5s → 逐通道 fps 5→3→1（按 §7.4 从贵桶开始）；NPU>85% → 暂停整个触发池（常驻检测+录像不受影响，触发本就是机会性工作）。

### 7.7 fd 生命周期与微观纪律（零拷贝架构的隐蔽坑）

- **fd 生命周期**：Job 持有 dma-buf 引用直到推理回调返回；队列深度上限（8）同时是内存上界（8×640×640×1.5B≈5MB 可控）；**丢帧必须同步释放 fd 引用**，否则 FramePool 预分配帧池被耗尽——零拷贝架构最隐蔽的泄漏点
- **保序**：单 ctx 串行推理天然保序（§6.2），detections 按提交顺序出，前端插值依赖此，不为吞吐引入乱序
- **测量**：`/sys/kernel/debug/rknpu/load`（三列=三核）——禁用的 Core2 应恒为 0，是"分区纪律没被破坏"的直接证据；两队列各挂 EdgeMeter（§9），**DROPPED_LOW_PRI 丢弃率 >1% 是 V2 验收红线**
- **工程定位**：RKNN 只提供 rknn_run 原语，NpuScheduler（分桶/令牌/准入/降级联动）是**全自研模块，core-engine 中算法含量最高的部分**

## 8. 资源仲裁与降级

### 8.1 指标采集（Monitor，500ms tick）

```
npu_load    ← /sys/kernel/debug/rknpu/load（三列=三核）
ddr_bw      ← rockchip-dfi 事件计数换算（内核路径 V0 阶段确认适配）
temp        ← /sys/class/thermal/thermal_zone*/temp
queue_stats ← 各 BoundedQueue/EdgeMeter 内省 {depth, in, out, drop, sampled}
vdec_delay  ← MPP 帧时间戳 vs 系统时钟
zlm_rss/cpu ← /proc/<pid>（独立式增补：MediaServer 进程）
proxy_state ← getMediaList 1s 轮询，重试计数进健康指标（独立式增补）
```

### 8.2 五级降级阶梯（滞回恢复，防抖动）

| 级 | 触发             | 动作                                                                      | 恢复            |
| -- | ---------------- | ------------------------------------------------------------------------- | --------------- |
| ① | NPU 队列积压 5s  | 检测帧率 5→3→1fps（按通道逐级，从最贵模型桶开始）                       | 水位<3 持续 30s |
| ② | DDR>70% 持续 10s | 显示/Web 预览抽帧 30→15fps                                               | <55% 持续 60s   |
| ③ | NPU>85%          | 暂停触发式模型（检测与录像不停）                                          | <60%            |
| ④ | 温度>85℃        | AI 降 1fps + CPU 限频                                                     | <75℃           |
| ⑤ | 磁盘/上报积压    | 快照降质、事件限流；segments 水位 >90% 按时间序删最旧切片（保留天数可配） | 排空            |

### 8.3 压测与故障注入验收线（V3）

**72h 烤机（8 路满载）**：DDR<70%、无持续队列堆积、**录像零丢帧**、AI 丢帧率<1%、RSS 增长<5%、无 OOM/死锁。故障注入手段：灌爆 NPU 队列（验①③）、stress-ng 抢内存（验②）、锁频+捂散热（验④）、拔盘+断网（验⑤）。**RSS 按进程分别考核**（MediaServer 以自身部署基线为参照）。

**三进程故障注入矩阵（独立式新增，对应故障域设计）**：

1. `kill -9 core-engine` ×100：录像与预览 **100% 零中断**（切片帧级连续性校验）
2. `kill -9 MediaServer` ×20：录像缺口 ≤1 切片、自动恢复 <10s、core-engine 断流自愈率 100%
3. `kill -9 businessd`：媒体/AI 不中断；重启后 UDS 全量同步正确、segments 补扫一致

## 9. 数据链路追踪体系（实现规范）

> 管理原则：**生产运行时不逐帧追踪（太贵），三层体系分工——通道级状态机（健康判定）+ 环节级计数器（堵点定位）+ 帧级时间戳（因果凭证，仅推理路径）**。

**第 1 层：通道级（CameraChannel 户口本）**。每路摄像头一个属主对象，链路各环节是其成员，各自暴露状态字段（puller 会话/重连计数、decoder 最后出帧时刻、各队列内省、检测 fps 实际值、活跃 track 数）。管理动作全部发生在通道粒度：看门狗只 teardown 该路、RECONFIG 只 drain 该路 AI 队列（§6.4 状态机）。回答"**这路摄像头现在健康吗**"。

**第 2 层：环节级（EdgeMeter，Metrics.h 的具体化）**。数据每流过一条边，两端原子计数器自增——帧不需要身份证，**计数器差分即可定位问题**：

```cpp
struct EdgeMeter {              // 挂在每条边(队列/函数调用边界)上
    std::atomic<uint64_t> in, out;         // 进出帧数
    std::atomic<uint64_t> drop;            // 异常丢弃(有界队列 newest-wins)
    std::atomic<uint64_t> sampled;         // 设计内采样丢弃(抽帧器 25→3fps)——与 drop 分计!
    std::atomic<uint64_t> latency_us_sum;  // 入队/出队时间戳差累计 → 平均滞留
};
// 不变量: in == out + drop + sampled(允许短暂窗口抖动); 违反即数据意外丢失
```

每通道的边集：`拉流器→解码器→有界队列(主/子)→抽帧器→NPU提交→后处理→UDS出口`。Monitor 500ms 采样全量快照，双消费：Arbiter 降级决策（§8.2 阈值①）+ `get_metrics` 上 Web 资源面板。回答"**数据在哪一环堵了/丢了**"。

**排障手法（计数器差分定位）**：不重现、不抓包，按边扫描看 in/out/drop 从哪一环开始不收敛。示例——第 3 路黑屏：

```
环节            in     out    drop   sampled  诊断
拉流器          25fps  25fps  0      0        ✓
解码器          25fps  25fps  0      0        ✓
子码流队列       25fps  25fps  0      0        ✓
抽帧器          25fps  3fps   0      22/s     ✓ 按检测fps采样, 设计行为
后处理出口       3fps   0fps   0      0        ✗✗ 断点在此: 后处理卡死/回调丢失
```

**第 3 层：帧级（frame_seq + pts，仅推理路径）**。帧身份作为数据一部分随帧透传（§6.2 时间戳链 / §12.6），服务于"NPU 异步乱序回调后结果与帧的因果对应"与事件取证；录像/预览路径不做逐帧追踪。

**日志规范（事后回放）**：core-engine 所有日志强制带通道上下文字段（日志框架统一注入）：

```
{"cam":3, "evt":"reconnect", "reason":"no_frame_5s", "backoff_ms":8000, "pull_attempts":3}
```

**流名即跨进程 trace_id**：`cam3_sub` 同时出现在 MediaServer 日志、core-engine 日志、businessd DB 记录中，三边日志按流名即可对齐时间线。

**端到端延迟测量线**：三个全局锚点差分——源 pts（摄像头时钟）→ vdec_delay（§8.1 解码锚点）→ detections.ts（出口锚点），得"拉流→解码"与"解码→推理出口"两段延迟，Web 面板画曲线。

**责任归属**：EdgeMeter/日志字段注入属 core-engine 工程量（约百行）；MediaServer 侧不改造（ZLM 自带日志+流名可用）；businessd 只做 get_metrics 透传与面板展示。

---

# 第四部分 业务层

## 10. Web 应用层（businessd 托管静态 SPA）

### 10.1 页面

```
摄像头管理   添加/编辑/删除 RTSP · 连通性测试 · 在线状态徽标
监控宫格     1/4/9 宫格（子码流 WebRTC）+ canvas 叠检测框；单击单路切主码流
算法库       算法注册表（人脸/工装/安全帽/打电话/站坐/车辆/车牌/跌倒…）
通道算法配置  每路勾选多算法 + 独立阈值 + ROI 多边形 + 检测帧率
             页面实时显示"NPU 预估占用 xx%"（算力预检，超 80% 警告）
事件中心     实时事件流（WS 弹窗）+ 历史检索（通道/算法/时间）+ 快照图
录像回放     segments 检索 + MP4 直链播放（独立式新增，检索 <1s）
系统设置     网络/存储/云对接/OTA/资源监控面板（NPU/DDR/温度曲线）
```

### 10.2 数据模型（SQLite）

```sql
cameras       (id, name, rtsp_main, rtsp_sub, enabled, online, last_heartbeat,
               zlm_key_main, zlm_key_sub)          -- 独立式新增两列: ZLM 代理 key
algorithms    (id, name, type /*det|cls|pose|lpr|face*/, model_ref,  -- 逻辑算法指向物理模型
               pkg_path, version, classes, default_threshold, input_size,
               npu_cost_ms, npu_mem_kb, trigger, trigger_source, enabled)
camera_algos  (camera_id, algorithm_id, threshold, roi_polygon, detect_fps, enabled)
events        (id, camera_id, algo_id, type, ts, snapshot_path, targets_json)
segments      (id, camera_id, file_path, start_ts, end_ts, duration_s)  -- 独立式新增: 回放索引
-- 注：algorithms 表由 AlgoRegistry（§11.4.2）同步刷新，业务侧只读；
--     camera_algos 中的 algorithm_id 可为逻辑算法（Web 勾选粒度）
```

### 10.3 REST / WS API

| API                                            | 说明                                                                                                                                                           |
| ---------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| POST/GET/DELETE /api/cameras                   | 摄像头 CRUD，添加即 RTSP 探测返回连通结果                                                                                                                      |
| GET /api/algorithms                            | 算法库列表                                                                                                                                                     |
| PUT /api/cameras/{id}/algos                    | 更新算法集/阈值/ROI →**热生效**（通道 RECONFIG）                                                                                                        |
| GET /api/cameras/{id}/stream?quality=sub\|main | 返回`{type:"webrtc", signaling:"http://<host>/index/api/webrtc", app:"live", stream:"cam1_sub"}`；前端用 ZLM 自带 ZLMRTCClient.js 或 50 行 WHEP 风格轻客户端 |
| GET /api/recordings?camera_id&from&to          | segments 检索 + MP4 直链（独立式新增）                                                                                                                         |
| WS /ws/events                                  | 全局事件推送（弹窗+列表）                                                                                                                                      |
| WS /ws/meta/{cam_id}                           | 通道检测元数据流（帧号+框+类别+置信度+track_id）                                                                                                               |
| GET /api/resources                             | NPU/DDR/温度实时占用                                                                                                                                           |

### 10.4 关键技术决策

1. **WebRTC 而非 RTSP/FLV**：ZLM 原生输出，浏览器零插件，延迟 300~800ms；宫格强制子码流（8×1080p 浏览器解码会卡死）
2. **OSD 客户端叠加**：视频（WebRTC）与元数据（WS）分离，前端透明 canvas 画框——服务端画框必须 VENC 重编码，违反 VENC 零占用原则，禁止
3. **框画面对齐**：视频与元数据两条通道同源但延迟不同（视频路径 300~800ms > 元数据路径 100~400ms，且随网络变化）。**弃用固定 render_delay，改为 pts 透传自动对齐**（详见 §12.5）：板端检测结果携带源帧 pts，前端用「视频缓冲前沿(video.buffered.end)锚定零点差」逐帧对齐，换任何网络环境免校准。
4. **配置热生效**：通道 RECONFIG，百 ms 级，其他通路无感
5. **算力预检前移**：配置页双因子实时预检——算力 Σ(去重后物理模型 npu_cost_ms × 各通道 fps) 与 NPU 内存 Σ(ctx 占用) 各设 80% 上限，超限警告——被动降级变主动预防（详见 §11.4.3）
6. **事件防抖**：同通道同算法同类目标 N 秒时间窗合并（快照取首帧），否则 3fps×8 路刷屏

## 11. AI 算法方案

### 11.1 算法库（首版，全部取自 rknn_model_zoo 2.3.2 官方实现）

| 算法                     | 模型                                                    | 类型      | 触发方式       |
| ------------------------ | ------------------------------------------------------- | --------- | -------------- |
| 通用目标检测（人/车/物） | **YOLOv8n 640×640 i8（基线）+ YOLO11n 并行 A/B** | 检测      | 常驻（Core0）  |
| 工装/安全帽/打电话       | YOLOv8n 自训练类别 或 一级人框+二次分类                 | 检测/分类 | 常驻或触发     |
| 站/坐识别                | 一级人框 + 轻量分类（ResNet18 级）                      | 分类      | 触发（Core1）  |
| 人脸检测+识别            | RetinaFace-m 320 + MobileFaceNet 112                    | 级联      | 检测到人脸触发 |
| 车牌识别                 | 轻量车牌检测 + LPRNet 168×48                           | 级联      | 检测到车辆触发 |
| 人体姿态/跌倒            | YOLOv8n-Pose 640                                        | 姿态      | 进入 ROI 触发  |

### 11.2 YOLOv8 vs YOLO11 结论（2026-09 评审）

纸面 YOLO11n 占优（mAP 39.5 vs 37.3，GFLOPs 6.5 vs 8.7），但 C2PSA 注意力模块对 NPU 算子友好度存疑，INT8 量化损失需实测。**决策：YOLOv8n 为基线，YOLO11n 双轨 A/B**，定型判据 = RK3588 实测三组数据：单次推理耗时（入 npu_cost_ms 表）、INT8 量化后 mAP、CPU 后处理耗时——禁用 FP32 精度或 PC 端速度做决策。YOLO12/RT-DETR 等新架构 RKNN 算子支持差，不纳入。

### 11.3 模型转换规范（PC 端 rknn-toolkit2 2.3.2）

- 转换 target：验证期 `rk3588`，量产 `rk3576`（不可混用）；
- 量化：INT8 非对称，校准集须含现场场景（逆光/夜视/雨雾各采样）；
- 每模型产出：`.rknn` + 板端实测单次耗时（**仅 `rknn_run` 一段、loop 100 取中位数，非端到端**，见 §7.5 口径纪律）→ 写入 algorithms.npu_cost_ms；
- 自训练垂类：Ultralytics 训练 → export ONNX → zoo 转换脚本，换模型仅改配置。

### 11.4 算法库管理

#### 11.4.1 算法包（Algorithm Package）——文件系统层

每个算法是 `/data/algorithms/<algo_id>/` 下的自包含目录，**目录即算法**：拷走目录 = 带走完整算法（模型+标签+后处理参数+描述），备份/迁移/回滚均为文件操作。

```
/data/algorithms/
├── yolov8n-base/              ← 物理模型：基础检测（人形相关类合训，见 11.4.5）
│   ├── manifest.yaml          ← 唯一入口，描述本算法一切信息
│   ├── models/
│   │   ├── yolov8n-base.rknn          ← rk3588 转换产物
│   │   └── yolov8n-base.rk3576.rknn   ← 量产平台模型（可选并存）
│   ├── labels.txt             ← person/vehicle/helmet/no_helmet/phone...
│   ├── postproc.yaml          ← NMS 阈值、decode 网格、置信度门槛（经验参数随包走）
│   └── selftest/              ← 3~5 张标注图 + 期望输出（升级防呆）
├── person-det/                ← 逻辑算法（model_ref 指向共享物理模型）
│   └── manifest.yaml: { model_ref: yolov8n-base, classes: [person] }
├── helmet-det/                ← 逻辑算法
│   └── manifest.yaml: { model_ref: yolov8n-base, classes: [helmet, no_helmet, head] }
├── face/                      ← 级联算法（含多模型）
├── lpr/
└── standsit/
```

**manifest.yaml 示例**：

```yaml
algo_id: helmet-det
name: 安全帽检测
version: 1.2.0                    # 语义化：模型权重变化必须升版本
type: det                         # det | cls | pose | lpr | face
model_ref: yolov8n-base           # 逻辑算法指向共享物理模型；物理模型此处为 null
target_platform: [rk3588, rk3576]
model_file: null                  # 物理模型填 models/xxx.rknn
input_size: [640, 640]
input_format: NV12                # NV12 直入 | RGB（需 RGA 转换）
framework_version: rknn-2.3.2     # 转换时 toolkit 版本，加载时校验
npu_cost_ms: 28                   # 板端实测（启动自检时刷新）
npu_mem_kb: 24576                 # ctx 内存（rknn_query 实测）
default_threshold: 0.45
default_fps: 3
classes: [helmet, no_helmet, head]
trigger: primary                  # primary=常驻 | triggered=需一级框触发
trigger_source: null              # 级联时填一级算法 id
scene_tags: [工地, 工厂, 电力]     # Web 算法库页筛选用
changelog: "1.2.0: 更换标注外包后重训，mAP +1.8"
```

#### 11.4.2 算法注册表（AlgoRegistry）——运行时层

core-engine 启动时扫描算法目录建内存注册表，businessd 经 UDS 读表暴露给 Web：

```
启动流程：
1. 扫描 /data/algorithms/*/manifest.yaml（内置算法在只读分区 /usr/share/algorithms/，扩展算法在 /data/）
2. 校验：framework_version 与板上 librknnrt 匹配？target_platform 含本机 SoC？
   模型文件存在？→ 不合格算法标记 disabled（日志告警），不阻塞启动
3. 解析 postproc.yaml → 后处理参数绑定到对应 C++ 后处理实现
4. 注册表就绪 → UDS 上报 {"evt":"registry_ready","algos":[...]} → businessd 刷库
```

运行时三级结构：

```
AlgoRegistry（注册表：manifest 数据，无状态）
     │ 按需 load()
     ▼
ModelRegistry（模型运行时：rknn_context 池 + 引用计数，按物理模型去重）
     │ get(algo_id) → shared_ptr<AlgoInstance>
     ▼
AlgoInstance = { rknn_ctx, 后处理参数, IO tensor 布局, 成本统计 }
```

加载/卸载规则（引用计数语义）：

| 事件                   | 动作                                                                               |
| ---------------------- | ---------------------------------------------------------------------------------- |
| 首个通道勾选算法 X     | `load(X)`：解析 model_ref 定位物理模型 → 创建 rknn_context（绑核按 §7.3 分区） |
| 多通道勾选同一物理模型 | **共享同一 ctx**，模型内存只占一份                                           |
| 取消最后一个引用       | 引用 -1 → 0 时释放 context、归还 NPU 内存                                         |
| Web 更新算法包         | 卸载旧 ctx → 热加载新版本（见 11.4.4）                                            |

**逻辑算法共享机制**（Web"每路单独选算法"的支撑）：

- 一个物理模型可注册多个逻辑算法（person-det / helmet-det / phone-det 均指向 yolov8n-base）；
- 通道勾选多个逻辑算法 → 引用同一物理模型 ctx，类别取并集做后处理过滤；
- **NPU 每通道只跑一次基础检测，算力不随勾选数增加**；
- 算力预检按物理模型去重计算，不虚报。

**级联依赖**：级联算法 manifest 声明 `trigger_source`（如 face 依赖 yolov8n-base 的 person 类）。配置校验发现依赖缺失时**自动勾选**一级算法（注册表记录隐式依赖），而非报错拒绝。

#### 11.4.3 NPU 内存预算（双因子预检）

```
factor1 算力：Σ(去重后物理模型 npu_cost_ms × 各通道 fps) < 核预算×80%
factor2 内存：Σ(去重后待加载算法 ctx 内存) < NPU 总内存 × 80%
任一超限 → 配置页警告并给出建议（降 fps / 减算法 / 升级型号）
```

ctx 内存来源：加载时 `rknn_query(RKNN_QUERY_MEM_SIZE)` 实测写回注册表，随元数据暴露给 Web。V2 阶段补全所有算法实测数据。

#### 11.4.4 算法包生命周期（安装/升级/回滚）

```
来源：内置（只读分区，出厂基线）| OTA 增量下发 | U 盘导入 zip

安装/升级：
1. 校验包签名（RSA）+ manifest 完整性 + target_platform 匹配
2. 解压到 /data/algorithms/<algo_id>.<version>.new/     ← 不碰在用目录
3. 原子切换：rename 旧目录 → .bak，rename .new → 正式名
4. 通知 core-engine reload(algo_id)：卸载旧 ctx → 加载新 ctx
   （引用计数 >0 的通道在新 ctx 就绪后无缝切换，期间该算法结果短暂为空）
5. businessd 更新 SQLite algorithms 表（version/npu_cost_ms/changelog）

回滚：
  自动：新版本精度自检不过 → 自动 rename 回 .bak + 告警
  手动：Web 算法库页"回滚到 v1.1.0"按钮
```

**精度自检（升级防呆）**：热加载后立即用 selftest/ 标注图跑自检，mAP 掉出容忍带（5%）判为坏包自动回滚。算法 OTA 与固件 OTA 的最大区别：模型文件是"数据"也会坏（转换器版本错、量化损失、包损坏），必须带验收机制。

#### 11.4.5 训练规范（单模型 vs 多模型决策）

**策略：分层混合训练。**

```
第 1 层：基础物理模型 yolov8n-base（单模型合训）
   person / vehicle / helmet / no_helmet / phone 等人形相关类
   ← 同图上下文互相帮助，合训精度更高、算力最省（一次推理出全部结果）

第 2 层：级联专项模型（独立小模型，一级框触发）
   人脸（RetinaFace+特征）· 车牌（检测+LPRNet）· 站坐分类 · 姿态跌倒
   ← 输入是 crop 小图，与一级模型输入域不同，必须独立模型
```

拆分独立包的判定条件：

1. 输入域不同（级联类，本来就不能合）；
2. 合训后某类掉点严重（如 no_helmet 小目标被 person 挤压）——实测 mAP 对比后定；
3. 数据集来源不同（不同客户垂类，数据不共享）；
4. 更新节奏不同（如安全帽规范变了只需重训 helmet 包）。

类别变更 = 重训 base + 升版本号，走算法包 OTA。

#### 11.4.6 Web 算法库页面

```
算法库页：
  筛选：type（检测/分类/姿态/车牌/人脸）· scene_tags（工地/工厂/电力）
  条目：名称 · 版本 · 类型 · 输入尺寸 · npu_cost_ms · 常驻/触发
        类别列表 · 已被 N 路使用 · 依赖标注 · [详情][更新日志]
  底栏：NPU 预算实时条（算力 xx% · 内存 xx%）· [导入算法包] [检查更新]
  详情弹窗：类别与示例图 · 默认阈值 · changelog · 各通道使用情况
```

页面数据全部来自注册表经 `GET /api/algorithms` 暴露，无前端硬编码。

#### 11.4.7 V2 验证项

1. 同一物理模型下"多通道不同类别过滤"功能验证；
2. 合训 vs 独训精度对比实验（用安全帽数据集先做一轮，实测后定稿训练规范）；
3. 每算法 ctx 内存实测入库（npu_mem_kb）；
4. 算法包热升级 + 精度自检 + 自动回滚演练。

#### 11.4.8 产品决策（已定）

| 决策点           | 结论                                                                     |
| ---------------- | ------------------------------------------------------------------------ |
| 级联依赖缺失时   | **自动勾选一级算法**（用户体验优先，注册表记录隐式依赖）           |
| 第三方算法包导入 | 首版仅内置（目录结构与签名校验保留，导入 API 后置）                      |
| 人脸库归属       | 算法包之外的独立资源（/data/faces/ + 独立页面），由用户运营而非 OTA 分发 |

---

# 第五部分 工程保障

## 12. 端到端数据流详解（心智模型）

> 本节沉淀架构对齐讨论的结论，是全文档的"原理速览"章节。配套图：`AIBox_四路视频全流程.drawio` / `AIBox_4cam_flow.png`。

### 12.1 一帧画面的完整旅程（以 4 摄像头为例）

```
摄像头(内部: RAW→ISP→VENC编码)
  │ H.264 主码流(1080p,录像用) + 子码流(480p,AI/预览用) —— 两路独立RTSP地址,同时编码
  ▼
MediaServer 拉流代理×8 (4摄像头×2流) —— 全程不解码, 只转封装
  ├─► MP4录像: 主码流ES直存, 5min切片 → SATA        (压缩态原样落盘)
  ├─► WebRTC : 子码流 → 浏览器宫格(原生信令)          (压缩态原样分发)
  └─► RTSP回环: 子码流×4 → core-engine               (压缩态过本机TCP)
  ▼
core-engine (唯一解码点在此进程内)
  RtspPuller×4 → MPP VDEC(硬解→NV12 dma-buf) → RGA(缩放+letterbox→640×640, 零拷贝)
  → NPU(推理→候选框张量) → CPU后处理(NMS→逆letterbox→ByteTrack→规则) → detections JSON
  ▼
UDS → businessd → WS → 浏览器 canvas 叠框
```

### 12.2 视频格式六次换手（压缩态尽量长，像素态尽量短）

| 环节             | 格式状态                       | 说明                                               |
| ---------------- | ------------------------------ | -------------------------------------------------- |
| 摄像头内         | RAW→H.264                     | 唯一一次视频编码（摄像头 VENC）                    |
| 摄像头→ZLM      | H.264 ES (RTP/RTSP)            | 压缩态过网                                         |
| ZLM 内部         | 转封装：ES→MP4 / RTP / SRTP   | **字节不变，只换包装**；"转封装"≠"格式变化" |
| ZLM→core-engine | H.264 ES（回环）               | 压缩态                                             |
| VDEC             | H.264 → NV12 dma-buf          | **唯一一次解码**（只有 NPU 需要像素）        |
| RGA              | NV12 480p → NV12/RGB 640×640 | 像素态，仅进程内零拷贝存在                         |
| NPU 后           | 张量→坐标 JSON                | 结构化态，字节级体积                               |

关键账目：1080p 一帧 H.264≈15KB vs NV12≈3MB（200 倍）——这是"压缩态占链路 80%、像素态只存在于零拷贝进程内一小段"的根本原因。**全链路编码 1 次、解码 1 次、重编码 0 次**（烧框=重编码，被铁律禁止）。

### 12.3 各消费者用哪条流（按用途固定，不动态切换）

| 消费者         | 流                     | 理由                                                       |
| -------------- | ---------------------- | ---------------------------------------------------------- |
| 录像 P0        | 主码流                 | 证据要最高分辨率；不解码直存，成本低                       |
| AI P2/P3       | 子码流                 | 模型输入 640 定死，分辨率换不来检测率；真小目标走级联 crop |
| Web 宫格       | 子码流                 | 8×1080p 浏览器解码卡死                                    |
| Web 单路放大   | 主码流                 | 用户主动看细节时（宫格/全屏是两个会话，非切换）            |
| HDMI P1        | 主码流                 | VOP 直扫，天然高画质                                       |
| 无子码流摄像头 | AI 拉主码流 + RGA 缩放 | 兼容路径，配置表标注                                       |

主/子码流是 IPC 内部两个编码器**同时持续输出**的两个独立 RTSP 会话（如海康 Channels/101、/102），故障也相互独立——看门狗按通道+码流分别监控。摄像头总码率预算：主4M+子0.5M，16路≈72Mbps 进 GMAC。

### 12.4 RGA 预处理的三件事

把 VDEC 的 NV12 帧调成"模型的口味"，全零拷贝：

1. **缩放**：480p → 640×640 的几何变换；
2. **Letterbox**：等比缩放+灰边填充，防变形（YOLO 训练约定），后处理按 `s/pad_x/pad_y` 逆算回原坐标；
3. **按需附加**：格式转换（模型若要 RGB 输入）与 crop（P3 二级模型按一级框裁剪、ROI 检测区域裁剪）。

必须 RGA 干而非 CPU：16 路全帧 YUV 运算会吃 1~2 个 A72 核且打断 dma-buf 链；RGA 是独立 2D 加速器，CPU 只提交任务描述符。RGA 单元内部有并发限制，软件侧 2 线程串行喂（§3.3 分区表）。参考实现：RK_VideoPipe/models/resize_function.cpp。

### 12.5 检测框的两个映射（空间/时间）

**空间——板端后处理解决，前端零换算**：

```cpp
// RGA 预处理记录 {s, pad_x, pad_y}，NMS 后、组 JSON 前逆变换
t.x = (b.x - pad_x) / s;  t.y = (b.y - pad_y) / s;  t.w = b.w / s;  t.h = b.h / s;
```

契约：**detections 发出的就是原始分辨率坐标**。前端 canvas.width=video.videoWidth 与 video 同步 CSS 拉伸，两层坐标系天然重合，`strokeRect` 直接画。ROI 裁剪路径需再加 crop 偏移还原。

**时间——pts 透传 + 前端自动对齐**（视频路径延迟 300~800ms > 元数据路径 100~400ms，直接画框会"框超前画面"）：

固定 `render_delay` 依赖"延迟恒定"，但延迟 = 固定基线 + 网络项，换网络/弱网都要重调，不可扩展。demo 实测（§0 排障）验证了 pts 对齐方案的可落地性：

1. **板端 pts 归一化**：检测通道直拉 RTSP 拿到的是海康绝对大时间戳（数十亿 us），而视频通道经 ZLM `modify_stamp=2` 时间从 0 起——两端不在同一时间域。RtspPuller 以每段会话首帧 pts 为基准归零，使检测 pts 与视频同域、同步增长；
2. **透传 frame_pts**：detections 消息携带该帧的归零 pts；
3. **前端锚定零点差**：两条通道零点差 = 浏览器比 core-engine 晚打开的时间（恒定，与网络延迟无关）。锚定必须用「实时边缘」对拍——检测侧用最新 frame_pts，视频侧用 `video.buffered.end()`（缓冲前沿，≈实时），**不能用 `video.currentTime`（播放头，落后实时 300~800ms）**，否则检测零点被错位推后、框系统性超前画面；
4. **逐帧对齐**：`vc = video.currentTime×1e6 + anchorFp`，弹出所有 `frame_pts ≤ vc` 的检测取最新画框。

> 效果：网络延迟变化只会让「框跟着视频一起延迟」，不影响相对对齐；换任何网络环境免校准。残余偏差仅推理+采样粒度（约 0.2~0.3s）。

3fps 检测 × 30fps 显示的平滑：板端 ByteTrack 输出 track_id+速度（Kalman 外推量，detections 的 vx/vy 字段），前端两次检测间线性插值，超 1s 未更新视为离场。可选进阶（WebRTC getStats 的 jitterBufferDelay 动态修正 / requestVideoFrameCallback）。

### 12.6 帧的标识：哪里精确知道，哪里只能近似

帧身份 = `frame_seq`（抽帧器自增计数）+ `pts`（源 RTP 时间戳），作为数据一部分随帧透传，NPU 异步乱序回调不丢（§6.2 时间戳链）。

| 场景       | 帧级对应                     | 机制                                                                    |
| ---------- | ---------------------------- | ----------------------------------------------------------------------- |
| 板端推理   | 精确                         | pts+frame_seq 随数据流                                                  |
| 事件快照   | 精确（天然同帧）             | 在哪帧上检测的，存哪帧为 JPEG——**取证用快照而非实时截图**的原因 |
| 录像回放   | 精确                         | MP4 时间戳 ↔ detections.ts 双轴检索                                    |
| 浏览器实时 | **近似**（改用 pts 自动对齐） | WebRTC 不暴露"当前渲染帧"给 JS，前端用 video.buffered.end 锚定零点差 + frame_pts 逐帧对齐（§12.5），非固定 render_delay |

### 12.7 浏览器收到的到底是什么

```
编码层: H.264（与摄像头吐出的同一码流格式; Profile 选 Baseline/Main 确保浏览器兼容）
音频层: AAC→Opus（ZLM 为 WebRTC 标准做的唯一一次转码, CPU 极低）
传输层: SRTP over UDP（DTLS 加密, 抗丢包）
信令层: HTTP(WHEP 式 SDP 交换), 建连时走一次
```

浏览器原生 WebRTC 栈（C++）完成解包+解码+抖动缓冲，无 JS 参与、无 MSE——这是选 WebRTC 弃 FLV 的原因（零插件、延迟 300~800ms vs FLV 3~5s）。VM 原型的 flv.js 路线（FLV→MSE→video）仅作过渡期验证手段。

> **过渡期延迟警示**：demo 实测 FLV 的 3~5s 延迟**全在浏览器 MSE 层**（ffplay 直连摄像头 / ZLM RTSP / ZLM FLV 三路几乎零延迟），这是 FLV+MSE 的固有特性，**不代表最终 WebRTC 的 300~800ms**。过渡期用 flv.js 验证时勿据此误判"方案有性能问题"；框画面对齐已改用 pts 自动对齐（§12.5），与播放延迟解耦。

**画面与框的关系**（一句话心智模型）：浏览器显示的是摄像头子码流的实时还原（WebRTC 原生解码）；AI 检测框以坐标元数据经独立通道到达，在前端透明 canvas 与画面会合——**两条通道同源（同一帧的子码流副本），所以坐标天然对得上；视频路径与元数据路径全程分离，服务端永不重编码**。

## 13. 部署

### 13.1 进程监管（systemd，三 unit）

```ini
# zlm.service —— 版本锁定，绝不随业务迭代升级
[Unit]
After=network-online.target
[Service]
WorkingDirectory=/opt/aibox/zlm
ExecStart=/opt/aibox/zlm/MediaServer
CPUAffinity=0 1 2 3          # 小核簇（RK3588/RK3576 拓扑一致：0-3 小核）
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target

# aibox-engine.service
[Unit]
After=zlm.service             # 仅顺序提示，非强依赖：zlm 未起时拉流退避自愈
[Service]
ExecStart=/opt/aibox/bin/core-engine
CPUAffinity=4 5 6 7           # 大核簇；进程内细分绑核沿用 §3.3 分区表
Restart=always
RestartSec=3
[Install]
WantedBy=multi-user.target

# aibox-business.service
[Unit]
After=zlm.service aibox-engine.service
[Service]
ExecStart=/opt/aibox/bin/businessd
CPUQuota=100%                 # ≈1 核上限（cgroup 预算）
Restart=always
RestartSec=2
[Install]
WantedBy=multi-user.target
```

不使用 Requires/BindsTo：三进程各自独立自愈，启动顺序只影响首次就绪速度。非 root 方案：`AmbientCapabilities=CAP_NET_BIND_SERVICE`（绑 80/554 需要）。

### 13.2 MediaServer 配置基线（config.ini）

| 配置项                | 值               | 说明                                                                  |
| --------------------- | ---------------- | --------------------------------------------------------------------- |
| [http] port           | 80               | 设备侧运行，浏览器免输端口                                            |
| [rtsp] port           | 554              | 回环拉流地址                                                          |
| [rtmp] port           | 1935             | 协议完整性预留                                                        |
| [rtc] port / externIP | 8000/udp · 留空 | 局域网部署留空自动取网卡 IP                                           |
| [record] enableMp4    | 0                | 全局关，**按代理开启**（只录主码流）                            |
| [record] mp4MaxSecond | 300              | 5min 切片                                                             |
| [record] mp4SavePath  | /data/record     | SATA 挂载点                                                           |
| [hook] enable         | 1                | on_stream_changed / on_record_mp4 →`http://127.0.0.1:8090/hook/…` |
| [hook] timeoutSec     | 5                | hook 慢不拖媒体面                                                     |

安全：`[api] secret` 出厂随机化；hook 仅监听 127.0.0.1；Web 管理面由 businessd 承担，MediaServer 的 80 端口只暴露播放信令与（可选）回放文件直链。

## 14. 验证计划（V0~V4）

| 阶段                  | 内容                                                                                                                                                                                                          | 验收标准                                                                                                                                                                             |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **V0 环境基线** | BSP 内核检查（/dev/dri、mpp_service、rga、dma_heap、rknpu≥0.9.2）；**MediaServer aarch64 交叉编译部署**（工具链 + OpenSSL≥1.1 + libsrtp2，锁 commit）、config 基线、三 systemd unit；dfi 计数路径适配 | 检查清单全绿 + 三服务开机自起、端口核对                                                                                                                                              |
| **V1 媒体链路** | 16 路 × 双流 addStreamProxy、MediaServer 原生录像与切片、WebRTC 原生端点预览、回环拉流基线                                                                                                                   | 8 路 24h 零断流（kill 源 100 次恢复 100 次）、切片完整 + segments 索引齐、预览延迟<1s、解码 CPU<15%、**回环开销实测（见下）**                                                  |
| **V2 AI 链路**  | 模型重转换（§11.3）、NpuScheduler（分桶轮转/令牌限速/准入控制）、通道管线、ByteTrack、检测元数据出口                                                                                                         | 8 路×2 算法帧率达标、NPU 两核<60%、rq_triggered 丢弃率<1%、RECONFIG 中断<500ms、kill MediaServer ×100 次 core-engine 重连成功率 100%、多常驻模型分桶场景（base+pose 双桶混跑）验证 |
| **V3 资源仲裁** | 指标采集、降级阶梯、故障注入、72h 烤机                                                                                                                                                                        | 五级降级可触发可恢复（<2s）、录像全程无影响、RSS 增长<5%、**§8.3 三进程故障注入矩阵全绿**                                                                                     |
| **V4 业务闭环** | businessd + Web 页最小集 + 真实 IPC + 回放页                                                                                                                                                                  | 全链路：加摄像头→配算法→看画面→收事件→回放录像（含 MQTT 可选）                                                                                                                   |

**回环开销实测方法**（本方案的关键自证实验）：同一块板上，core-engine 帧入口分别接 ①回环 RTSP ②本地文件直灌（≈"嵌入式理想入口"），对比 CPU 与端到端延迟，差额即"架构税"。**若 >5% 整机 CPU 或引入 >100ms 隐藏延迟，触发 §17 切换评估。**

**验证期专项风险**：板卡缺 BSP 节点先换系统勿硬扛；预装 runtime 旧版必须替换 2.3.2；RGA 多线程内部锁（2 线程串行喂）；WebRTC 端口/证书先局域网验证。

## 15. RK3576 迁移与版本锁定

### 15.1 迁移映射

| 项                 | 迁移动作                                                                                                      | 风险                     |
| ------------------ | ------------------------------------------------------------------------------------------------------------- | ------------------------ |
| 全部代码           | 直接重编译（MPP/RGA/RKNN API 同源）                                                                           | 低                       |
| 模型               | `target_platform='rk3576'` 重转换 + 精度回归 + npu_cost_ms 重标定                                           | 中（量化行为可能不同）   |
| NPU 调度           | core_mask 2+1 → 1+1，纯配置项                                                                                | 无                       |
| librknnrt.so       | 换 RK3576 对应 2.3.2 包                                                                                       | 低                       |
| MediaServer        | aarch64 交叉编译（OpenSSL≥1.1 + libsrtp2），同 commit 锁定；版本锁定表加一行校验：二进制 md5 + git rev-parse | 低                       |
| systemd            | CPUAffinity 拓扑（0-3 小核/4-7 大核）两 SoC 一致，零改动                                                      | 无                       |
| **DDR 结论** | **必须 RK3576 板复测**（64→32-bit，唯一硬差异），阈值②可能调低                                        | **高（Go/No-Go）** |

### 15.2 版本锁定表（CI/产线对齐用）

| 组件                     | 版本                          | 校验方式                                          |
| ------------------------ | ----------------------------- | ------------------------------------------------- |
| rknn-toolkit2（PC）      | 2.3.2                         | pip freeze                                        |
| librknnrt.so（板端）     | 2.3.2 配套                    | cat /sys/kernel/debug/rknpu/version + strings .so |
| rknn_model_zoo           | 2.3.2 tag                     | git tag                                           |
| ZLMediaKit / MediaServer | 固定 commit（进入 V1 时记录） | git rev-parse + 二进制 md5                        |
| MPP / librga             | 板卡 BSP 系统库（不自编译）   | ldconfig -p                                       |
| 内核                     | 5.10/6.1-rockchip BSP         | uname -r                                          |

## 16. 独立式特有风险登记

| # | 风险                                                                             | 缓解                                         |
| - | -------------------------------------------------------------------------------- | -------------------------------------------- |
| 1 | **回环隐藏缓冲**（最大坑）：ffmpeg RTSP 客户端默认抖动缓冲引入数百 ms 延迟 | §6.3 参数显式压低；V1 必测项                |
| 2 | MediaServer 崩溃时当前 MP4 切片 moov 损坏（嵌入式同有）                          | 切片完整性校验进 V3；必要时调小 mp4MaxSecond |
| 3 | ZLM WebRTC 公网部署需 externIP + UDP 端口映射                                    | 首版局域网产品不涉及；公网版本立项时单独评估 |
| 4 | 双层重试风暴：ZLM 源重试 × core-engine 拉流重连叠加                             | 两层退避均 64s 封顶并错峰；V2 断流注入验证   |
| 5 | businessd 重启期间 on_record_mp4 丢失 → segments 缺口                           | 启动时扫盘比对 DB 补扫；事件级丢失可容忍     |

## 17. 与嵌入式方案的互切（架构保险）

**不变量清单**（互切 = 只换实现，不换契约）：

- UDS 协议（§5.3）逐字段不变
- REST/WS 前端 API（§10.3）不变
- 流命名 `live/cam{id}_{main,sub}` 不变
- detections 元数据结构与时间戳链不变

**切换点**（工作量收敛处）：

| 模块                    | 独立式实现                | 嵌入式实现                       |
| ----------------------- | ------------------------- | -------------------------------- |
| core-engine/media/ 入口 | RtspPuller（回环拉流）    | MediaService（C API 进程内回调） |
| 录像归属                | MediaServer 原生 recorder | mk_recorder                      |
| 编排通道                | REST（businessd 单写者）  | 进程内函数调用                   |

两侧入口模块对下游暴露同一接口（`onEsPacket(nalu, pts)` → MppDecoder），切换不动解码之后任何代码。

**触发切换的条件**：V1 回环实测超标（§14）→ 切嵌入式；嵌入式 WHEP 自建成本超预算 / 交付要求单二进制 → 反向不适用，独立式即当前形态。

## 18. 下一步

1. **VM 先行验证数据面**（零成本）：现有 VM 环境（ZLM + detect_server.py）**本来就是本方案的架构原型**——把 detect_server.py 的拉流地址从"ffmpeg 直推流"改为"经 addStreamProxy 代理的流"，即完成回环链路 + 编排序列的桌面验证；
2. 板上 V0：交叉编译 MediaServer、落地三 systemd unit 与 config 基线；
3. 按 §14 验证计划推进，V1 重点完成回环开销自证实验；
4. 前端补最后一块核心逻辑：detQueue + pts 自动对齐（§12.5）+ track 插值，可在 VM 上先行开发；
5. PC 端搭 rknn-toolkit2 2.3.2 环境，完成 6 个模型 rk3588 转换（§11.3）；
6. core-engine 骨架起步顺序：`common/（DmaBuffer+BoundedQueue+EdgeMeter）→ media/（RtspPuller+MppDecoder+CameraChannel）`，即 V1 最小可运行集。
