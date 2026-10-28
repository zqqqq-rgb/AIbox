# AIBox 任务分解与测试规划 V1.0

> 文档日期：2026-09-22
> 上游文档：《AIBox 软件方案 · 独立式进程架构 V1.1》（架构§N）、《AIBox 软件详细设计_独立式_V1.0》（详设§N）
> 定位：开发排期基线——WBS 任务卡片、依赖关系、单测用例清单、CI 流水线。任务颗粒度=单人 1~3 天。
> 核心原则：**先建测试地基（接缝+Fake），再排功能任务**——硬件依赖不解耦，单元测试无从谈起。

---

## 目录

```
0  AI 执行与人工确认总则（任务字段规范·验证命令·人工确认点·gate 机制·AI 执行纪律）
1  可测试性三前提（骨架阶段纪律）
2  里程碑总览与依赖图（含 gate 断点）
3  WBS 任务卡片（M0~M6）
    附：任务确认矩阵（每任务：验证命令 + 人工确认点）
4  单元测试用例清单（按任务）
5  CI 流水线设计
6  覆盖率与验收门槛
7  风险与缓冲（含 demo 已复现坑清单）
8  与架构验证计划(架构§14)的映射
```

---

## 0. AI 执行与人工确认总则

> 本节规定：本 WBS 如何被 AI 逐任务执行、人如何逐步确认。核心诉求——**每一步都能被人确认，AI 不得在无人工确认的情况下"一路冲到底"**。

### 0.1 执行单元与字段规范

AI 执行的**最小不可分割单元 = 一个任务卡片**（T 编号）。每个任务卡片除原有「交付物 / 依赖 / 接缝 / DoD / 遗留项」外，本节起**强制配套**下面两个字段（统一收在 §3 末「任务确认矩阵」里）：

| 字段                 | 含义                                                                                      | 谁提供     |
| -------------------- | ----------------------------------------------------------------------------------------- | ---------- |
| **验证命令**   | AI 声称"做完了"前**必须自己在板/CI 上跑通、且输出符合预期**的命令（含期望输出样例） | 本文档写死 |
| **人工确认点** | 人（或复核者）看完**哪个具体输出/数字/日志**，即判定该任务 gate 通过                | 本文档写死 |

**规则**：任务没有「验证命令」和「人工确认点」两个字段的，**不允许开始执行**；本节把全部任务的这两字段**补齐**（§3 附矩阵）。

### 0.2 门禁（gate）机制——AI 必须停下等确认

- **任务级 gate**：每个任务完成后，AI 输出「验证命令的运行结果 + 对人工确认点的自述」，**停止**，等人工确认「通过 / 驳回」。
- **里程碑级 gate**：关键里程碑（M0、M2、M3、M4）结束设**硬 gate**——未过人工确认，AI 不得开始依赖该里程碑的下游任务。
- **驳回重做**：人工确认「驳回」时，必须给一条具体原因；AI 只修该原因，**不擅自扩大改动面**。

### 0.3 AI 执行纪律（防埋雷）

1. **单步提交**：一次只做一个任务；不得跨任务、不得"顺手重构"无关代码；
2. **先写测试**：T 编号任务对应 §4 用例必须先落（或同步落），再写实现（沿用文档 §1 可测试性纪律）；
3. **不注入未要求能力**：禁止"顺手加"未在任务卡片的特性、参数、抽象；
4. **证据留痕**：每个任务完成后附「改动文件清单 + 验证命令输出 + 确认点自证」，供人复核；
5. **卡点上报**：验证命令跑不通或与确认点不符时，**报告并停下**，不靠猜改变验收标准。

### 0.4 基于 demo 实测的「资产复用」纪律（`aibox-pipeline-demo` 已做过的，不重复做）

`aibox-pipeline-demo` 的**单路全链路技术可行性已验证完毕**（MPP 解码→RGA letterbox→RKNN 推理→后处理→WS 出框均跑通），并产出两类可直接被 M3 复用的资产：

1. **叶子实现**（可直接进入产品，无需重做）：`letterbox 数学 + RGA 调用`、`yolo 后处理（DFL decode + NMS）`、`MPP 解码正确用法`；
2. **确定性坑清单 K1~K12**（§7 附表）：已复现、有明确修法，M3 写真件时须自检不重踩；
3. **实测基线数据**：`npu_cost_ms`（loop100 中位）、端到端耗时（~40ms）、fd 直出成立、pts 对齐成立。

**纪律**：T3.x（MppDecoder / RgaPreproc / ModelRegistry / npu_bench）实现时**直接复用上述资产**，不重新做单路验证；demo 的单路写死骨架（pipeline.cpp）**不进入产品**，多路架构按详设重写。**不设独立"单路切片"里程碑**（demo 已覆盖，避免重复）。

---

## 1. 可测试性三前提

### 1.1 接缝设计（Seam）——硬件依赖全部藏在接口后

四个硬件触点各抽一个接口，板端实现与 Fake 分离：

| 接口              | 板端实现                             | Fake 实现（x86 CI 可用）                                     | 详设对应      |
| ----------------- | ------------------------------------ | ------------------------------------------------------------ | ------------- |
| `IStreamSource` | RtspPuller（ffmpeg 拉回环流）        | FileSource（本地 h264 裸流按 pts 节拍回放）                  | 详设§3.2     |
| `IDecoder`      | MppDecoder（MPP 硬解，dma-buf 出帧） | FileDecoder（直接从文件读预解码帧序列）                      | 详设§3.3     |
| `IPreproc`      | RgaPreproc（RGA letterbox）          | SoftLetterbox（CPU 参考实现，**兼黄金对照答案**）      | 详设§3.6     |
| `IInfer`        | RknnEngine（rknn_run）               | FakeInfer（可编程：固定耗时+预设输出框，验证调度/跟踪/规则） | 详设§3.5/3.6 |

```cpp
// 接口形态示例（engine/interfaces/，仅示意，字段以详设为准）
struct Frame { /* pts, frame_seq, fd|pixels, w, h */ };
struct Targets { /* cls, conf, x,y,w,h (原始分辨率) */ };

class IStreamSource { public: virtual void start(Cb) = 0; virtual void restart() = 0; };
class IDecoder      { public: virtual void feed(nalu, pts) = 0; virtual void set_cb(FrameCb) = 0; };
class IPreproc      { public: virtual LetterBoxParam letterbox(const Frame&, Frame& out) = 0; };
class IInfer        { public: virtual int run(const Frame&, const AlgoInstance&, Targets&) = 0; };
```

### 1.2 CMake 三目标——纯逻辑必须 x86 CI 可编译

```
aibox_core   # 纯逻辑: letterbox数学/DRR调度/令牌桶/规则/协议编解码/状态机
             #   —— 零硬件头文件(CI 强制检查: include rknn_api.h/mpp/rga 即编译失败)
aibox_hw     # MPP/RGA/RKNN 封装(实现上述接口) —— 仅交叉编译
core-engine  # core + hw 装配(板端)
businessd    # 独立目标(依赖均为可mock的http/sqlite/uds)
```

**纪律**：`aibox_core` 硬件头文件检查进 PR 门禁；违反=CI 红。

### 1.3 黄金对照——软件实现验证硬件实现

`SoftLetterbox` 与 `RgaPreproc` 对同一帧输出逐像素比对，差异>0.5px 判 RGA 用法错误；同理 `FakeInfer` 的预设框序列用于在 x86 上"重放"板端推理结果。硬件没有断言可用，这是最省事的硬件行为验证法。

---

## 2. 里程碑总览与依赖图

**依赖图（含硬 gate 断点 ★）**：

```
M0 测试地基 ★gate ──► M1 common ──► M2 engine逻辑 ★gate
   │                   │             │      │
   │                   │             │      └──► M3 板端硬件层 ★gate ──┘→ M4 ★gate → M6 端到端
   └──► M5 前端 ──► M6                    (M3 复用 demo 叶子代码 + K1~K12 + 实测基线)
```

| 里程碑         | 内容                                                           | 阻塞关系                                     | 硬 gate（§0.2）         | 对应架构验证     |
| -------------- | -------------------------------------------------------------- | -------------------------------------------- | ------------------------ | ---------------- |
| **M0**   | 构建骨架+接口+Fake+测试数据                                    | **阻塞全部**；T0.2 接口评审=全局检查点 | ★ 接口评审通过才放行    | ——             |
| **M1**   | common 层 6 任务（100% 纯逻辑）                                | 依赖 M0                                      | 无（纯逻辑）             | ——             |
| **M2**   | engine 逻辑层 6 任务（Fake 注入）                              | 依赖 M1；与 M3 部分并行                      | ★ 五组 DRR/准入用例全绿 | V2 前置          |
| **M3**   | 板端硬件层 5 任务（复用 demo 叶子代码 + K1~K12 + 实测基线）    | 依赖 M0 接口；T3.4 依赖 M2                   | ★ 板上假件 16 通道稳定  | V1/V2            |
| **M4**   | businessd 6 任务                                               | 依赖 M0（接口）+M1（协议）；Fake engine mock | ★ SYNCING 状态机全绿    | V4 前置          |
| **M5**   | 前端 2 任务                                                    | overlay 依赖 M1（协议契约）；player 手工     | 无（DOM 手工）           | V4               |
| **M6**   | 端到端脚本化 6 时序                                            | 依赖 M3+M4+M5                                | —                       | V1~V4 各阶段验收 |

> **gate 语义**：标 ★ 的里程碑是「硬确认断点」，AI 执行到此处必须输出该里程碑的验证结果与人工确认点自证并**停下**，等人确认「通过」后才可进入下游（§0.2）。
> **demo 资产复用**：`aibox-pipeline-demo` 已验证单路全链路可行性，其「叶子代码 + K1~K12 坑清单 + 实测基线」作为 **M3 的输入**直接复用（§0.4），不设独立"单路切片"里程碑——避免重复验证。

**并行建议**（假设 3 人：A=engine、B=businessd/前端、C=算法/板端）：

```
周1     : 全员 M0 (接口评审全员参与)
周1~2   : A→M1 / B→M4(Fake engine) / C→M3(T3.1/T3.2 复用 demo 叶子代码)
周2~3   : A→M2 / B→M4+M5 / C→M3(T3.3 用 demo npu_cost_ms 基线)
周4~5   : A→M2收尾+T3.4装配 / B→M4收尾+M5 / C→M3收尾+T3.5 npu_bench
周6+    : M6 端到端 (按架构§14 V1→V4 顺序)
```

---

## 3. WBS 任务卡片

> 卡片字段：交付物 / 依赖 / 接缝 / DoD（Definition of Done）/ 遗留项。单测用例明细见 §4。

### M0 测试地基

#### T0.1 构建骨架

- 交付：三目标 CMake（aibox_core/aibox_hw/core-engine + businessd + web 静态目录）+ x86 CI 流水线（编译即绿）+ clang-format/tidy 基础规则
- 依赖：无
- DoD：空实现下 `cmake --build` 全绿；CI 触发一次成功

#### T0.2 接口定义与评审 ★全局检查点

- 交付：`engine/interfaces/` 四接口头文件 + `Frame/Targets/LetterBoxParam` 数据结构定稿
- 依赖：T0.1
- DoD：**接口评审会通过**（全员：按详设§3 逐模块核对方法签名/回调上下文/所有权语义）；评审重点=接缝位置是否切在硬件边界（letterbox 数学必须在 core，RGA 调用必须在 hw）
- 风险注记：接缝切错位置（如数学进了 hw 层）会导致后续单测全废——本任务是最高优先级检查点

#### T0.3 Fake 四件套

- 交付：FileSource（h264 裸流按 pts 节拍回放，支持倍速）/ FileDecoder（预解码帧序列文件）/ SoftLetterbox（CPU 实现）/ FakeInfer（配置文件指定：每算法固定耗时+输出框序列）
- 依赖：T0.2
- DoD：各 Fake 自带最小自测（能跑通 start→回调链）

#### T0.4 测试数据资产

- 交付：①一段 60s 真实摄像头 h264 裸流（板端/ffmpeg 录制）；②对应 FakeInfer 预设框序列（真实 YOLO 输出采样导出 JSON）；③合成边界用例集（空目标/满目标/越界框）
- 依赖：无（可与 T0.3 并行）
- 复用声明：tracker（T2.3）/rule（T2.4）/前端插值（T5.1）三层测试共用本资产
- DoD：数据入库 `tests/assets/`，README 说明来源与再生成方法

### M1 common 层

#### T1.1 EdgeMeter 与差分

- 交付：EdgeMeter + `diff()` 快照差分（详设§2.3）
- 依赖：T0.1
- 接缝：无（纯逻辑）
- DoD：单测过（§4-T1.1）；核心逻辑覆盖 ≥80%

#### T1.2 BoundedQueue

- 交付：SPSC 有界队列（详设§2.2）
- 依赖：T0.1
- DoD：单测过；drain 语义（优雅退出用）验证

#### T1.3 FramePool

- 交付：预分配池 + fd 引用回收（详设§2.1）；**x86 版用普通内存模拟 dma-buf**（接口同 fd 语义），板端编译时换 dmabuf_heap 分配——池逻辑本身在 core
- 依赖：T0.2（Frame 结构）
- DoD：并发 10 万次 acquire/回收 fd 计数归零（泄漏检测）；池空返回 nullptr 路径

#### T1.4 Letterbox 数学

- 交付：`compute_letterbox(src, model_input) → {s, pad_x, pad_y}` + `inv_letterbox(targets, lb)`（详设§12.5 契约）
- 依赖：T0.2
- DoD：**黄金用例表全过**（640×480→s=1/pad_y=80；640×360→pad_y=140；854×480→s≈0.749；1080p ROI 裁剪路径）；正逆变换往返误差=0

#### T1.5 Sampler 令牌桶

- 交付：时间片抽帧器（详设§3.4）
- 依赖：T0.1
- DoD：单测过（含防追帧重置）

#### T1.6 UDS 协议编解码

- 交付：消息结构体 ↔ JSON-lines（详设§8.1 全字段表）
- 依赖：T0.1（jsoncpp）
- DoD：全消息类型往返序列化测试；坏 JSON→错误码 4001

### M2 engine 逻辑层（Fake 注入）

#### T2.1 NpuScheduler ★最高测试投入

- 交付：分桶 DRR + 触发优先级队列 + 准入 + 降级联动（详设§3.6 完整伪代码）
- 依赖：T1.2（队列）、T0.3（FakeInfer）
- 接缝：IInfer=FakeInfer（固定耗时模拟负载）
- DoD：§4-T2.1 五组用例全过；覆盖率 ≥80%；两 worker 绑核行为留 M3 验证
- 遗留：绑核亲和（L3）

#### T2.2 CameraChannel 状态机

- 交付：通道属主+迁移表+看门狗（详设§3.1）
- 依赖：T1.2/T1.3/T1.5、T0.3（FileSource/FileDecoder）
- DoD：迁移表全行覆盖；RECONFIG<500ms（假 decoder）；看门狗仅 restart puller 不降状态

#### T2.3 TrackerHub

- 交付：ByteTrack 封装 + vx/vy 输出 + reset（详设§3.7）；ByteTrack 本体从 RK_VideoPipe 移植进 aibox_core
- 依赖：T0.4（回放数据）
- DoD：回放测试——track_id 连续性/丢失保持≤15 帧/速度方向正确；Eigen 移植编译进 core（无 RK_VideoPipe 其余依赖）

#### T2.4 RuleEngine

- 交付：四类规则+防抖窗+快照触发闭包（详设§3.8）
- 依赖：T0.4
- DoD：防抖窗合并正确；点在多边形判定单测；DWELL 计时（mock 时钟）

#### T2.5 UdsServer

- 交付：epoll JSON-lines 服务端+订阅过滤+慢消费者保护（详设§3.10）
- 依赖：T1.6
- DoD：loopback 自连测试全套（§4-T2.5）

#### T2.6 Monitor / DegradeController

- 交付：500ms 快照+五级阶梯滞回状态机（详设§3.9）
- 依赖：T1.1
- 接缝：指标源=可注入合成序列（接口 `IMetricsSource`，板端=sysfs 实现）
- DoD：温度爬坡序列→④进入/退出按滞回参数且无抖动；降级①联动 Sampler（需 mock 调度器接口验证调用）

### M3 板端硬件层（L3，板上跑）

#### T3.1 MppDecoder

- 交付：IDecoder 板端实现（详设§3.3）
- 依赖：T0.2、**demo 叶子代码复用（aibox-pipeline-demo 的 MPP 正确用法，§0.4）**
- 测试：真流帧计数；**SPS 切换 reset 不崩**（推流端中途改分辨率）；**dma-buf 复用问题实测**（详设§3.3 标注的决策点：直出 vs 保险 memcpy）
- DoD：24h 真流零崩溃；决策记录写入本文档附录
- **demo 已证实的确定性坑（必须内建于实现与自测，§7 附表）**：
  1. 非阻塞 `decode_get_frame` 无帧返回 `MPP_OK+NULL` → 循环必须 break，不得 continue；
  2. `decode_put_packet` 返 `MPP_ERR_BUFFER_FULL(-1012)` → 先 `pull` 排空再重试（活锁）；
  3. 帧 buffer 组在 info-change 分支挂载（非 open 时提前挂）；
  4. 非阻塞超时 `MPP_SET_OUTPUT_TIMEOUT=0` 合法（非 0 才能非阻塞）。
- 遗留：板上批量 16 通道的 MPP 并发复用行为

#### T3.2 RgaPreproc + 黄金对照

- 交付：IPreproc 板端实现
- 依赖：T0.2、T1.4（黄金参数）、**demo 叶子代码复用（aibox-pipeline-demo 的 RGA 判据，§0.4）**
- 测试：与 SoftLetterbox 同帧逐像素比对（>0.5px 差异=失败）；RGA 2 线程串行喂压测（架构§16 RGA 内部锁）
- DoD：对照脚本入 tools/，可在板上重复执行
- **demo 已证实的坑**：RGA `improcess`/`imresize` 成功返回 `IM_STATUS_SUCCESS(=1)` 而非 0，判据用 `ret <= 0`（否则成功被当失败，队列空）

#### T3.3 ModelRegistry(RKNN) + AlgoRegistry

- 交付：rknn ctx 管理+算法包扫描校验+selftest/回滚（详设§3.5 + 架构§11.4.2/11.4.4）
- 依赖：T0.2、算法包样例 1 个（含坏包 1 个）、**demo 实测基线（aibox-pipeline-demo 的 npu_cost_ms，§0.4）**
- 测试：manifest 校验全路径（版本错/平台错/缺文件→disabled）；热加载新旧 ctx 切换；selftest 失败自动回滚 .bak
- DoD：样例包全场景脚本通过
- **demo 已证实的坑（必须写入 DoD）**：
  1. `load()` 创建 ctx 后**显式 `rknn_set_core_mask` 绑目标核**（常驻→Core0/触发→Core1）；不调用默认 `RKNN_NPU_CORE_AUTO`=随机单核，等于只用 1/3 算力且破坏分区纪律（架构§7.3）；
  2. 查询 attr/output 前设 `.index=i`（否则 SIGSEGV）；
  3. `npu_cost_ms` 只测 `rknn_run` 段（loop100 中位），非端到端（~40ms）。

#### T3.4 core-engine 板端装配

- 交付：main.cpp 装配（详设§3.11）——**先用 FileSource+FakeInfer 板上全链路**（验证骨架），再逐步换真件
- 依赖：M2 全部 + T3.1~T3.3
- 测试：假件 16 通道 30 分钟稳定性；换真件后冒烟
- DoD：UDS add_cam→detections 全链路出数

#### T3.5 npu_bench 工具

- 交付：模型耗时实测工具（loop100 中位→manifest npu_cost_ms，架构§11.3）
- 依赖：T3.3
- DoD：6 个模型实测值入库
- **口径**：只测 `rknn_run` 一段（纯 NPU），**非**「拉流→解码→RGA→推理完成」端到端；工具须同时输出两数以防混用（架构§7.5 口径纪律）

### M4 businessd

#### T4.1 Db + DDL + 迁移

- 交付：Schema.sql/迁移框架/WAL 配置（详设§5.6）
- 依赖：T0.1
- 测试：内存 SQLite 建表/外键约束/迁移幂等
- DoD：单测过

#### T4.2 UdsClient 状态机

- 交付：DISCONNECTED→SYNCING→ONLINE 全量同步（详设§5.2）
- 依赖：T1.6（协议）、**FakeEngine**（新写：~150 行 mock 进程，按脚本文件回 ack/事件——脚本驱动，可重放"部分失败/中途断线"场景）
- DoD：状态机全路径（含 SYNCING 部分失败降级、心跳超时、ZLM 重启代理重建序列）

#### T4.3 ZlmClient + 流编排事务

- 交付：REST 封装+add/del 编排（详设§5.3）
- 依赖：T0.1
- 接缝：ZLM HTTP mock（本地起假 8080 应答脚本）
- DoD：**事务回滚用例**——add main 成功+add sub 失败→主代理被 del（防半配置）；同名幂等

#### T4.4 HttpApi 12 条

- 交付：详设§5.4 全表实现+错误码
- 依赖：T4.1/T4.2/T4.3
- DoD：每条 API 正常+错误路径（2002 密码错/3001 预检超限带建议文案）单测过

#### T4.5 WsHub + HookServer

- 交付：WS 订阅路由+WebHook 端点（详设§5.5/§4.2）
- 依赖：T4.2
- DoD：多 viewer 引用计数（2 连接只 sub 一次，全断才 unsub）；on_record_mp4→segments 插入

#### T4.6 磁盘水位 + Outbox

- 交付：循环删除+断网缓存重传（详设§5.6/5.8）
- 依赖：T4.1
- 测试：伪文件系统注入（目录塞假文件>90% 触发删最旧）
- DoD：脚本用例过

### M5 前端

#### T5.1 overlay 纯函数化 + 单测

- 交付：detQueue 淘汰/**pts 零点锚定 + frame_pts 逐帧对齐**/插值外推/离场判定/绘制计算抽成**无 DOM 纯函数**（`overlay_core.js`）+ Vitest 测试；DOM 层薄壳
- 依赖：T1.6（detections JSON 契约含 `frame_pts`）、T0.4（回放数据）
- DoD：纯函数单测——①pts 锚定：视频首帧就绪时用 `buffered.end` 锚定零点差（锚后不再变）；②逐帧对齐：弹出所有 `frame_pts ≤ currentTime×1e6 + 锚` 的检测取最新；③两次检测间按 vx/vy 线性推进；④超 1s 删 track；⑤队列容量 64 丢最旧（详设§6.3 对齐，原 30→64）
- **对齐机制变更**（demo 实测结论）：弃固定 `renderDelay`，改 **pts 透传 + 前端 buffered 锚定自动对齐**（换网络免校准）；锚定忌用 `video.currentTime`（播放头落后实时 300~800ms）

#### T5.2 player + 页面

- 交付：CamPlayer（ZLMRTCClient 封装+重连）+ 七页面骨架（详设§6）
- 依赖：M4 API 可用
- DoD：手工清单（宫格/单路切换/事件弹窗/回放）；E2E 留 M6

### M6 端到端（对应架构§14）

| 任务 | 内容                                 | 验收线                                  |
| ---- | ------------------------------------ | --------------------------------------- |
| T6.1 | 时序脚本：详设§7.1 添加摄像头端到端 | 全链路自动跑通                          |
| T6.2 | 时序脚本：§7.2/7.3 数据流+RECONFIG  | RECONFIG<500ms 且录像零影响             |
| T6.3 | 时序脚本：§7.4 断电恢复             | kill 源 100 次恢复 100 次（架构 V1 线） |
| T6.4 | 时序脚本：§7.5 算法包热升级+回滚    | 切换无 crash、坏包自动回滚              |
| T6.5 | 时序脚本：§7.6 三进程 kill 矩阵     | 架构§8.3 三条验收线                    |
| T6.6 | V1~V4 完整验证（回环架构税实验含）   | 架构§14 表全绿                         |

---

## 3 附：任务确认矩阵（每任务：验证命令 + 人工确认点）

> 本节是 §0.1「验证命令 / 人工确认点」两字段的落地。AI 每完成一个任务，必须**跑通验证命令并把结果**（连同人工确认点自证）输出给人；人核对下表「人工确认点」判定通过/驳回。

| 任务 | 验证命令（AI 自证）                                                              | 人工确认点（人看什么判通过）                                |
| ---- | -------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| T0.1 | `cmake --build . --target all` 全绿 + CI 触发一次成功                          | CI 页面显示一次绿；三目标产物存在                           |
| T0.2 | 评审材料（四接口头文件+数据结构）逐条比对详设§3                                 | 接口签名/回调上下文/所有权与详设一致，全员签字              |
| T0.3 | 各 Fake 自带自测跑通                                                             | 能看到 FileSource→回调链输出                               |
| T0.4 | `ls tests/assets/` + README 说明                                               | 60s h264 裸流 + FakeInfer 框序列 JSON 存在且来路清楚        |
| T1.1 | 单测 + 覆盖率报表                                                                | `in==out+drop+sampled+depth` 全采样点成立                 |
| T1.2 | 单测                                                                             | push 满 4 弹最旧、drop=1                                    |
| T1.3 | 单测（含 -fsanitize=address 跑 10 万次）                                         | 无泄漏、池空返回 nullptr                                    |
| T1.4 | 单测                                                                             | 黄金表 4 组 s/pad 逐值相等、往返误差 0                      |
| T1.5 | 单测                                                                             | 25fps→3fps 放行约 30/10s、防追帧重置                       |
| T1.6 | 单测                                                                             | 全消息往返逐字段相等；坏 JSON→4001                         |
| T2.1 | 单测（五组）+ 覆盖率 ≥80%                                                       | DRR 服务比∈[3.8,4.2]、触发准入拒新、保序                   |
| T2.2 | 单测                                                                             | 迁移表全行、RECONFIG<500ms、看门狗只 restart                |
| T2.3 | 回放测试                                                                         | track_id 连续、漏检≤15 帧保持、vx 方向对                   |
| T2.4 | 单测                                                                             | 防抖窗合并、多边形三态、DWELL 计时                          |
| T2.5 | loopback 自连测试                                                                | 订阅过滤、慢消费者不拖快者                                  |
| T2.6 | 合成指标序列注入                                                                 | 温度抖动只进入一次（滞回）                                  |
| T3.1 | 板上 24h 真流 + SPS 切换脚本                                                     | 零崩溃；SPS 切换不崩；决策记录入库                          |
| T3.2 | 板上对照脚本（tools/）                                                           | 与 SoftLetterbox 逐像素 <0.5px                              |
| T3.3 | 样例包全场景脚本                                                                 | manifest 校验/热切换/坏包回滚全过；**ctx 已显式绑核** |
| T3.4 | 假件 16 通道 30min + 冒烟                                                        | UDS add_cam→detections 出数                                |
| T3.5 | `npu_bench` 跑 6 模型                                                          | 每模型输出纯 NPU + 端到端两数，中位写入 manifest            |
| T4.1 | 单测（内存 SQLite）                                                              | 建表/外键/迁移幂等                                          |
| T4.2 | FakeEngine 脚本重放                                                              | SYNCING 全路径 + 断线全量重建计数                           |
| T4.3 | ZLM HTTP mock                                                                    | 事务回滚（main 成功 sub 失败→del main）                    |
| T4.4 | 单测                                                                             | 12 条 API 正常+错误路径、3001 带建议                        |
| T4.5 | 单测                                                                             | 双 viewer 只 sub 1 次、全断 unsub                           |
| T4.6 | 伪文件系统注入                                                                   | 95% 水位删最旧                                              |
| T5.1 | `npm test overlay_core`（Vitest）                                              | pts 锚定/逐帧对齐/插值/离场/队列 64 全过                    |
| T5.2 | 手工清单                                                                         | 宫格/单路/事件/回放可用                                     |
| T6.x | 对应 M6 时序脚本                                                                 | 见各 T6 验收线                                              |

---

## 4. 单元测试用例清单（按任务，实现前先写）

### T1.x common（断言明确，全部 x86 毫秒级）

| 任务 | 用例                         | 断言                                       |
| ---- | ---------------------------- | ------------------------------------------ |
| T1.1 | 并发 4 线程混写 in/out/drop  | 任意采样点`in == out+drop+sampled+depth` |
| T1.1 | 差分基准                     | 两次快照差分速率正确（构造 10 帧/500ms）   |
| T1.2 | push 满 4 再 push            | 最旧被弹、drop=1、新帧在队尾               |
| T1.2 | pop 超时                     | 100ms 超时返回 false 不阻塞                |
| T1.3 | 10 万次 acquire/随机延迟回收 | 结束后 in_use()==0（无泄漏）               |
| T1.3 | 池空 acquire                 | 返回 nullptr（调用方计 drop，不等待）      |
| T1.4 | 黄金表 4 组分辨率            | s/pad 参数逐值相等；往返误差<1e-6          |
| T1.4 | ROI 裁剪路径                 | 裁剪偏移还原正确                           |
| T1.5 | 25fps 流 3fps 采样 10s       | 放行数∈[29,31]                            |
| T1.5 | 运行中 3→1fps               | 下一秒起放行率立即≈1                      |
| T1.5 | 人为落后 3 周期              | 重置对齐当前时刻（不连发追帧）             |
| T1.6 | 全消息往返                   | 结构→JSON→结构 逐字段相等                |
| T1.6 | 截断/坏 JSON                 | 返回 4001，连接不断                        |

### T2.x engine 逻辑

| 任务 | 用例                                            | 断言                                                          |
| ---- | ----------------------------------------------- | ------------------------------------------------------------- |
| T2.1 | DRR 公平性：桶A权重24/桶B权重6，注入 60s 假任务 | 服务次数比 ∈ [3.8, 4.2]                                      |
| T2.1 | 准入：触发队列填 8 后再 submit                  | 返回 DROPPED_LOW_PRI，计数+1                                  |
| T2.1 | 优先级：混入 pri 0~3 各若干                     | 出队顺序严格按 pri 升序                                       |
| T2.1 | 降级联动：mock 阶梯①触发                       | 对应通道 Sampler target_fps 原子值变更                        |
| T2.1 | 保序：同通道 100 job 乱序完成                   | 回调顺序==提交顺序                                            |
| T2.2 | 迁移表逐行                                      | 每行一个用例，动作/次态断言                                   |
| T2.2 | RECONFIG 计时                                   | <500ms 且期间录像/显示通路 meter 零变化                       |
| T2.2 | 看门狗：last_frame 停 5.1s                      | puller.restart() 恰好 1 次，state 不变                        |
| T2.3 | 回放 60s 数据                                   | track_id 无跳变（同目标）；漏检≤15 帧保持；vx 方向与轨迹一致 |
| T2.4 | 防抖：同类事件连发 10 次/5s 窗                  | 仅 1 条 AlarmEvent，快照=首帧                                 |
| T2.4 | 多边形：内/外/边上点                            | 三态判定正确                                                  |
| T2.5 | 订阅过滤：sub cam1，双通道发流                  | 仅收 cam1 detections                                          |
| T2.5 | 慢消费者：不读的连接灌 2000 条                  | 其发送队列封顶丢最旧，其他连接不受影响                        |
| T2.6 | 温度 84→86→84→86（抖动注入）                 | ④只进入一次（滞回），恢复需 <75℃ 持续                       |

### T4.x businessd（mock：FakeEngine / ZLM HTTP mock / 内存 SQLite）

| 任务 | 用例                                      | 断言                                                              |
| ---- | ----------------------------------------- | ----------------------------------------------------------------- |
| T4.2 | SYNCING 脚本：第 3 台 add_cam ack 失败×2 | 标记 degraded 继续，其余完成，终态 ONLINE                         |
| T4.2 | ONLINE 后 FakeEngine 断连                 | →DISCONNECTED→重连→全量重建（FakeEngine 计数收到全部 add_cam） |
| T4.3 | 编排事务：main 成功/sub 失败              | ZlmClient 收到对应 del(main_key)；DB 无残留                       |
| T4.4 | set_algos 预检超限                        | code=3001，msg 含"fps 5→3"建议                                   |
| T4.5 | 双 viewer 同通道                          | UDS sub_meta 恰好 1 次；两连接都收流；全断后 unsub 1 次           |
| T4.6 | 伪磁盘 95% 水位                           | 最旧 segments 文件+行被删，最新保留                               |

### T5.x 前端

| 任务 | 用例                                    | 断言                                    |
| ---- | --------------------------------------- | --------------------------------------- |
| T5.1 | pts 锚定：fp=5000000，buffered.end=0.5s | anchorFp=4500000（=5000000-0.5e6）      |
| T5.1 | 逐帧对齐：anchorFp 定后注入 fp 序列     | 只弹出 fp ≤ currentTime×1e6+锚 的检测 |
| T5.1 | 插值：t0 框(100,100) vx=50              | t0+400ms 绘制位置 x=120                 |
| T5.1 | 离场：track 超 1s 无更新                | 下帧不绘制                              |
| T5.1 | 队列：注入 70 条                        | 长度=64（丢最旧）                       |

---

## 5. CI 流水线

```
PR 触发（必须 <5 分钟）:
  1. 三目标 x86 编译 + businessd 编译
  2. aibox_core 硬件头文件扫描（grep rknn_api/mpp/rga → 失败）
  3. L1+L2 全部单测（Fake 注入）+ 覆盖率报表（aibox_core ≥80%）
  4. clang-format/tidy diff 检查

Nightly（有板 CI Runner 时）:
  5. aibox_hw 交叉编译
  6. 部署 RK3588 + L3 冒烟（T3.x 对照脚本）
  7. Fake 件 16 通道 30min 稳定性

手动 tag（阶段验收）:
  8. M6 对应时序脚本 + 架构§14 当前阶段验收线
```

**门禁规则**：1~4 任一红=不可合并；覆盖率仅 core 门禁（见 §6），不卡整体。

---

## 6. 覆盖率与验收门槛（分级）

| 模块                                           | 行覆盖门禁      | 理由                        |
| ---------------------------------------------- | --------------- | --------------------------- |
| common/（T1.x）                                | **≥80%** | 纯逻辑，无借口              |
| NpuScheduler / RuleEngine / TrackerHub（T2.x） | **≥80%** | 产品复杂度所在              |
| CameraChannel / UdsServer / businessd 逻辑     | ≥60%           | 有胶水成分                  |
| aibox_hw（T3.x）                               | 不设门禁        | 逻辑薄，靠黄金对照+板上冒烟 |
| 前端 overlay_core.js                           | ≥80%           | 纯函数                      |
| 前端 DOM/页面                                  | 不设            | 手工+E2E                    |

DoD 统一模板：`代码+单测+CI绿+覆盖率达标+任务卡片遗留项记录`。**单测先行**：用例清单进卡片后才开写实现。

---

## 7. 风险与缓冲

| # | 风险                                          | 影响      | 缓冲                                          |
| - | --------------------------------------------- | --------- | --------------------------------------------- |
| 1 | T0.2 接口返工                                 | 全局      | 评审会全干系人参与；预留 2 天缓冲（M0 内）    |
| 2 | T3.1 dma-buf 复用问题成立→需 memcpy 保险路径 | 性能/工期 | 详设§3.3 已预案；M3 首周即验证（前移）       |
| 3 | ByteTrack 移植依赖纠缠（Eigen 版本）          | T2.3      | 提前一天做依赖剥离 spike                      |
| 4 | FakeEngine 脚本能力不足（复杂时序重放难写）   | T4.2      | 简化：先覆盖三主路径，复杂场景借 M6 板上验证  |
| 5 | RGA 黄金对照差异>0.5px（格式语义误解）        | T3.2      | 差异分析工具（hexdump 两实现输出）；预留 2 天 |
| 6 | 板端 CI Runner 不具备（夜跑缺板）             | M3 节奏   | L3 降级为手动清单+SOP，验收线不降             |

### 附表：demo 已复现坑清单（AI 执行时的"确定性坑"速查，对应 §0.4）

> 以下是 `aibox-pipeline-demo` 单路实测**已确定复现、有明确触发条件与修法**的坑。AI 实现 T3.x/T5.1 时必须自检，不得重踩；人工确认时按本表核对。

| #   | 层       | 现象                                     | 根因                                                                           | 修法（须写入实现）                                                             |
| --- | -------- | ---------------------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------ |
| K1  | MPP      | dec=0fps，卡在 fed SPS/PPS               | 非阻塞`decode_get_frame` 无帧返回 `MPP_OK+NULL`，循环 `continue` 死循环  | 无帧即`break` 返回继续喂包                                                   |
| K2  | MPP      | `put_packet err -1012` 刷屏            | 输入任务队列满 + put 失败直接 return 不排空，三方活锁                          | `MPP_ERR_BUFFER_FULL` 时先 `pull` 排空再重试                               |
| K3  | RGA      | `improcess failed: Run successfully`   | RGA 成功返回`IM_STATUS_SUCCESS(=1)`，判据写成 `!=NOERROR(2)`               | 判据`ret <= 0`                                                               |
| K4  | RKNN     | Segmentation fault（首次推理）           | `rknn_tensor_attr.index`/`rknn_output.index` query/get 前未设 `.index=i` | 查询/get 前设`index=i`                                                       |
| K5  | 后处理   | 段错误                                   | `(const float**)outs` 把 `vector<float>*` 强转 float* 传给后处理           | 构造`const float*[]` 用 `vector::data()`                                   |
| K6  | 后处理   | 框坐标全错                               | DFL 通道访问 stride=1 连续读，误把相邻像素当 bin                               | DFL 逐通道取（stride=hw）                                                      |
| K7  | WS 握手  | `client connected` 后立即 `recv FIN` | 自实现 base64「余 2 字节」缺`<<2`；SHA1 变量更新错序                         | 用库或按 RFC 测试向量验证 base64/SHA1                                          |
| K8  | WS 维持  | 握手成功后 5s 内断开 ws=0                | 非阻塞 recv/send 把`EAGAIN` 误判为断开                                       | 仅对端 FIN/真实错误才 drop，排除 EAGAIN/EWOULDBLOCK/EINTR                      |
| K9  | NPU      | 只用 1/3 算力                            | 未调`rknn_set_core_mask`，默认 `AUTO`=随机单核                             | ctx 创建后显式绑核（Core0/Core1）                                              |
| K10 | 时间对齐 | 框比画面早/晚                            | 双通道 pts 不同域 + 零点不同步                                                 | pts 归零透传`frame_pts` + 前端 `buffered.end` 锚定（忌用 `currentTime`） |
| K11 | 播放     | 视频延迟 3~5s                            | 全在浏览器 FLV/MSE 层，非源端                                                  | 过渡期 flv.js 延迟属固有，最终 WebRTC 300~800ms；勿误判                        |
| K12 | 口径     | 算力预检偏高                             | 把「拉流→推理完成」端到端(~40ms) 当`npu_cost_ms`(仅 rknn_run)               | npu_cost_ms 只测 rknn_run 一段                                                 |

---

## 8. 与架构验证计划（架构§14）的映射

| 架构阶段    | 本文档任务                                       | 备注                                         |
| ----------- | ------------------------------------------------ | -------------------------------------------- |
| V0 环境基线 | T0.1（板侧为 install.sh/交叉编译，另行任务）     | 板侧 V0 清单不在本 WBS（运维任务）           |
| V1 媒体链路 | T3.1/T3.4 假件部分 + T6.1/T6.3（复用 demo 叶子代码 + K1~K12） | MediaServer 侧零开发 |
| V2 AI 链路  | T2.x 全部 + T3.2/T3.3/T3.5 + T6.2/T6.4           | NpuScheduler=T2.1 主战场                     |
| V3 资源仲裁 | T2.6 + T6.5 kill 矩阵                            | 烤机 72h 为运维执行项                        |
| V4 业务闭环 | M4+M5 全部 + T6.6                                |                                              |

---

*（完）本文与架构 V1.1、详设 V1.0 构成三层基线：为什么→怎么做→谁先做怎么验。实现期接口偏离须先改文档。*
