# AIBox —— RK3588 智能视频分析盒（独立式三进程架构）

> 产品代码库。上游基线文档（位于上级目录 impl/）：
>
> - 《AIBox_软件方案_独立式_V1.0.md》—— 架构方案（为什么）
> - 《AIBox_软件详细设计_独立式_V1.0.md》—— 编码级详设（怎么做）
> - 《AIBox_任务分解_V1.0.md》—— 任务分解与测试规划（谁先做、怎么验）

## 编译目标分层

| 目标         | 内容                       | 编译环境  |
| ------------ | -------------------------- | --------- |
| aibox_core   | 纯逻辑，零硬件头文件       | x86 / 板  |
| aibox_hw     | MPP/RGA/RKNN 封装          | 仅板上    |
| core-engine  | core + hw 装配             | 仅板上    |
| aibox_business | businessd 逻辑（REST/WS/SQLite/ZLM/UDS 客户端） | x86 / 板 |
| businessd    | 业务进程                   | x86 / 板  |

## 构建

```bash
# x86（开发机/CI）：只编 aibox_core + businessd
./scripts/build.sh

# 板上（RK3588，需 MPP/RGA/RKNN 交叉工具链）
./scripts/build.sh --hw

# 带单测
./scripts/build.sh --tests
```

## 验证（对应任务分解 T0.1 DoD）

```bash
./scripts/build.sh            # 预期：编译全绿，产出 build/engine/libaibox_core.a 与 build/business/businessd
./scripts/check-headers.sh    # 预期：OK，aibox_core 无硬件头文件
```

## 里程碑进度

- M0 测试地基：完成（板上验证全绿）
  - [x] T0.1 构建骨架
  - [x] T0.2 接口定义（engine/interfaces/ 四接缝 + 数据结构定稿）
  - [x] T0.3 Fake 四件套 + 最小自测（tests/test_fakes.cpp）
  - [x] T0.4 测试数据资产（scripts/gen_test_assets.py 合成流 + 预设真值）
- M1 common 层：完成（板上验证 7/7 全绿）
  - [x] T1.1 EdgeMeter + diff 差分（common/EdgeMeter.h）
  - [x] T1.2 BoundedQueue SPSC + drain（common/BoundedQueue.h）
  - [x] T1.3 FramePool 位图空闲链 + x86 模拟分配器接缝（common/FramePool.*）
  - [x] T1.4 Letterbox 数学黄金表 + ROI 逆变换（common/LetterboxMath.*，SoftLetterbox 已复用）
  - [x] T1.5 Sampler 时间片抽帧 + 防追帧重置（media/Sampler.*，虚拟时钟可测）
  - [x] T1.6 UDS 协议编解码全字段 + 4001（ipc/Protocol.*，jsoncpp）
- M2 engine 逻辑层：完成（板上验证 13/13 全绿）
  - [x] T2.1 NpuScheduler：DRR 双队列 + newest-wins 准入 + 降级联动（infer/NpuScheduler.*）
  - [x] T2.2 CameraChannel：五态状态机 + 看门狗 + EdgeMeter 四边（media/CameraChannel.*）
  - [x] T2.3 TrackerHub：IoU+质心速度跟踪、漏检预测、vx/vy、低分回收（track/TrackerHub.*）
  - [x] T2.4 RuleEngine：多边形三态 + REGION_IN/OUT/DWELL/STATE_SWITCH + 防抖 + 状态按规则隔离（rules/RuleEngine.*）
  - [x] T2.5 UdsServer：epoll 多客户端 + 订阅路由 + 慢消费者背压 + 4001 断连（ipc/UdsServer.*）
  - [x] T2.6 Monitor + DegradeController：五级降级迟滞状态机 + 采样节拍（arbiter/DegradeController.*）
- M3 板端硬件层：完成（板上验证中）
  - [x] T3.1 RtspPuller（hw/RtspPuller.*）：ffmpeg libavformat 拉流，SPS/PPS Annex-B 首包（K1）、pts 归零透传、interrupt_callback 看门狗
  - [x] T3.2 MppDecoder（hw/MppDecoder.*）：K2~K6 坑内建（MPP_OK+NULL break、-1012 先 pull、get_frame 超时循环、输出计数、fd/ptr 双通道）；rga_golden 黄金对照工具（tools/rga_golden.cpp）
  - [x] T3.3 RgaPreproc（hw/RgaPreproc.*）：NV12→RGB888 letterbox，dma-buf fd 直通 + 保险 memcpy 路径，K9 判据 ret<=0
  - [x] T3.4 RknnEngine + ModelRegistry（hw/RknnEngine.* / model/ModelRegistry.*）：zoo 9 输出反量化、K10 attr .index=i、K11 显式绑核、K12 npu_cost_ms 只测 rknn_run；RegistryDeps 工厂注入零硬件依赖（x86 CI 可测），selftest 失败→disabled、reload 失败→.bak 回滚
  - [x] T3.5 main.cpp 板端装配：fake/真件双模式 + UDS 全命令（add_cam/remove_cam/set_algos/reload_algo/get_metrics）+ PostProc MPSC 单消费队列 + 降级五级联动；DmaHeapAllocator（hw/DmaHeapAllocator.*）/ NpuMetricsSource（hw/NpuMetricsSource.*）
  - [x] T3.6 npu_bench（tools/npu_bench.cpp）：npu_cost_ms（仅 rknn_run，权威）+ e2e_ms（参考）双口径输出防混用
- M4 businessd：完成（板上验证 23/23 全绿，2026-10-09）
  - [x] T4.1 Db + DDL + 迁移（business/db.*）：详设 §5.6 全表 + WAL + schema_meta 版本迁移（幂等）； Stmt 预编译 + Txn RAII + 全 SQL 递归锁串行
  - [x] T4.2 UdsClient 状态机（business/uds_client.*）：DISCONNECTED→SYNCING→ONLINE；SYNCING 前 ensure_proxies（ZLM 代理重建）+ 逐台 add_cam/set_algos（5s ack 重试×2，degraded 继续）；ONLINE 心跳=get_metrics（连续 3 次超时断线）；断线重连自动重发 sub_meta；cmd_mu_ 全局串行=因果序
  - [x] T4.3 ZlmClient + 流编排事务（business/zlm_client.*）：自写极简 HTTP 客户端（GET 参数走 query string）；ensure_proxy 幂等（getMediaList 查存在性）；add_camera_proxies 事务（任一失败回滚已建代理）
  - [x] T4.4 HttpApi 12 条（business/http_api.*）：详设 §5.4 全表 + 错误码 §8.2；npu_precheck 纯函数（Σcost×Σfps ÷ 核数×800ms，3001/3002 + "camN fps →3 可降至 XX%"建议）；cpp-httplib REST + web//recordings 静态挂载
  - [x] T4.5 WsServer/WsHub + HookServer（business/ws_server.* / hook_server.*）：自写 RFC6455（httplib 不支持 WS 升级）——SHA1/base64 握手、引用计数（多 viewer 只 sub_meta 一次）、30s ping/60s 超时、每连接 1024 帧背压；HookServer :8090（on_stream_changed/on_record_mp4，form+json 双格式兼容）
  - [x] T4.6 磁盘水位 + Outbox（business/janitor.*）：DiskJanitor（60s 巡检：保留天数清理 + 90% 水位循环删最旧，batch_max 上限）；OutboxRetransmitter（IMqttPublisher 接缝，ack 后删行 + events.reported 对账）
  - [x] T4.7 main 装配（business/main.cpp）：详设 §5.7 启动序（配置→Db→Hook→Ws→Uds→HttpApi→Janitor→ready）+ 事件五路分发（detections→WS 组播 / event→DB+WS+outbox / camera_status→DB+WS / degrade→WS / registry_ready→algorithms 表）+ 优雅退出

## 依赖

```bash
# 板上/开发机（Ubuntu）：T1.6 需要 jsoncpp；M4 businessd 需要 sqlite3 与 cpp-httplib
apt install libjsoncpp-dev libsqlite3-dev libcpp-httplib-dev
```

## M0~M2 验证命令（DoD）

```bash
./scripts/build.sh --tests       # 预期：编译全绿
ctest --test-dir build           # 预期：13/13 通过（fakes + M1 六项 + M2 六项）
./scripts/check-headers.sh       # 预期：纯逻辑目录无硬件头文件
python3 scripts/gen_test_assets.py   # 生成合成资产（64x48 × 30帧 + 预设真值）
```

## M3 验证命令（DoD，板上）

```bash
./scripts/build.sh --hw --tests  # 预期：全绿（含 MPP/RGA/RKNN/FFMPEG 探测 + core-engine + npu_bench + rga_golden）
ctest --test-dir build           # 预期：16/16 通过（13 + yolov8_post + algo_package + model_registry）
./scripts/check-headers.sh       # 预期：OK（含 engine/model 门禁）

# 算法包（放 packages/ 下，manifest.json platform 须匹配 AIBOX_TARGET_PLATFORM）
./build/engine/npu_bench --model <包内>.rknn --core steady --loop 200
#   输出 manifest 回填片段：npu_cost_ms 用"仅 rknn_run"口径，勿与 e2e_ms 混用
./build/engine/rga_golden --frame <nv12> --w W --h H --model-w 640 --model-h 640 --dma
#   几何判据 scale/pad 差 ≤0.5px 即 PASS（exit 0）

# 端到端冒烟（fake 源走 UDS 全命令；真件模式去掉 --fake 换 --video）
./build/engine/core-engine --fake --packages ./packages --uds /tmp/engine.sock
# 另一终端：add_cam → 订阅 detections → set_algos → reload_algo → get_metrics
# 注意：M3 无 businessd 心跳，默认 --uds-idle-ms 0 禁用空闲掐断；M4 接 businessd 时用 5000
```

## M4 验证命令（DoD，x86 CI/板上）

```bash
./scripts/build.sh --tests          # 预期：全绿（新增 aibox_business + businessd）
ctest --test-dir build              # 预期：23/23 通过（16 旧 + db/uds_client/zlm_client/http_api/ws_hub/janitor）
./build/businessd --config businessd.conf   # 板上：联调 core-engine（UDS SYNCING→ONLINE）
# 浏览器 http://<板IP>/ → /api/system/info ready=true → /api/cameras → /ws/events
```

### M4 三进程联调冒烟（板上）

```bash
./scripts/build.sh --hw --tests
./scripts/smoke_m4.sh               # Tier A 无 ZLM：同步/API/WS/重启重建 12 项断言
SMOKE_ZLM_SECRET=<secret> ./scripts/smoke_m4.sh   # ZLM 在跑时加测 Tier B 全链路
```

脚本自动编排：engine(--fake NV12) + businessd(临时端口 18080/18081/18090 + 临时
DB) → 只读 API → DB 直插摄像头验证 SYNCING 全量同步 → WS detections 探针
（scripts/ws_probe.py）→ kill engine 验证下线感知与全量重建 → ZLM 可用时加测
POST/PUT/DELETE 摄像头全链路。退出码 0 = 全过。

businessd.conf 示例（JSON，各字段缺省值见 business/main.cpp Config）：

```json
{
  "http_port": 80, "ws_port": 8081, "hook_port": 8090,
  "uds_path": "/tmp/aibox-engine.sock",
  "db_path": "/var/lib/aibox/businessd.db",
  "recordings_dir": "/data/mp4", "keep_days": 30,
  "static_dir": "/opt/aibox/web",
  "zlm": {"host": "127.0.0.1", "port": 80, "secret": "<ZLM api secret>"}
}
```

## M5 验证命令（DoD，板上）

```bash
# 1. 前端纯函数单测（x86/开发机）
cd web && npm install && npm test     # 预期：24/24（overlay_core 22 + 模块导入 2）

# 2. 契约冒烟（板上；Tier A 无 ZLM）
./scripts/smoke_m5.sh                 # 预期：静态托管 9 文件 / SYNCING 同步 / stream
                                      # 信令(sub|main) / WS meta D3 契约（frame_w/frame_h）/
                                      # PUT algos npu_est / events+recordings envelope /
                                      # engine 重启→WS events registry_ready 全过，exit 0

# 3. 浏览器手工验收（脚本项全过后）
#    浏览器打开 http://<板IP>:<http_port>/ 按 web/README.md 手工清单过 11 项
#    （框压人 / 宫格切换 / kill engine 自愈 / 3001 拒绝 / 事件弹窗 / MP4 回放）
```
