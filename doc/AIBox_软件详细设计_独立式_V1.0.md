# AIBox 软件详细设计说明书（独立式进程架构）V1.0

> 文档日期：2026-09-22
> 上游文档：《AIBox 软件方案 · 独立式进程架构 V1.1》（下称"架构方案"，引用记作 架构§N）
> 定位：架构方案的**编码级落地设计**——模块内类/接口/数据结构/线程模型/状态机/时序。本文不重复架构论证，只定义实现；与架构方案冲突时以架构方案为准并回改本文。
> 阅读对象：core-engine / businessd / 前端 开发工程师
> 语言/标准：C++17（core-engine、businessd）；ES2020（前端）；Python 不进入产品（仅工具链）

---

## 目录

```
1  总体设计（设计单元/源码结构/全局约定/线程全景）
2  基础库 common/ 详细设计
3  core-engine 详细设计（通道/拉流/解码/推理调度/跟踪/规则/仲裁/UDS）
4  MediaServer 集成设计（零改造）
5  businessd 详细设计（模块/状态机/API/DDL/启动）
6  前端详细设计（播放内核/元数据叠加管线）
7  关键流程时序（6 个）
8  接口契约汇总（UDS 字段表/错误码/WebHook 格式）
9  算法包 manifest 完整 Schema
10 日志与可观测性
11 测试设计要点
12 附录：配置与目录清单
```

---

## 1. 总体设计

### 1.1 设计单元与依赖关系

```
┌─ MediaServer（ZLMediaKit，零改造，闭源二进制形态交付）
│    依赖：无（被 businessd 经 REST 编排，被 core-engine 经 RTSP 消费）
├─ core-engine（自研，C++17）
│    依赖：librockchip-mpp · librga · librknnrt(2.3.2) · libavformat/libavcodec(仅demux) · zlib(可选)
│    依赖：jsoncpp（随 ZLM 源码树 3rdpart，独立编译一份）
├─ businessd（自研，C++17）
│    依赖：cpp-httplib(header-only) · sqlite3 · libmosquitto · jsoncpp
└─ web/（静态 SPA，无构建链）
     依赖：ZLMRTCClient.js（随 ZLM www 目录）
```

**依赖纪律**：core-engine 与 businessd **互不链接对方代码**，唯一通道是 UDS（架构§5.3）；core-engine **不依赖** SQLite/httplib（热路径进程最小化依赖）；两进程各自静态链接 C++ 运行时（`-static-libstdc++`），规避板端 glibstdc++ 版本漂移。

### 1.2 源码目录结构

```
aibox/
├── engine/                          # core-engine
│   ├── CMakeLists.txt
│   ├── common/
│   │   ├── DmaBuffer.h  FramePool.h  BoundedQueue.h  EdgeMeter.h
│   │   └── NonCopyable.h  TimeUtil.h
│   ├── media/
│   │   ├── RtspPuller.h/.cpp        # 回环拉流线程
│   │   ├── CameraChannel.h/.cpp     # 通道属主 + 状态机 + 看门狗
│   │   └── Sampler.h/.cpp           # 抽帧器（令牌桶）
│   ├── codec/
│   │   └── MppDecoder.h/.cpp
│   ├── infer/
│   │   ├── ModelRegistry.h/.cpp     # ctx 池 + 引用计数
│   │   ├── NpuScheduler.h/.cpp      # 分桶 WRR + 准入
│   │   ├── AlgoInstance.h           # 单模型运行时
│   │   └── algos/                   # 每算法族 preproc/run/postproc
│   │       ├── yolo.h/.cpp          # yolov8/yolo11 检测
│   │       ├── pose.h/.cpp  cls.h/.cpp  face.h/.cpp  lpr.h/.cpp
│   │       └── letterbox.h/.cpp     # RGA letterbox + 逆变换参数
│   ├── track/
│   │   └── TrackerHub.h/.cpp        # 每通道 ByteTrack 封装
│   ├── rules/
│   │   └── RuleEngine.h/.cpp
│   ├── arbiter/
│   │   ├── Monitor.h/.cpp           # 500ms 指标快照
│   │   └── DegradeController.h/.cpp
│   ├── ipc/
│   │   ├── UdsServer.h/.cpp         # epoll + JSON-lines
│   │   └── Protocol.h               # 消息定义(§8.1)
│   ├── algo_pkg/
│   │   └── Manifest.h/.cpp          # 算法包扫描/校验(AlgoRegistry)
│   └── main.cpp
├── business/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── api/   HttpApi.h/.cpp  WsHub.h/.cpp
│   ├── engine/ UdsClient.h/.cpp
│   ├── zlm/   ZlmClient.h/.cpp  HookServer.h/.cpp
│   ├── db/    Db.h/.cpp  Schema.sql  Migrations/
│   ├── cloud/ MqttClient.h/.cpp  Outbox.h/.cpp
│   ├── ota/   OtaManager.h/.cpp
│   └── common/ Config.h/.cpp  Log.h
├── web/                             # 静态 SPA（businessd 托管）
│   ├── index.html  app.js  player.js  overlay.js  api.js
│   └── vendor/ ZLMRTCClient.js
├── deploy/
│   ├── systemd/{zlm,aibox-engine,aibox-business}.service
│   ├── zlm_config.ini               # MediaServer 基线配置
│   └── install.sh                    # 产线安装脚本(校验版本锁定表)
└── tools/                            # PC 端工具(非产品)
    ├── npu_bench/                    # npu_cost_ms 实测工具
    └── pack_algo/                    # 算法包打包/签名工具
```

### 1.3 全局约定

| 项     | 约定                                                                                                                        |
| ------ | --------------------------------------------------------------------------------------------------------------------------- |
| 时间   | 进程内统一`steady_clock`（单调）；对外（UDS/DB/日志）统一 Unix 毫秒（墙钟）；pts 保持摄像头 90kHz 原值不换算              |
| ID     | `cam_id: uint32`（DB 自增）；`stream_name = "cam{id}_{main,sub}"`（唯一跨进程 trace_id，架构§9）                       |
| 错误码 | 分段：0=成功；1xxx 参数/状态；2xxx 摄像头/流；3xxx NPU/算法；4xxx UDS/内部（§8.2 全表）                                    |
| 命名   | 类 PascalCase；成员`snake_case_` 尾下划线；线程函数 `xxx_loop`；UDS 字段与架构§5.3 逐字一致                            |
| 内存   | 热路径禁`new/delete`（帧走 FramePool；Job 走预分配 slab 或 object pool）；非热路径无约束                                  |
| 异常   | 热路径**禁异常**（`-fno-exceptions` 编译 core-engine 全量，错误用返回值/Result）；businessd 允许异常但 API 边界捕获 |
| 单位   | 比特率 kbps、时间 ms（存储）/ us（内部计量）、坐标浮点像素（原始分辨率系）                                                  |

### 1.4 线程全景表（core-engine，16 通道满配）

| #    | 线程                 | 数量            | 创建者                | 亲和       | 阻塞点                      | 崩溃域   |
| ---- | -------------------- | --------------- | --------------------- | ---------- | --------------------------- | -------- |
| 1    | main                 | 1               | main                  | 大核       | UDS 命令分发(非阻塞epoll)   | 全进程   |
| 2    | rtsp_pull_loop       | 16              | CameraChannel         | 大核       | av_read_frame (stimeout 5s) | 仅该通道 |
| 3    | mpp_decode 回调      | 16              | MPP 内部线程池        | 大核       | 硬件完成                    | 仅该通道 |
| 4    | vop_display          | 0/16            | CameraChannel(可选P1) | 大核       | drmModeAtomicCommit         | 仅该通道 |
| 5    | npu_worker_steady    | 1               | NpuScheduler          | 大核(独占) | 队列 take                   | 全进程   |
| 6    | npu_worker_triggered | 1               | NpuScheduler          | 大核(独占) | 队列 take                   | 全进程   |
| 7    | postproc_pool        | 3               | TrackerHub            | 大核       | 结果队列 take               | 全进程   |
| 8    | arbiter_tick         | 1               | Monitor               | 大核       | sleep 500ms                 | 全进程   |
| 9    | uds_send             | 1               | UdsServer             | 大核       | 发送队列 take               | 全进程   |
| 合计 |                      | **38~54** |                       |            |                             |          |

> 线程数上界 = 2 + 16×2(+16) + 2 + 3 + 2。线程创建/销毁仅发生在通道增删与 RECONFIG，运行期零增减。

---

## 2. 基础库 common/ 详细设计

### 2.1 DmaBuffer 与 FramePool

```cpp
// DmaBuffer.h —— 全链路流动的帧句柄（架构§6.2 "传fd不传像素"）
struct DmaBuffer {
    int      fd = -1;        // dma-buf 文件描述符
    uint32_t w = 0, h = 0;   // 分辨率（子码流如 640×480）
    uint32_t fmt = 0;        // DRM_FORMAT_NV12
    int64_t  pts = 0;        // 源 RTP 时间戳(90kHz)，随帧透传
    uint64_t frame_seq = 0;  // 抽帧计数（仅 AI 路径赋值）
    // 无析构语义：生命周期由 FramePool 经 shared_ptr 管理
};

// FramePool.h —— 每通道每用途一个池，进程启动预分配，运行期零分配
class FramePool {
public:
    // n: 池深(子码流AI池=8=RGA+NPU在途上限; 显示池=4)
    FramePool(size_t n, uint32_t w, uint32_t h, uint32_t fmt);
    ~FramePool();                                  // munmap + close 全部fd

    // 非阻塞获取；池空返回 nullptr（调用方计 drop，绝不等待）
    std::shared_ptr<DmaBuffer> acquire();
    size_t in_use() const;                         // EdgeMeter 供 Metrics 采样
private:
    struct Slab { void* map; int fd; };            // dmabuf_heap/DRM 分配
    std::vector<Slab> slabs_;
    // 空闲链表 + shared_ptr 删除器回收入池（无锁栈，单生产者=解码回调线程）
};
```

**实现要点**：

- 分配来源：`/dev/dma_heap/system`（BSP 需有 dma_heap 节点，V0 检查项）；fallback DRM dumb buffer
- `acquire()` 空返回是**设计行为**：说明下游积压，调用方（MppDecoder 回调）丢帧 + EdgeMeter.drop++
- 池深即背压内存上界：16 通道 × 8 × 640×480×1.5B ≈ **56MB**（dma-buf 计入 CMA，不计 RSS）

### 2.2 BoundedQueue

```cpp
// BoundedQueue.h —— SPSC 有界队列（架构§1 铁律2）
template <typename T, size_t Cap = 4>
class BoundedQueue {
public:
    // 满则弹最旧并 meter_.drop++；返回是否发生丢弃
    bool push_drop_oldest(T&& v);
    // 带超时 pop；超时返回 false（供线程优雅退出轮询）
    bool pop(T& out, std::chrono::milliseconds timeout);
    EdgeMeter& meter();
private:
    std::array<T, Cap> ring_;
    std::mutex m_;  std::condition_variable cv_;
    size_t head_ = 0, tail_ = 0, size_ = 0;   // 环形
};
```

- SPSC 假设：每队列只有一个生产者线程和一个消费者线程（架构§1.4 线程表保证）；跨生产者场景（如两个码流汇入）拆成两个队列
- 析构约定：进程退出时 `drain()`（pop 到空，回调收尾），配合 main 的优雅退出

### 2.3 EdgeMeter

```cpp
// EdgeMeter.h —— 每条边一个（架构§9 第2层的具体化）
struct EdgeMeter {
    std::atomic<uint64_t> in{0};             // 进入该边的帧数
    std::atomic<uint64_t> out{0};            // 离开的帧数
    std::atomic<uint64_t> drop{0};           // 异常丢弃（队列满/池空）
    std::atomic<uint64_t> sampled{0};        // 设计内采样（抽帧器令牌不通过）
    std::atomic<uint64_t> latency_us_sum{0}; // (出队时刻-入队时刻)累计
    // 不变量: in == out + drop + sampled + 队列当前深度（短暂抖动允许）
};
// 快照结构（Monitor 500ms 差分用，非累计值）
struct EdgeSnapshot { double in_rate, out_rate, drop_rate, sampled_rate, avg_latency_ms; };
EdgeSnapshot diff(const EdgeMeter& now, const EdgeMeter& base);  // 两次采样差分
```

---

## 3. core-engine 详细设计

### 3.1 CameraChannel —— 通道属主与状态机

```cpp
// CameraChannel.h —— 每路摄像头一个，通道粒度故障域（架构§9 第1层）
class CameraChannel {
public:
    struct Config {
        uint32_t cam_id;
        std::string rtsp_sub;               // rtsp://127.0.0.1:554/live/camN_sub
        std::string rtsp_main;              // 可选（P1 显示开启时）
        struct AlgoCfg {                    // 来自 set_algos
            std::string algo_id;            // 逻辑算法
            float threshold;
            uint8_t  detect_fps;            // 2~5
            std::vector<std::pair<float,float>> roi;  // 多边形，空=全画面
        };
        std::vector<AlgoCfg> algos;
    };

    void start(const Config&);              // INIT→LOADING→RUNNING
    void reconfigure(const std::vector<AlgoCfg>&);   // RUNNING→RECONFIG→RUNNING, <500ms
    void stop();                            // →STOPPED（drain 本通道队列）

    State state() const;                    // 供 get_metrics
    const ChannelMetrics& metrics() const;  // 全部 EdgeMeter 快照 + 状态字段

private:
    // ---- 成员：通道户口本（架构§9）----
    RtspPuller puller_sub_, puller_main_;   // main 可选
    std::unique_ptr<MppDecoder> dec_sub_, dec_main_;
    FramePool  pool_ai_;                    // 640×480 NV12 × 8
    BoundedQueue<std::shared_ptr<DmaBuffer>, 4> q_ai_;
    Sampler   sampler_;                     // 令牌桶抽帧
    std::unique_ptr<tracker::ByteTrack> tracker_;
    // ---- 状态机 ----
    enum class State { INIT, LOADING, RUNNING, RECONFIG, STOPPED };
    std::atomic<State> state_;
    // ---- 看门狗 ----
    std::atomic<int64_t> last_frame_ms_;    // 解码回调刷新
    void watchdog_loop();                   // 1s tick: >5000ms无帧→puller.restart()
};
```

**状态机迁移表**：

| 当前态   | 事件                    | 动作                                                                           | 次态     |
| -------- | ----------------------- | ------------------------------------------------------------------------------ | -------- |
| INIT     | start()                 | 创建池/队列/decoder（不加载模型）；启动 puller+watchdog                        | LOADING  |
| LOADING  | 首帧到达 或 模型ctx就绪 | ——                                                                           | RUNNING  |
| RUNNING  | set_algos               | drain 本通道 q_ai_ 与 sampler 令牌；增删 ModelRegistry 引用；重建 sampler 参数 | RECONFIG |
| RECONFIG | drain 完成（<500ms）    | 恢复采样                                                                       | RUNNING  |
| RUNNING  | 看门狗超时              | 仅`puller_.restart()`（decoder/池/队列保留，状态不降级）                     | RUNNING  |
| 任意     | stop()                  | 停 puller→drain→释放池→ModelRegistry 减引用→tracker 清空                   | STOPPED  |

**看门狗细节**：超时判定基于 `last_frame_ms_`（MPP 回调刷新）；重启只重建拉流会话（RTSP 重连 + SDP 重协商），**MPP ctx 保留**——海康源 H.264 SPS/PPS 不变时解码器无需重建；若重连后 SPS 变化（改了分辨率），MppDecoder 内部 `reset()`。

### 3.2 RtspPuller

```cpp
// RtspPuller.h —— 每码流一个线程
class RtspPuller {
public:
    struct Params {
        std::string url;
        // 三个关键参数压低隐藏延迟（架构§6.3，风险§16-1）
        int   stimeout_sec = 5;      // "stimeout"（ffmpeg≥5 改名 "timeout"）
        int   max_delay_ms = 100;    // av_dict: "max_delay"
        int   buffer_size  = 0;      // "buffer_size" 最小化
        // 回调（拉流线程上下文执行，实现方必须非阻塞/快速返回）
        std::function<void(const uint8_t* data, size_t len, int64_t pts)> on_nalu;
        std::function<void()> on_alive;      // 每包刷新（喂看门狗）
    };
    void start(Params);  void stop();
    void restart();                     // 看门狗触发：close当前ctx→立即重连（内部已含退避）
private:
    void pull_loop();                   // 线程主体
    int64_t pts_base_ = -1;             // 会话首帧 pts，用于归零（架构§12.5）
    // ffmpeg: avformat_open_input(rtsp/tcp) → find_stream_info(快速参数) → av_read_frame
    // 分包: AVPacket → 剥 RTP 头得完整 NALU（Annex-B 起始码透传给 MPP）
    // 重连退避: 1→2→4→...→64s 封顶（与 ZLM 源侧重试错峰, 架构§16-4）
    int backoff_ms_ = 1000;
};
```

**实现要点**：

- `avformat_find_stream_info` 必须传 `AVFormatContext.interrupt_callback`（stimeout 生效前提），且限制 probesize（默认 5MB 会让起播慢，设 512KB）
- **音视频流处理**：当前版本只取 video stream（`stream_index` 在 open 后锁定），audio NALU 丢弃（AI 无需；预览音频走 MediaServer WebRTC 直出不经本进程）
- `on_nalu` 在拉流线程执行 → 直接 `decoder_->feed()`（函数调用，无队列——架构§5.2 第一行）
- **pts 归一化（架构§12.5 前提前提）**：检测通道直拉 RTSP 拿到的是摄像头绝对大时间戳（数十亿 us），而视频通道经 ZLM `modify_stamp=2` 时间从 0 起——两端不同域。本会话首帧 pts 记入 `pts_base_`，之后 `pts = raw_pts - pts_base_` 归零，使检测 pts 与视频同域、同步增长；重连后 `pts_base_` 重置（首帧重新锚定）。

### 3.3 MppDecoder

```cpp
// MppDecoder.h —— 每码流一个
class MppDecoder {
public:
    using FrameCb = std::function<void(std::shared_ptr<DmaBuffer>)>;
    bool open(const VideoInfo& sps_info);        // mpp_create/mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingAVC/H265)
    void feed(const uint8_t* nalu, size_t len, int64_t pts);   // 拉流线程调用，仅组包+submit
    void set_frame_cb(FrameCb);                  // MPP 线程池回调 → pool.acquire→拷出dma-buf→入q_ai_
    void reset();                                // SPS 变化时 flush+重建内部上下文
    int64_t last_frame_ms() const;               // 喂看门狗
private:
    MppCtx ctx_ = nullptr;  MppApi* api_ = nullptr;
    // packets: MppPacket 队列喂 mpp; frames: mpi_decode_get_frame 轮询或回调模式
    // 输出: mpp_frame → fd via mpp_buffer_get_fd(MPP_BUFFER_TYPE_DMA_BUF)
    // SPS/PPS 缓存: 首个IDR前缓存sps/pps，reset后重发
};
```

**关键实现决策**：

- 解码输出帧**不复制**：MPP 帧的 dma-buf fd 直接包成 DmaBuffer 出回调；MPP 内部 buffer 引用计数通过 `mpp_frame_ref/deinit` 管理，确保 fd 在下游（RGA/NPU）使用期间不被解码器复写——**若 MPP 复用 buffer，则 FramePool.acquire() 后 memcpy 一次作为保险路径**（V1 实测确定，宁多一次拷贝不可花屏）
- `vdec_delay` 采集点：回调内 `now - pts_to_wallclock(pts)`，Monitor 采样

### 3.4 Sampler（令牌桶抽帧器）

```cpp
// Sampler.h —— 通道内对象，解码回调线程调用
class Sampler {
public:
    void configure(uint8_t fps);          // set_algos/degrade 调整（原子写）
    // 返回 true=该帧被选中送推理
    bool take(std::shared_ptr<DmaBuffer> f, uint64_t frame_seq);
private:
    std::atomic<uint8_t> target_fps_{3};
    // 令牌桶: 每秒补充 target_fps 个令牌, 桶容1
    // 实现: 记录 last_take_steady_, 到间隔(1s/fps)才放行——等价令牌桶(桶容1)
    std::atomic<int64_t> next_allowed_us_;
};
```

- 逻辑极简：时间片判断（`now >= next_allowed` → 放行并 `next_allowed += period`；若落后超过 2×period 则重置对齐当前——防降级恢复后连发追帧）
- 未放行的帧：`meter_.sampled++` 后**立即释放** DmaBuffer 引用（回到池，fd 生命周期纪律，架构§7.7）

### 3.5 ModelRegistry / AlgoInstance

```cpp
// ModelRegistry.h —— 进程单例（架构§11.4.2 三级结构的后两级）
struct PhysicalModel {
    std::string id;                       // "yolov8n-base"
    rknn_context ctx = 0;
    rknn_input_output_num io_num;
    std::vector<rknn_tensor_attr> input_attrs, output_attrs;
    uint32_t npu_cost_ms;                 // 启动自检刷新（口径：仅 rknn_run 一段，见下）
    uint32_t npu_mem_kb;
    uint8_t  core_mask;                   // 0x01=Core0(常驻) 0x02=Core1(触发)；rknn_init 后显式 rknn_set_core_mask
};

class ModelRegistry {
public:
    // 引用计数式获取; 首次调用触发 load(校验framework_version/platform, rknn_init(core_mask))
    std::shared_ptr<AlgoInstance> acquire(const std::string& algo_id, int* err);
    void release_all_for(uint32_t cam_id);    // 通道 stop/RECONFIG 时按句柄归还
    std::vector<PhysicalModel> snapshot() const;   // get_metrics/registry_ready 上报
    bool reload(const std::string& algo_id);       // 算法包热升级(架构§11.4.4)
private:
    std::mutex m_;                        // 仅 load/unload 持锁(冷路径)
    std::map<std::string, std::shared_ptr<AlgoInstance>> instances_;  // 按物理模型
    std::map<std::string, int> refcnt_;   // algo_id(逻辑) → 计数
};

struct AlgoInstance {
    std::shared_ptr<PhysicalModel> model;
    // 后处理参数(来自postproc.yaml): conf阈值/NMS iou/网格/anchors
    YoloPostParam post_param;             // 按算法族多态, 用 variant 或基类指针
    std::vector<std::string> classes;     // 逻辑算法类别过滤子集
};
```

**逻辑算法→物理模型解析**：`acquire("helmet-det")` 查 AlgoRegistry（manifest 内存表）得 `model_ref="yolov8n-base"`，命中同一 AlgoInstance；`classes` 按逻辑算法各自的 manifest 过滤（多逻辑算法并集，架构§11.4.2）。

**加载细节（core_mask 纪律）**：`load()` 创建 ctx 后**必须显式 `rknn_set_core_mask(ctx, core_mask)` 绑到目标核**（常驻→`RKNN_NPU_CORE_0`，触发→`RKNN_NPU_CORE_1`）。不调用时默认 `RKNN_NPU_CORE_AUTO`（随机挑一个核，且是单核），等于只用 1/3 算力且随机核直接破坏分区纪律（架构§7.3）。**npu_cost_ms 口径**：只测 `rknn_run` 一段（loop 100 中位数），**非**「拉流→解码→RGA→推理完成」的端到端耗时（后者约 40ms），否则算力预检系统性偏低、误判过载。

### 3.6 NpuScheduler（核心自研模块）

```cpp
// NpuScheduler.h
struct Job {
    uint32_t cam_id;
    int64_t  pts;  uint64_t frame_seq;
    std::shared_ptr<DmaBuffer> frame;     // 持有引用至回调返回（fd生命周期）
    LetterBoxParam lb;                    // {s, pad_x, pad_y} RGA记录(架构§12.5)
    std::shared_ptr<AlgoInstance> algo;
    uint8_t  pri = 0;                     // 触发队列: 0车牌/1人脸/2姿态/3分类(小者优先)
    std::function<void(Targets&&)> cb;    // postproc 线程池执行
};
enum class SubmitResult { ACCEPTED, DROPPED_LOW_PRI, DROPPED_BACKPRESSURE };

class NpuScheduler {
public:
    SubmitResult submit_steady(Job&&);      // 常驻: 入桶, 桶满丢最旧(drop++)
    SubmitResult submit_triggered(Job&&);   // 触发: 深度>8按pri拒新
    void set_triggered_paused(bool);        // 降级③
    void set_channel_fps(uint32_t cam, uint8_t fps);   // 降级①→Sampler
    const EdgeMeter& steady_meter() const; const EdgeMeter& trig_meter() const;
private:
    struct Bucket {
        std::shared_ptr<AlgoInstance> algo;
        std::deque<Job> q;                 // 深度上限 = Σ通道fps×0.5s 向上取整(防抖)
        uint32_t quantum_credit = 0;       // DRR 赤字计数
    };
    std::vector<Bucket> buckets_;          // 常驻桶（按物理模型）
    std::deque<Job> rq_triggered_;
    // worker 池: steady_loop×N(=NpuCorePlan.steady_cores.size())，triggered_loop×1(V1 固定)
};
```

**多核 worker 池（NpuCorePlan 分核联动）**：`start(steady_workers)` 按核分配计划起 steady worker 池（默认 1 = 历史双核行为，既有部署零变化）。桶级 in-flight 认领（busy 集合）保证同桶（同物理模型 ctx）任意时刻至多一个 worker 在飞——同 ctx 不并发、同通道回调序==提交序的保序语义不变；不同桶被不同 worker 认领，在已按 plan 绑核的 ctx 上真并行。桶容器用有序 map（稳定桶序 + 轮转起点 rr，防首桶独占致他桶饥饿）；triggered 侧 V1 固定单 worker（plan.triggered 多核仅首核生效，main 告警）。核→队列映射见架构 §7.3 NpuCorePlan：`--npu-cores steady=0,1:trig=2` 显式指定，缺省按 device-tree 识别 SoC（rk3588→steady=[0,1]/triggered=[2]，rk3576→[0]/[1]，未知→2 核兜底；解析失败回退 auto 并告警不拒启）。get_metrics 曝光 `npu.steady_cores/triggered_cores`，对账 `/sys/kernel/debug/rknpu/load`：未分配核恒 0 即分区纪律未被破坏的直接证据。

**steady_loop（分桶 WRR，DRR 赤字轮询实现）**：

```
loop:
  job = nullptr
  // 1. DRR 选桶: 每轮给活跃桶加 quantum(=该桶Σfps)信用, 首个信用>0且非空的桶取一个job, 信用-1
  for b in buckets_:
      if b.q.empty(): continue
      b.quantum_credit += b.algo->weight          // weight=Σ勾选通道fps
      if b.quantum_credit >= 1 and not picked:
          job = b.q.pop_front(); b.quantum_credit -= 1; picked=true
  //    (完整DRR: 循环直到取到job; 权重退化相等时即round-robin)
  if not job: wait_on_cv(timeout 50ms); continue

  // 2. 执行（单ctx串行, 天然保序——架构§7.7）
  rga_letterbox(job.frame, job.lb, scratch_)      // RGA crop+scale 到模型输入, 同步<1ms
  rknn_input_set / rknn_run / rknn_output_get     // 直接内存(NV12)或零拷贝fd
  targets_raw = post_decode(job.algo, outputs)    // 解码+NMS (仅本job)
  targets = inv_letterbox(targets_raw, job.lb)    // 逆变换回原始分辨率(架构§12.5)
  job.cb(std::move(targets))                      // 递交 postproc 线程池(跟踪+规则)
  // job.frame 析构(引用归零→回FramePool) —— cb 内不持有 frame
```

**triggered_loop**：无分桶，纯优先级小顶堆（pri 相同 FIFO）；每次取队首执行，流程同上第 2 步。`set_triggered_paused(true)` 时 worker 睡眠条件变量（队列仍有准入计数）。

**降级联动接口**（DegradeController 调用）：`set_channel_fps` 直接改对应通道 Sampler 的 target_fps（atomic，下个帧周期生效）；`set_triggered_paused` 如上。

### 3.7 TrackerHub

```cpp
// TrackerHub.h —— postproc 线程池内执行（串行 per channel）
struct Target {                             // 对外即 detections.targets 元素
    std::string cls;  float conf;
    float x, y, w, h;                       // 原始分辨率坐标
    uint32_t track_id;
    float vx, vy;                           // Kalman 外推速度(px/s), 前端插值用
};
class TrackerHub {
    // 每通道一个 BYTETracker（Eigen 实现, 复用 RK_VideoPipe 代码）
    void update(uint32_t cam, const Targets& dets, int64_t pts, Targets* out);
    void reset(uint32_t cam);               // RECONFIG/断流清空（track_id 重置）
};
```

- update 内部：ByteTrack 关联 → 漏检帧 Kalman 预测保持 track（最多 N=15 帧≈5s@3fps 不删）→ 输出**实际观测框**（外推框只在 track 短暂丢失时输出，标记 `predicted=true` 可选）
- vx/vy 取 Kalman 状态速度分量，经 `s` 换算回原始分辨率系

### 3.8 RuleEngine

```cpp
// RuleEngine.h —— postproc 线程内, TrackerHub.update 之后
class RuleEngine {
public:
    struct Rule {                           // 对应 camera_algos 行 + manifest trigger
        enum Type { REGION_IN, REGION_OUT, DWELL, STATE_SWITCH } type;
        std::vector<std::pair<float,float>> polygon;   // ROI
        uint32_t dwell_sec;                 // DWELL 驻留阈值
        std::string event_type;             // 如 "no_helmet"/"intrusion"
        uint32_t debounce_sec;              // 事件防抖窗(架构§10.4-6)
    };
    // 输入: 跟踪后的targets; 输出: 事件列表(带防抖合并)
    std::vector<AlarmEvent> eval(uint32_t cam, const Targets&, int64_t ts_ms);
private:
    // 防抖: (cam, event_type, cls) → 上次上报ts; 窗口内合并(快照取首帧)
    std::map<Key, int64_t> last_fire_;
};
```

- **事件快照**：eval 产出事件时，从该 job 的 frame（回调闭包仍持有）经 RGA 转 JPEG 存 `/data/snaps/{cam}/{ts}.jpg`——在 `job.cb` 闭包内做，postproc 线程执行；快照即"检测所在帧"（天然同帧取证，架构§12.6）

### 3.9 Monitor / DegradeController

```cpp
// Monitor.h —— arbiter_tick 线程, 500ms
class Monitor {
    MetricsSnapshot capture_once();   // 汇总: npu_load(sysfs×3核) / ddr(dfi) / temp
                                      //       全通道 EdgeMeter 差分 / vdec_delay
                                      //       /proc/self RSS / zlm pid RSS+CPU(读/proc/<pid>)
    void on_snapshot(MetricsSnapshot);   // 订阅者: DegradeController + UdsServer(get_metrics缓存)
};

// DegradeController.h —— 五级阶梯+滞回(架构§8.2), 状态机实现
class DegradeController {
    struct LevelCtl {
        TriggerCond cond;  uint32_t hold_ms;     // 触发保持(滞回进入)
        TriggerCond recover;  uint32_t recover_hold_ms;  // 恢复保持(滞回退出)
        std::function<void(bool enter)> action;  // 进入/退出动作
    };
    void on_snapshot(const MetricsSnapshot&);    // 每级独立计时器判定, 单调进入/退出
    // ①: rq_steady深度>阈值持续5s → 从最贵桶开始逐通道降fps(Sampler); 恢复水位<3持续30s逐级回升
    // ②: DDR>70%持续10s → VOP抽帧atomic开关+MediaServer无操作(预览抽帧由前端video.playbackRate处理,V1简化)
    // ③: npu_load>85% → set_triggered_paused(true)
    // ④: temp>85℃ → 全通道fps=1 + 写 /sys cpufreq max 限制
    // ⑤: 由businessd磁盘水位触发(engine只响应"事件限流"标志)
};
```

### 3.10 UdsServer

```cpp
// UdsServer.h —— main 线程驱动(或独立线程+命令队列)
class UdsServer {
public:
    bool start(const std::string& path /* /var/run/aibox/engine.sock */);
    // 命令处理器注册(main 装配时绑定各模块)
    void on_cmd(const std::string& cmd, CmdHandler h);
    // 事件推送(线程安全): 发送队列上限1024, 满丢最旧+计数(架构§5.3 背压)
    void emit(const json& evt);
private:
    // epoll ET; 每连接读缓冲按'\n'分行→json::parse; 解析失败回 ERR_PROTOCOL 并断开
    // 会话状态: { conn, subscriptions:set<cam_id>, last_ping }
    // 心跳: 对端5s无ping且无流量 → 关闭(EPOLLHUP天然覆盖对端死亡)
    // 每连接发送队列独立(慢消费者不拖快者); detections按 subscriptions 过滤后才入队(架构§5.3 订阅制)
};
```

### 3.11 main 装配与生命周期

```cpp
// main.cpp 伪代码
int main() {
    signal(SIGTERM/SIGINT) → g_shutdown
    // 1. 算法注册表: 扫描 /usr/share/algorithms + /data/algorithms → AlgoRegistry(校验, 架构§11.4.2)
    // 2. 单例装配: ModelRegistry / NpuScheduler(2 worker) / TrackerHub / RuleEngine
    //    / Monitor+DegradeController / UdsServer
    // 3. UDS 上报 {"evt":"registry_ready", algos:[...]}
    // 4. 事件循环: 等命令
    //    add_cam → new CameraChannel(不持久化; 重启白纸由businessd全量重建, 架构§5.3)
    //    set_algos → channels[id].reconfigure()
    //    sub_meta/unsub_meta → UdsServer 会话订阅表
    //    reload_algo → ModelRegistry.reload() + selftest(失败回滚.bak, 架构§11.4.4)
    //    get_metrics → Monitor 缓存快照回 ack
    // 5. 优雅退出: g_shutdown → 全通道 stop()(drain) → 队列 drain → UDS emit shutdown → exit 0
}
```

---

## 4. MediaServer 集成设计（零改造）

### 4.1 businessd 使用的 ZLM REST API（ZlmClient 封装）

| API                            | 用途                  | 关键参数                                                          | 失败处理                                                        |
| ------------------------------ | --------------------- | ----------------------------------------------------------------- | --------------------------------------------------------------- |
| POST /index/api/addStreamProxy | 拉摄像头（架构§4.3） | vhost/app/stream/url/rtp_type=0/enable_mp4(主1子0)/retry_count=-1 | 超时3s重试×3；同名已存在按幂等成功（先 getMediaList 查存在性） |
| POST /index/api/delStreamProxy | 删摄像头              | key                                                               | 幂等                                                            |
| GET /index/api/getMediaList    | 代理健康              | ——                                                              | Monitor 1s 轮询（经 businessd 转发到 get_metrics）              |
| GET /index/api/getServerConfig | 启动自检              | ——                                                              | 校验 record/hook 段与基线一致（防产线配置漂移）                 |

### 4.2 WebHook 端点（HookServer，:8090，仅 127.0.0.1）

| ZLM hook               | businessd 端点               | 处理                                                                                               | 响应           |
| ---------------------- | ---------------------------- | -------------------------------------------------------------------------------------------------- | -------------- |
| on_stream_changed      | POST /hook/on_stream_changed | regist→cameras.online=1/0；推 UDS 事件 camera_status 由 engine 无关，businessd 直推 WS /ws/events | `{"code":0}` |
| on_record_mp4          | POST /hook/on_record_mp4     | file/start_time/time_len → segments 插入                                                          | `{"code":0}` |
| on_flow_report（可选） | POST /hook/on_flow_report    | 流量统计入 metrics                                                                                 | `{"code":0}` |

> on_publish/on_play 鉴权 hook **暂不启用**（首版局域网，无分发鉴权需求）；公网版本启用时在此扩展，ZLM 侧仅需改 config.ini——零代码。

### 4.3 配置基线

见架构§13.2（不再重复）。install.sh 负责：写入 config.ini → `md5sum` 校验 MediaServer 二进制 → 拉起 systemd → getServerConfig 自检回读比对。

---

## 5. businessd 详细设计

### 5.1 模块结构与线程

```
businessd 进程
├── main 线程: 启动编排 + 信号
├── HttpApi (cpp-httplib, :80)        # REST + 静态web/ + WS升级
├── HookServer (cpp-httplib, :8090)   # WebHook 接收(独立端口, hook不与API抢线程)
├── UdsClient 线程                     # 连接 core-engine, 状态机+发送队列
├── MqttClient 线程(libmosquitto循环)  # 云通道(可配关闭)
└── Db (SQLite, WAL, 单写线程化: 全部SQL经串行队列)
```

线程模型简单化：httplib 自带 per-conn 线程（局域网少量并发足够）；DB 用 mutex 串行化。

### 5.2 UdsClient 状态机（全量同步，架构§5.3）

```
DISCONNECTED ──connect()──► SYNCING ──(全部add_cam/set_algos ack)──► ONLINE
     ▲                          │ ack失败(重试×2)                       │
     └────── 心跳超时/EPOLLHUP ◄─┴──────────────────────────────────────┘
SYNCING 序列:
  1. 读 SQLite: SELECT * FROM cameras WHERE enabled=1 (+camera_algos)
  2. 逐台: add_cam(cam_id, rtsp_sub, rtsp_main?)  → 等ack(5s超时→重试2次→标记该台degraded继续)
  3. 逐台: set_algos(cam_id, algos[])             → 同上
  4. 全部完成 → ONLINE; 期间收到 evt:* 丢弃(engine重启场景旧事件无意义)
ONLINE: 
  - 5s ping; 3次超时→DISCONNECTED
  - evt 转发: detections→按cam_id路由到 WsHub 订阅者; event→DB插入+WS弹窗+Mqtt outbox; 
    camera_status→DB online字段+WS; degrade→DB+WS; registry_ready→刷algorithms表
```

**通道级因果序实现**：per-cam 命令队列（add_cam/set_algos 按通道串行发送，等前一个 ack 再发下一个），事件接收不阻塞发送。

### 5.3 ZlmClient / 流编排（架构§4.3 的实现）

```cpp
// ZlmClient.h
class ZlmClient {
public:
    struct ProxyResult { std::string key; int err; };
    ProxyResult add_main(uint32_t cam, const std::string& rtsp);   // stream=cam{id}_main, enable_mp4=1
    ProxyResult add_sub (uint32_t cam, const std::string& rtsp);   // stream=cam{id}_sub,  enable_mp4=0
    void del(const std::string& key);
    json media_list();                                              // Monitor 健康轮询
};
// 编排事务性: add_main成功+add_sub成功→落库; 任一失败→回滚del已建代理→返回错误(摄像头API层)
```

### 5.4 HttpApi —— REST 完整定义

统一响应包：`{"code":0,"msg":"ok","data":...}`；错误码见 §8.2。鉴权：首版无（局域网）；预留 `Authorization: Bearer <token>`。

| #  | 方法/路径                                       | 请求                                         | data 响应                                                                                         | 副作用链                                          |
| -- | ----------------------------------------------- | -------------------------------------------- | ------------------------------------------------------------------------------------------------- | ------------------------------------------------- |
| 1  | POST /api/cameras                               | `{name, rtsp_main, rtsp_sub, enabled}`     | `{id, probe:{main:{codec,w,h,fps,ok}, sub:{...}}}`                                              | ISAPI探测(可选)→ZlmClient建代理→DB→UDS add_cam |
| 2  | GET /api/cameras                                | ——                                         | `[{id,name,online,algos:[...],...}]`                                                            | ——                                              |
| 3  | PUT /api/cameras/{id}                           | `{name?, rtsp_main?, rtsp_sub?, enabled?}` | `{ok:true}`                                                                                     | 变更→del+add代理→DB→UDS(重建)                  |
| 4  | DELETE /api/cameras/{id}                        | ——                                         | `{ok:true}`                                                                                     | UDS remove_cam→del代理→DB(逻辑删)               |
| 5  | PUT /api/cameras/{id}/algos                     | `{algos:[{algo_id,threshold,fps,roi}]}`    | `{ok:true, npu_est:{compute_pct, mem_pct}}`                                                     | **预检(架构§11.4.3)→UDS set_algos→DB**   |
| 6  | GET /api/cameras/{id}/stream?quality=sub\|main  | ——                                         | `{type:"webrtc", signaling:"http://{host}/index/api/webrtc", app:"live", stream:"cam{id}_{q}"}` | 只读                                              |
| 7  | GET /api/algorithms                             | ——                                         | `[{algo_id,name,version,type,model_ref,npu_cost_ms,npu_mem_kb,classes,trigger,...}]`            | 读 registry_ready 缓存                            |
| 8  | GET /api/recordings?camera_id&from&to&limit     | ——                                         | `{total, items:[{id,file_path,start_ts,duration_s,url}]}`                                       | segments 查询；url=MP4直链(走 :80)                |
| 9  | GET /api/events?camera_id&algo_id&from&to&limit | ——                                         | 分页事件                                                                                          | ——                                              |
| 10 | GET /api/resources                              | ——                                         | `{npu:{c0,c1,c2}, ddr_pct, temp, queue:{...}, vdec_delay_ms, rss:{engine,zlm}}`                 | UDS get_metrics 透传+本地                         |
| 11 | GET /api/system/info                            | ——                                         | `{version, uptime, cameras_online/total, disk:{total,free,retention_days_est}}`                 | ——                                              |
| 12 | POST /api/algorithms/{id}/rollback              | `{to_version}`                             | `{ok}`                                                                                          | 通知engine reload(.bak)                           |

**PUT /api/cameras/{id}/algos 预检细节**（闸门1，架构§7.6）：

```
compute: 对"去重物理模型"Σ cost_ms × Σ(勾选通道fps) ÷ (核数×1000ms×80%) → pct
mem:     Σ 去重新增ctx内存 ÷ NPU总内存×80% → pct
任一 pct>100 → code=3001/3002, msg附建议("cam3 fps 5→3 可降至87%")
```

### 5.5 WsHub

```cpp
// WsHub.h —— WS 连接与订阅路由
class WsHub {
    // /ws/events: 全局事件(event/degrade/camera_status/registry_ready) → 广播
    // /ws/meta/{cam_id}: 连接建立→UDS sub_meta(cam_id)+本组播detections
    //                    连接全断→UDS unsub_meta(cam_id)
    // 实现: map<cam_id, set<conn>>; 引用计数决定 sub/unsub(多viewer一路流只订一次)
    // 心跳: 30s ping; 发送缓冲满→断开(前端重连补拉)
};
```

### 5.6 数据库 DDL（Schema.sql）

```sql
PRAGMA journal_mode=WAL;  PRAGMA synchronous=NORMAL;  PRAGMA foreign_keys=ON;

CREATE TABLE cameras (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  name        TEXT NOT NULL,
  rtsp_main   TEXT NOT NULL,
  rtsp_sub    TEXT NOT NULL,
  zlm_key_main TEXT, zlm_key_sub TEXT,      -- MediaServer 代理key(重启用)
  enabled     INTEGER NOT NULL DEFAULT 1,   -- 0=逻辑删除
  online      INTEGER NOT NULL DEFAULT 0,
  last_heartbeat INTEGER
);

CREATE TABLE algorithms (                    -- registry_ready 同步刷新(整表replace)
  algo_id     TEXT PRIMARY KEY,
  name        TEXT, version TEXT, type TEXT,
  model_ref   TEXT, classes TEXT/*json*/, trigger_type TEXT, trigger_source TEXT,
  npu_cost_ms INTEGER, npu_mem_kb INTEGER, input_size TEXT, disabled INTEGER DEFAULT 0
);

CREATE TABLE camera_algos (
  camera_id   INTEGER NOT NULL REFERENCES cameras(id),
  algo_id     TEXT NOT NULL,
  threshold   REAL NOT NULL DEFAULT 0.45,
  detect_fps  INTEGER NOT NULL DEFAULT 3,
  roi_polygon TEXT,                          -- json [[x,y]...] 归一化0~1坐标
  enabled     INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (camera_id, algo_id)
);

CREATE TABLE events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  camera_id INTEGER NOT NULL, algo_id TEXT NOT NULL,
  type TEXT NOT NULL, ts INTEGER NOT NULL,   -- Unix ms
  snapshot_path TEXT, targets_json TEXT,
  reported INTEGER DEFAULT 0                 -- MQTT上报成功置1(outbox对账)
);
CREATE INDEX idx_events_cam_ts ON events(camera_id, ts);

CREATE TABLE segments (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  camera_id INTEGER NOT NULL, file_path TEXT NOT NULL,
  start_ts INTEGER NOT NULL, end_ts INTEGER, duration_s REAL
);
CREATE INDEX idx_segments_cam_ts ON segments(camera_id, start_ts);

CREATE TABLE outbox (                        -- MQTT 断网缓存(架构§5.5)
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  topic TEXT, payload TEXT, created_ts INTEGER
);
```

**磁盘水位**（降级⑤）：后台线程 60s 检查 `mp4SavePath` 水位>90% → 删最旧 segments 文件+行（保留天数可配）；DB 本体<50MB 不清理。

### 5.7 启动序列

```
1. 读配置 /etc/aibox/businessd.conf {zlm:{host,secret}, uds_path, http_port, mqtt:...}
2. Db open + migration(版本表)
3. HookServer :8090 起（先于 ZLM 拉起也无妨，hook失败ZLM会重试）
4. HttpApi :80 起（静态 web/ 托管）
5. UDS connect core-engine（未起则1s重试）→ SYNCING 全量编排(架构§5.2状态机)
   注意顺序: SYNCING 前 ZlmClient.getMediaList 对比 DB——**ZLM已重启过**的代理全部丢失→
   对 enabled 摄像头逐台重建 addStreamProxy（幂等）再 add_cam
6. MqttClient（可选）连云 + outbox 重传
7. 全部就绪 → /api/system/info 的 ready=true
```

### 5.8 MqttClient / OTA（概要）

- MqttClient：QoS1 publish events/outbox 逐条；broker ack → events.reported=1、outbox 删除；心跳 topic 每 60s
- OtaManager：检查更新（HTTPS）→ 下载固件/算法包（断点续传）→ 算法包走架构§11.4.4 流程；固件 A/B 切换不在本文范围（另立文档）

---

## 6. 前端详细设计

### 6.1 结构

```
web/
├── index.html          # SPA壳(哈希路由): #/cameras #/grid #/algos #/config #/events #/recordings #/settings
├── app.js              # 路由+页面装配(原生JS, 无框架; 或Vue CDN版)
├── api.js              # REST封装(含错误码toast)
├── player.js           # 播放内核: WebRTC(ZLMRTCClient) 封装
├── overlay.js          # 元数据叠加管线(核心, §6.3)
└── vendor/ZLMRTCClient.js
```

### 6.2 播放内核（player.js）

```js
// 每宫格一个实例; 单路放大=新实例(主码流), 宫格实例销毁
class CamPlayer {
  constructor(container, camId, quality /*sub|main*/) { ... }
  async start() {
    const s = await api.getStream(this.camId, this.quality);   // 拿信令地址
    this.pc = new ZLMRTCClient.Endpoint({
      element: this.video, url: s.signaling,
      streamId: s.stream, app: s.app, ... ZLM 参数
    });
    // 事件: onPlaying→标记延迟基线; onDisconnect→指数退避重建(1s→8s)
  }
  destroy() { this.pc.close(); }
}
```

### 6.3 元数据叠加管线（overlay.js —— 检测框渲染核心）

```js
class MetaOverlay {
  constructor(video, canvas) {
    this.detQ = []; this.tracks = new Map();
    this.anchorFp = null;     // pts 零点锚（null=未校准，校准后不变）；对齐方案见下
    this.videoReady = false;  // 视频首帧就绪（canplay/onPlaying 置位）
  }
  onVideoReady() { this.videoReady = true; }

  onMeta(msg) {   // WS /ws/meta/{cam_id} 每条 detections（带 frame_pts，架构§12.5）
    const fp = msg.frame_pts || 0;
    // 1. 零点锚定（仅一次）: 用"实时边缘"对拍——最新检测 fp 与视频缓冲前沿
    //    video.buffered.end（≈实时）相减即两条通道零点差（恒定）。★忌用
    //    video.currentTime（播放头，落后实时 300~800ms），否则框系统性超前画面。
    if (this.anchorFp === null && this.videoReady) {
      try { if (video.buffered.length > 0)
              this.anchorFp = fp - video.buffered.end(video.buffered.length - 1) * 1e6; }
      catch (_) {}
    }
    // 2. 记录 track 状态(位置+速度+时刻), 供插值
    for (const t of msg.targets)
      this.tracks.set(t.track_id, {x:t.x, y:t.y, w:t.w, h:t.h,
                                    vx:t.vx||0, vy:t.vy||0, t:performance.now(),
                                    cls:t.cls, conf:t.conf});
    // 3. 入队(容量64, 满丢最旧)
    this.detQ.push({fp, objs: msg.targets});
    if (this.detQ.length > 64) this.detQ.shift();
  }

  tick() {        // requestAnimationFrame 30fps 驱动
    if (this.anchorFp !== null) {
      // A. pts 逐帧对齐: 视频当前画面在"检测域"坐标 = currentTime×1e6 + 零点锚
      const vc = video.currentTime * 1e6 + this.anchorFp;
      let cur = null;
      while (this.detQ.length && this.detQ[0].fp <= vc) cur = this.detQ.shift();
      while (this.detQ.length && vc - this.detQ[0].fp > 2e6) this.detQ.shift(); // 丢陈旧
      if (cur) this.lastObjs = cur.objs;
    }
    // B. 插值: 两次检测间用 track 速度外推(架构§12.5)
    const objs = (this.lastObjs || []).map(o => {
      const tr = this.tracks.get(o.track_id);
      const dt = tr ? (performance.now() - tr.t) / 1000 : 0;
      return dt > 1.0 ? null : {...o, x: o.x + (tr?tr.vx:0)*dt, y: o.y + (tr?tr.vy:0)*dt};
    }).filter(Boolean);
    // C. 绘制: canvas尺寸=video原始分辨率(loadedmetadata时设定), strokeRect直画
    drawBoxes(this.canvas, objs);
    requestAnimationFrame(() => this.tick());
  }
}
```

**对齐细节**：弃用固定 `renderDelay`（依赖"延迟恒定"，换网络/弱网要重调，不可扩展）。改用 **pts 透传 + 前端自动对齐**：detections 携带源帧 pts，前端用 `video.buffered.end`（缓冲前沿≈实时）锚定零点差后，按 `frame_pts ≤ video.currentTime×1e6 + 零点锚` 逐帧弹出检测——网络延迟变化只会让「框跟着视频一起延迟」，不影响相对对齐，换任何网络免校准。残余偏差仅推理+采样粒度（约 0.2~0.3s）。

---

## 7. 关键流程时序

### 7.1 添加摄像头（端到端）

```
Web          businessd              ZLM(MediaServer)         core-engine
 │ POST /api/cameras │                     │                      │
 │─────────────────►│ ①addStreamProxy(main)│                      │
 │                  │────────────────────►│ ②addStreamProxy(sub) │
 │                  │◄────key_main/sub────│                      │
 │                  │ ③SQLite insert(cameras)                    │
 │                  │ ④UDS: add_cam{id,rtsp_sub,...} ───────────►│
 │                  │◄──────────────ack─────────────────────────│ ⑤CameraChannel.start
 │ {id, probe}      │                     │◄──RTSP sub 拉流──────│ ⑥RtspPuller 连ZLM回环
 │◄─────────────────│                     │                      │ ⑦(有算法时)set_algos同链路
```

### 7.2 检测帧数据流（单帧微观）

```
ZLM ──RTP──► RtspPuller线程 ──nalu(pts归零: raw-pts_base_)──► MppDecoder(feed)
                MPP线程池 ──dma-buf──► FramePool.acquire → BoundedQueue(q_ai)
消费(AI): q_ai.pop → Sampler.take(令牌) ─未过→ sampled++ 释放
                过→ NpuScheduler.submit_steady(入桶)
Core0 worker: RGA letterbox → rknn_run → decode+NMS → inv_letterbox
                → cb(Targets) → postproc线程池: Tracker.update → Rule.eval
                → UdsServer.emit(detections{frame_pts}, 按订阅过滤) → job.frame释放回池
```

### 7.3 set_algos（RECONFIG）

```
Web ──PUT /api/cameras/1/algos──► businessd: 预检(3001/3002拒绝)
     ──UDS set_algos──► core-engine:
        该通道: state→RECONFIG; q_ai drain; Sampler暂停; ModelRegistry 增减引用
        (其他通道/录像/预览零交互) → ack → state→RUNNING
     ◄── {ok, npu_est} ──   DB camera_algos upsert
```

### 7.4 摄像头断电恢复（双层重连，架构§16-4）

```
T0 相机断电:
  ZLM: 源断开 → 代理自动重试(退避, retry_count=-1) → on_stream_changed(regist=false)
       → businessd: online=0 → WS 广播徽标变灰
  engine: RtspPuller read失败 → backoff 1→2→...64s 重连(连ZLM, ZLM无流→DESCRIBE失败→继续退避)
T1 相机上电:
  ZLM 重连成功 → 流重新注册(regist=true) → businessd: online=1
  engine 下次重连成功 → 首帧 → last_frame_ms 恢复 → 看门狗清除
 tracker: RECONFIG未发生但断流>5s → TrackerHub 该通道 reset(track_id 重新计)
```

### 7.5 算法包热升级（架构§11.4.4 实现序）

```
businessd: 校验签名→解压 .new→rename 原子切换→UDS reload_algo(algo_id)
core-engine: ModelRegistry.reload():
  新ctx rknn_init(Core mask) → selftest/ 标注图推理 → mAP差>5%?
     是: rename回 .bak, 旧ctx继续, emit {"evt":"registry_ready", ...rollback}
     否: 引用>0通道的 AlgoInstance 指针原子换新(旧ctx在途job完成后释放)
  → emit registry_ready(新version/npu_cost_ms)
businessd: algorithms 表整表replace → WS 广播 → Web算法库页刷新
```

### 7.6 core-engine 崩溃自愈

```
kill -9: MediaServer录像/预览继续(独立进程) ✔
systemd 3s 后拉起: AlgoRegistry扫包 → registry_ready
businessd: UDS EPOLLHUP → DISCONNECTED → 1s重连 → SYNCING全量add_cam/set_algos
  (ZLM侧代理不受影响, 无需重建) → ONLINE → 前端WS重连(detections恢复)
验收: 录像帧级连续(架构§8.3-1)
```

---

## 8. 接口契约汇总

### 8.1 UDS 消息完整字段表（JSON-lines，架构§5.3）

**请求（businessd→engine，均需 ack）**：

| cmd                   | 字段      | 类型    | 说明                                                                      |
| --------------------- | --------- | ------- | ------------------------------------------------------------------------- |
| add_cam               | id        | u32     | cam_id                                                                    |
|                       | rtsp_sub  | string  | 回环子码流 URL                                                            |
|                       | rtsp_main | string? | 可选（P1 开启）                                                           |
| set_algos             | id        | u32     |                                                                           |
|                       | algos[]   | array   | `{algo:string, thr:f32, fps:u8, roi:[[x,y]...]\|null}`（roi 归一化 0~1） |
| remove_cam            | id        | u32     |                                                                           |
| sub_meta / unsub_meta | id        | u32     | 会话级（按 UDS 连接）                                                     |
| reload_algo           | algo      | string  | algo_id                                                                   |
| get_metrics           | ——      |         | ack data=§5.4-10 结构                                                    |

**事件（engine→businessd，可丢）**：

| evt            | 字段                                                                                     | 说明                                                                                 |
| -------------- | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| camera_status  | id, online:bool                                                                          |                                                                                      |
| registry_ready | algos[]: [{algo_id,version,model_ref,npu_cost_ms,npu_mem_kb,trigger,disabled,...}]       | 启动/热升级/回滚后                                                                   |
| detections     | id, ts:i64, frame_seq:u64, frame_pts:i64, targets[]: [{cls,conf,x,y,w,h,track_id,vx,vy}] | 坐标=原始分辨率；frame_pts=该帧归零 pts(us，前端 pts 对齐用，架构§12.5)；vx/vy px/s |
| event          | id, algo, type, ts, snap(路径), targets[]                                                | RuleEngine 产出                                                                      |
| degrade        | level:u8(0~5), reason:string                                                             | 0=全恢复                                                                             |

**ack 格式**：`{"ack":<msg_id>, "ret":0, "data":...}` / `{"ack":<msg_id>, "ret":<错误码>, "msg":"..."}`

### 8.2 错误码表

| 码   | 含义                                    | 触发点           |
| ---- | --------------------------------------- | ---------------- |
| 0    | 成功                                    |                  |
| 1001 | 参数错误                                | API/UDS 字段校验 |
| 1002 | 对象不存在（cam/algo）                  |                  |
| 1003 | 状态不允许（如 STOPPED 通道 set_algos） |                  |
| 2001 | RTSP 探测失败（连接超时/404）           | 添加摄像头       |
| 2002 | RTSP 401 账密错误                       |                  |
| 2003 | 编码不受支持（非 H.264/H.265）          |                  |
| 3001 | NPU 算力预检超限                        | set_algos 闸门1  |
| 3002 | NPU 内存预检超限                        |                  |
| 3003 | 算法包校验失败（签名/版本/平台）        | reload           |
| 3004 | 自检失败已回滚                          |                  |
| 4001 | UDS 协议错误（JSON 解析失败）           |                  |
| 4002 | ZLM REST 失败                           | businessd        |
| 5001 | 内部错误（含日志 evt=internal_err）     |                  |

### 8.3 WebHook 响应格式（businessd 统一）

```json
{"code": 0, "msg": "success"}
// 预留: on_publish 鉴权版 {"code":0, "enable":true/false, "msg":"..."}
```

---

## 9. 算法包 manifest 完整 Schema（v1）

```yaml
# 必填字段(校验失败→disabled, 架构§11.4.2)
algo_id:          string  # ^[a-z0-9-]+$
name:             string
version:          string  # semver
type:             enum[det, cls, pose, lpr, face]
target_platform:  list    # [rk3588] / [rk3588, rk3576]
input_size:       [u32, u32]
input_format:     enum[NV12, RGB]
framework_version: string  # "rknn-2.3.2"
trigger:          enum[primary, triggered]
classes:          list    # 逻辑算法: 物理模型类别子集
# 条件必填
model_ref:        string  # 逻辑算法必填; 物理模型必须为 null
model_file:       string  # 物理模型必填 "models/xxx.rknn"
trigger_source:   string  # trigger=triggered 时必填(一级算法id+类)
# 可选
npu_cost_ms:      u32     # 启动自检刷新
npu_mem_kb:       u32
default_threshold: f32    # 默认0.45
default_fps:      u8      # 默认3
scene_tags:       list
changelog:        string
```

配套文件：`labels.txt`（行=类名，序号对齐模型输出）、`postproc.yaml`（conf/nms_iou/anchors 或 strides）、`selftest/`（图+期望JSON）。

---

## 10. 日志与可观测性

### 10.1 日志格式（两进程统一）

```
结构化JSON行 → stdout(journald 收集) + 按天轮转文件 /data/log/{engine,business}.log
字段: ts(墙钟ms) lvl(INFO/WARN/ERROR) mod(media/infer/...) evt(短事件名)
      cam(可空) ...自由字段    例:
{"ts":1760000000123,"lvl":"WARN","mod":"media","evt":"reconnect","cam":3,
 "reason":"no_frame_5s","backoff_ms":8000,"pull_attempts":3}
```

- 级别纪律：热路径**禁 INFO 逐帧日志**（detections 流不落日志）；WARN=自愈事件；ERROR=需要人工介入
- 帧级调试开关：env `AIBOX_TRACE_CAM=3` 时该通道打印 pts/frame_seq/queue 深度（性能损耗仅限调试）

### 10.2 指标暴露

| 消费者       | 来源                        | 通道               |
| ------------ | --------------------------- | ------------------ |
| Web 资源面板 | UDS get_metrics(1s) + /proc | GET /api/resources |
| 降级决策     | Monitor 500ms               | 进程内             |
| 云监控(可选) | businessd 汇总              | MQTT 心跳附带      |

---

## 11. 测试设计要点（对应架构§14，本文只列实现级用例）

| 阶段         | 用例（新增实现级）                                                                                                                                                                        |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 单元         | FramePool 并发 acquire/回收无泄漏(fd计数=valgrind)；BoundedQueue 丢旧正确性；Sampler 令牌率=配置；DRR 权重公平性(模拟桶注入统计服务率)；inv_letterbox 数值回归(合成框)；RuleEngine 防抖窗 |
| 集成(engine) | 假流源(本地文件→RtspPuller 直灌)打满16通道；kill 源 100 次(架构§14-V1)；SPS 切换(推流端改分辨率)→decoder reset 不崩                                                                    |
| 集成(业务)   | add_cam 事务回滚(ZLM子代理失败→主代理清理)；UDS SYNCING 部分失败降级；磁盘水位删除循环                                                                                                   |
| 端到端       | §7 六个时序各至少 1 个自动化脚本；pts 对齐（帧 pts 透传 + 前端 buffered 锚定）Mp4/WebRTC 双链路验证                                                                                      |
| 性能         | npu_bench: 每模型 loop100 取中位写入 manifest；回环架构税实验(架构§14)                                                                                                                   |

---

## 12. 附录：配置与目录清单

| 路径                                        | 内容                                        | 归属             |
| ------------------------------------------- | ------------------------------------------- | ---------------- |
| /opt/aibox/bin/{core-engine,businessd}      | 二进制(static libstdc++)                    | 自研             |
| /opt/aibox/zlm/                             | MediaServer+config.ini+www(空,仅回放直链用) | ZLM 锁 commit    |
| /opt/aibox/web/                             | 静态 SPA                                    | 自研             |
| /etc/aibox/businessd.conf                   | zlm host/secret、uds_path、mqtt             | businessd        |
| /var/run/aibox/engine.sock                  | UDS（tmpfs，重启自清）                      | engine           |
| /data/algorithms/ · /usr/share/algorithms/ | 算法包（架构§11.4.1）                      | engine 读        |
| /data/record/                               | MP4 切片（ZLM 写）                          | ZLM              |
| /data/snaps/ · /data/log/                  | 快照 / 日志                                 | engine/businessd |
| systemd 三 unit                             | 架构§13.1 原文                             | deploy/          |

**交付校验**：install.sh 末尾打印版本锁定表核对结果（架构§15.2），任一不符即失败退出。

---

*（完）本文与架构方案 V1.1 构成完整设计基线；实现过程中对接口的任何偏离须先改文档再改代码。*
