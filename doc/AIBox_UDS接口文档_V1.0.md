# AIBox UDS 接口文档（engine ↔ businessd 全量）

> 版本：2026-10（随 fe30a07/842e6cc 后代码核对生成）
> 权威源：`engine/ipc/Protocol.h/.cpp`（编解码）、`engine/main.cpp`（cmd handler）、`engine/ipc/UdsServer.h`（事件分发语义）、`business/main.cpp`（事件消费）、`business/uds_client.cpp`（请求发起 + 断线重同步）

---

## 1. 通道与帧格式

| 项       | 约定                                                                                |
| -------- | ----------------------------------------------------------------------------------- |
| 载体     | Unix Domain Socket（STREAM），路径`--uds` 参数（默认 `/tmp/aibox-engine.sock`） |
| 客户端   | businessd 为唯一客户端（单连接；断线 1s 重连 + 全量重同步）                         |
| 帧格式   | JSON-lines：一个 JSON 对象一行，`\n` 分帧（utf-8，紧凑序列化）                    |
| 方向     | 请求 businessd→engine（需 ack）；事件 engine→businessd（可丢，尽力而为）          |
| 心跳     | UdsClient 每 5s 应用层心跳帧（断链检测）；空闲 5s 无业务时保活                      |
| 队列纪律 | 事件发送为 per-connection 有界队列，慢消费者只丢自己的帧，不阻塞 emit、不拖累他人   |

## 2. 消息信封（三类）

```
请求  {"msg_id": <u64>, "cmd": "<name>", ...载荷}
事件  {"evt": "<name>", ...载荷}
应答  {"ack": <msg_id>, "ret": 0[, "data": <JSON 值>]}          成功
      {"ack": <msg_id>, "ret": <非0>, "msg": "<原因>"}          失败
```

规则：

- `msg_id` 由 businessd 单调递增分配，engine 原样回填到 `ack`
- `data` 是**内联 JSON 值**（对象/数组），不是转义字符串
- 解析失败/缺必填/未知 cmd/evt → engine 回 `ret=4001`，**连接不断**
- `ret=0` 时 `msg` 字段省略；`ret≠0` 时 `data` 字段省略

## 3. 错误码（UDS 域）

| ret  | 含义           | 产生场景                                      |
| ---- | -------------- | --------------------------------------------- |
| 0    | 成功           | —                                            |
| 1002 | 对象不存在     | add 的算法未注册 / remove/set 的 cam 不存在   |
| 1003 | 状态不允许     | add_cam 重复 id（通道已存在）                 |
| 3004 | 自检失败已回滚 | reload_algo 新包加载/自检失败，旧版本自动恢复 |
| 4001 | 协议错误       | JSON 坏/缺字段/未知消息名                     |

（HTTP 层另有 1001/2001/2002/3001/3002/4002/5001，见详设 §8.2，不在此域。）

---

## 4. 请求接口（businessd → engine，共 6 个）

### 4.1 `add_cam` — 添加摄像头通道

请求：

```json
{"msg_id":1, "cmd":"add_cam", "id":10,
 "rtsp_sub":"rtsp://admin:pass@192.168.1.64:554/Streaming/Channels/102",
 "rtsp_main":"rtsp://admin:pass@192.168.1.64:554/Streaming/Channels/101"}
```

| 字段      | 必填 | 说明                                             |
| --------- | ---- | ------------------------------------------------ |
| id        | ✓   | 通道 id（= DB cameras.id，非零）                 |
| rtsp_sub  | ✓   | 子码流拉流地址（检测源，恒拉）                   |
| rtsp_main | ✗   | 主码流地址（单路放大显示用；缺省不建主码流通道） |

行为：查重（重复 id → 1003）→ 构造 CameraChannel（源/解码工厂按运行模式）→ 启动拉流解码线程 → RuleEngine 登记 frame_size → 推送 `camera_status online=true` 事件。

应答：`{"ack":1,"ret":0}`；失败 1003（重复）。

### 4.2 `set_algos` — 配置通道算法（全量替换语义）

请求：

```json
{"msg_id":2, "cmd":"set_algos", "id":10,
 "algos":[
   {"algo":"person-det", "thr":0.30, "fps":5,
    "roi":null, "rule_type":"region_in", "dwell_sec":5, "debounce_sec":5},
   {"algo":"face-det", "thr":0.5, "fps":1,
    "roi":[[0.1,0.1],[0.9,0.1],[0.9,0.9],[0.1,0.9]],
    "rule_type":"dwell", "dwell_sec":10, "debounce_sec":8}
 ]}
```

| 字段               | 必填 | 说明                                                                                               |
| ------------------ | ---- | -------------------------------------------------------------------------------------------------- |
| id                 | ✓   | 通道 id                                                                                            |
| algos[]            | ✓   | 全量替换（空数组 = 清空该通道算法）                                                                |
| algos[].algo       | ✓   | algo_id（须在算法库已注册且 enabled，否则 1002）                                                   |
| algos[].thr        | ✓   | 通道级置信度阈值（conf < thr 的检测在 NpuScheduler 出口被滤除；≤0 用 manifest default_threshold） |
| algos[].fps        | ✓   | 检测采样帧率（0 视为 1）；通道抽帧率 = max(各算法 fps)                                             |
| algos[].roi        | ✗   | 归一化多边形顶点 [[x,y],...]（0~1）；null/缺省 = 全屏                                              |
| algos[].rule_type  | ✗   | 规则类型 `region_in`（进入区域）/ `dwell`（区域内驻留）；缺省 = region_in（V1 UI 仅暴露这两类） |
| algos[].event_type | ✗   | 事件类型串；缺省 = engine 按 `{algo}_{rule_type}` 兜底生成                                          |
| algos[].dwell_sec  | ✗   | DWELL 驻留阈值秒（缺省 5）                                                                          |
| algos[].debounce_sec | ✗ | 事件防抖窗秒（缺省 5；key=(cam,event_type,cls)）                                                    |

行为：逐算法查注册表（未注册 → 1002）→ 通道 RECONFIG 迁移（drain 在途帧 + 清 sampler 令牌）→ 调度器更新通道 fps 与桶权重 → **规则派生与下发**（带 roi（≥3 顶点）的算法生成 RuleEngine 规则并 set_rules 全量替换，未画 roi = 仅检测不报警）→ 下一采样帧起生效。

应答：`{"ack":2,"ret":0}`；失败 1002（algo 未注册）/ 1002（cam 不存在）。

注：**thr/fps/规则热生效**（RECONFIG <500ms，set_rules 同步清 track 状态）。规则字段 decode 可选（老版 businessd 缺省保默认值，双向向后兼容）。

### 4.3 `remove_cam` — 移除摄像头通道

请求：`{"msg_id":3, "cmd":"remove_cam", "id":10}`

行为：通道表摘除（不存在 → 1002）→ 锁外 stop（join 线程 + drain，fd 归还池）→ 规则/跟踪状态清理 → 重算调度权重 → 推送 `camera_status online=false`。

应答：`{"ack":3,"ret":0}`；失败 1002。

### 4.4 `sub_meta` / `unsub_meta` — 检测元数据订阅（成对，计 2 个接口名）

请求：

```json
{"msg_id":4, "cmd":"sub_meta", "id":10}
{"msg_id":5, "cmd":"unsub_meta", "id":10}
```

语义（会话级，按 UDS 连接记账）：

- `sub_meta` 后，该连接开始收到此 cam 的 `detections` 事件
- `unsub_meta` 停止
- businessd 的联动：**WS `/ws/meta/{cam}` 有 viewer 首连 → sub_meta；全部断开 → unsub_meta**（T4.5↔T4.7 接线；不订阅则 engine 侧该通道 detections 直接丢弃，不占 UDS 带宽）

应答：`{"ack":4,"ret":0}`（UdsServer 内部处理，恒成功）。

### 4.5 `reload_algo` — 算法包热升级/回滚

请求：`{"msg_id":6, "cmd":"reload_algo", "algo":"person-det"}`

行为：重新校验+构建该算法包（新 NPU ctx）→ **自检（selftest：真实推理一帧 + post 冒烟）** → 失败自动回滚旧版本并返回 3004 → 成功后推送 `registry_ready`（全表快照）。

应答：`{"ack":6,"ret":0}` 或 `{"ack":6,"ret":3004,"msg":"reload failed (rollback attempted): person-det"}`。

注：热升级（新包替换旧文件）与回滚（恢复 .bak）都走此接口；`npu_mem_kb` 由此时的实际 ctx 得出。

### 4.6 `get_metrics` — 运行指标查询

请求：`{"msg_id":7, "cmd":"get_metrics"}`

应答（data 内联对象，全字段）：

```json
{"ack":7,"ret":0,"data":{
  "npu": {"load_pct": 4.0, "steady_cores":[0,1], "triggered_cores":[2]},
  "ddr_pct": 31.5,
  "temp_c": 52.0,
  "queue": {"steady_depth":0, "triggered_depth":0, "uds_dropped":0},
  "cams": {
    "10": {
      "state": 3, "src_opens": 2, "sampler_fps": 5,
      "dec":  {"in":100,"out":100,"drop":0,"sampled":0},
      "pool": {"in":100,"out":100,"drop":0,"sampled":0},
      "samp": {"in":100,"out":20,"drop":0,"sampled":80}
    }
  }
}}
```

字段说明：

- `npu.load_pct`：三核峰值（来源 /sys/kernel/debug/rknpu/load）；`steady_cores/triggered_cores` 为 NpuCorePlan 分配（steady=[0,1] triggered=[2] 为 rk3588 默认）
- `temp_c`：NPU 温度（降级 L4 阈值 85°C）
- `queue.uds_dropped`：事件通道因慢消费累计丢弃数
- `cams.<id>.state`：0=STOPPED 1=LOADING 2=RECONFIG 3=RUNNING（CameraChannel::State 枚举序）
- `dec/pool/samp`：三道边计数器（in/out/drop/sampled），差分即速率；samp.sampled = 令牌拦截未推理帧数

该接口失败（engine 离线）时 UdsClient 返回 false，不产生 ack；`/api/resources` 的 `engine_online` 即以此判定。

---

## 5. 事件接口（engine → businessd，共 5 类；尽力而为，可丢）

### 5.1 `camera_status` — 通道上下线

```json
{"evt":"camera_status", "id":10, "online":true}
```

触发：add_cam 成功（online=true）、remove_cam（false）、通道 watchdog 断流重连状态迁移（源重开不重复报）。
businessd 消费：DB cameras.online 更新 + WS 全局广播（前端顶栏/卡片徽标）。

### 5.2 `registry_ready` — 算法注册表快照

```json
{"evt":"registry_ready", "algos":[
  {"algo_id":"person-det", "version":"1.0.0", "model_ref":"yolov8n.rknn",
   "npu_cost_ms":17.19, "npu_mem_kb":32768, "trigger":"steady",
   "disabled":false,
   "classes":[0],
   "class_names":["person"]}
]}
```

字段说明：

- `trigger`：steady（常驻 Core0-1）| triggered（事件触发 Core2）
- `disabled=true` 表示包校验/加载失败（引擎保留条目与原因，页面上灰显；原因见 businessd 日志）
- `classes`：manifest 白名单（COCO id；空 = 全类输出）
- `class_names`：下标即 COCO id 的名称映射（空 = 前端显示 cls{id}）
- 注：本快照为**全表**（每次整表 replace，非增量）

**设计权衡：类名为什么只在快照里、不进 detections/event 数据流**
id 是协议稳定键（数字、可索引、改名不受影响），名称是展示层概念——
名称只在 registry_ready 快照传递一次，前端/事件页按 id 查本地映射渲染。
收益：detections 每目标省 ~10B 持续带宽；算法侧改名（class_names 改一行）
全局生效且不污染历史数据（events 表存的 targets 是 id，检索不受改名影响）；
升级瞬间不会出现新旧名混飞。**消费端严禁把名称写回数据链路。**

触发：启动加载完成、reload_algo（成功或回滚）。
businessd 消费：algorithms 表整表 replace + WS 广播。

### 5.3 `detections` — 检测帧元数据（流式，订阅制）

```json
{"evt":"detections", "id":10, "ts":1728555555123456,
 "frame_seq":12345, "frame_pts":7890123,
 "frame_w":640, "frame_h":480,
 "targets":[
   {"cls":0, "conf":0.87, "x":100.5, "y":120.0, "w":80.0, "h":160.0,
    "track_id":3, "vx":12.5, "vy":-2.0}
 ]}
```

字段说明：

- **发送条件**：该 UDS 连接已 `sub_meta(id)`；未订阅的通道帧后处理完成后直接丢弃
- `ts`：墙钟微秒（事件检索对齐录像时间段）
- `frame_pts`：归零 pts 微秒（前端检测框与视频对齐锚）
- `frame_w/h`：检测域分辨率 = 解码帧宽高（前端 fitRect 缩放基准；D3 契约，缺字段按 4001 拒）
- `targets[].x/y/w/h`：原始分辨率坐标系（左上角+宽高，post 逆 letterbox 后）
- `track_id`：ByteTrack 分配（-1 = 未成 track 的裸检测；≥0 稳定 id，漏检 ≤15 帧内恢复同 id）
- `vx/vy`：KF 速度 px/s（前端"检测在哪画哪"策略已不消费，保留给事件/规则）
- `conf`：已过通道阈值（set_algos thr）过滤

频率：= 通道检测采样率（各算法 fps 最大值，如 5fps × 通道数）。
businessd 消费：按 cam 组播到 WS `/ws/meta/{cam}`（**原始行透传**，不重建 JSON）。

### 5.4 `event` — 规则事件（告警）

```json
{"evt":"event", "id":10, "algo":"person-det", "type":"intrusion",
 "ts":1728555555123, "snap":"/data/recordings/snap/xxx.jpg",
 "targets":[{...同 detections 的 target 结构...}]}
```

触发：RuleEngine 规则边沿（ROI 进入/驻留/类别切换触发）。防抖：(cam,event_type,cls) 三元组 debounce_sec 窗口内只报首帧。
传输：**全局广播（不受 sub_meta 订阅制约束）**——订阅制只为 detections 高频流省带宽；报警低频且关键，按通道过滤会在无人订阅（无宫格页）时于 engine 侧静默丢弃、连落库都不发生（板上实证）。
businessd 消费：**DB events 落库**（targets 取原始行 JSON 存档）+ WS 广播（toast 卡片）+ MQTT outbox（启用时，附 _event_id 供重传对账）。

### 5.5 `degrade` — 系统降级

```json
{"evt":"degrade", "level":2, "reason":"水位 >70% 持续 60s"}
```

级别（详设 §3.9）：0=全恢复；1=通道降采样（水位）；2=触发类停推；3=低优算法停；4=NPU 过温 85°C；5=裸模式（仅拉流录像）。level>0 进入即报、恢复到 0 报 0。
businessd 消费：LOGI + WS 广播（顶栏橙字 + toast）。

---

## 6. 交互时序（关键流程）

### 6.1 启动同步（SYNCING）

```
businessd 启动
  → UDS connect（1s 重连循环直至 engine up）
  → 收到 engine 的 registry_ready（启动快照）→ algorithms 整表 replace
  → ensure_proxies：getMediaList 对比 DB，ZLM 侧代理逐台重建（幂等）
  → 逐台 add_cam（ack 重试 ×N）+ set_algos（DB 中该通道算法全量下发）
  → 全部成功 → ready=true（/api/system/info 可见）
```

任何一台上：add 失败重试后仍败 → 该台 degraded（继续其余）；**断线 → 整个同步重来**（幂等保障：重复 add_cam 返回 1003 但 businessd 视为已存在）。

### 6.2 WS viewer 订阅联动

```
浏览器开宫格 → WS /ws/meta/10 首连接
  → businessd → engine: sub_meta(10)
  → engine 开始向该 UDS 连接推送 cam10 detections
浏览器全关 → WS /ws/meta/10 计数归零
  → businessd → engine: unsub_meta(10)
  → engine 停推（帧后处理仍做，只是元数据丢弃）
```

### 6.3 算法热升级

```
运营替换包文件 / 触发回滚
  → businessd → engine: reload_algo(algo)
  → engine：新 ctx 构建 → selftest 推理一帧
       ├─ 成功 → 替换实例 → registry_ready（全表）→ ack ret=0
       └─ 失败 → 自动回滚 .bak → registry_ready（旧版本）→ ack ret=3004
```

## 7. 严谨性契约（实现者必读）

1. **必填字段缺失 = 4001**：decode 逐字段校验（GET_U32/GET_I64/GET_F32/GET_STR 宏），不做静默默认——收发两端 schema 漂移立即暴露
2. **可选字段**：add_cam.rtsp_main（has_rtsp_main 区分"显式不带"与"空串"）、set_algos[].roi（has_roi + null 双态）、registry_ready 的 classes/class_names（老版本缺省 = 兼容）
3. **透传纪律**：detections/event 的原始行在 businessd **不重建 JSON**（ParsedMsg 往返会丢未知字段）——WS 透传与事件 targets 存档都取原始行
4. **可扩展性**：新增事件字段时 businessd 消费端按 `root.isMember` 判存在，禁止破坏性重排；新 cmd/evt 先加 Protocol.h 结构 + encode/decode + 测试（test_uds_protocol 往返断言），再接 handler
5. **性能纪律**：detections 发送频率 = 采样率 × 通道数，单行 <1KB；UDS 本地带宽无忧，但订阅制保证无人观看时零输出

## 8. 测试与验证

- 单测：`tests/test_uds_protocol.cpp`（全部消息 encode→decode 往返 + 坏输入 4001，板上 23 用例）
- 冒烟：`scripts/smoke_m4.sh`（SYNCING/事件流通）、`scripts/smoke_m5.sh`（WS meta D3 契约端到端）
- 手工：`scripts/ws_probe.py --port 18081 --path /ws/meta/1` 探针收帧
