# businessd 第一阶段第四批验收记录

日期：2026-10-29。本批补齐第三批留下的进程崩溃、ACK 丢失、补偿失败、错误分类和自动重连验证，并冻结第一阶段的接口状态约定。

结论：第一阶段 B0+B1 的无硬件自动化验收已完成。21 项验收场景在本文定义的模拟件和故障点范围内通过，可进入 V2 任务阶段。此结论不包含真板、真实摄像头、真实 ZLM/RKNN 联调，也不代表完整产品已完成。

## 本批改动

| 文件 | 内容与目的 |
| --- | --- |
| tests/test_business_recovery.cpp | 新增真实 businessd 子进程故障矩阵；SIGKILL 后用原 SQLite、原模拟引擎和原媒体状态重启，验证清理与历史保留 |
| tests/test_camera_service.cpp | 补偿本身失败、add_cam ACK 丢失、旧执行权收尾时自动重连、命令错误分类、重复目标复用、superseded 终态 |
| tests/test_uds_client.cpp | SYNCING 的 add/set/remove 分别遇到 1002/1003，断言安全重建、降级与持久恢复，不全局吞掉错误 |
| tests/test_http_api.cpp | 部分同步失败、后台恢复、断线时查询真实状态；验证最近同步统计不会被误解释为实时故障数 |
| tests/fake_engine.h、tests/mock_zlm.h | 命令执行前/后、代理删除后可控暂停；单次错误脚本；按订阅发送检测；MockZlm RAII 清理线程 |
| engine/ipc/UdsServer.h/.cpp | 增加 disconnect_clients：对现有会话 shutdown，监听器继续运行，关闭和回收仍由 IO 线程负责。用它确定性验证自动重连，未改变线协议 |
| business/reconciler.h/.cpp | 将媒体恢复失败原因写入操作错误；恢复旧代理也失败时明确记录 compensation pending，文本不含源 URL/凭据 |
| business/db.h/.cpp | 查询摄像头当前版本的操作，供相同待执行目标复用；无需新迁移 |
| business/camera_service.cpp | 相同完整配置正在等待重试时复用原 operation_id/revision；删除墓碑可复用未完成的删除操作；旧等待遇 superseded 立即结束 |
| business/http_api.cpp | superseded 不再输出 pending=true，避免前端把已被替代的操作当成仍将生效 |
| web/tests/api.test.js | accepted=true、code=1003、pending=false 的 superseded 响应保持错误分支，不显示保存成功 |
| tests/CMakeLists.txt | 注册 business_recovery，加入 businessd_phase1 标签 |

复用比较完整快照，算法按 ID 排序后比较；没有新增 hash 列。新增摄像头仍是独立创建意图。执行器持有配置锁或处于 SYNCING/断线时仍按既有 busy 约定拒绝新写入；复用发生在可准入的 pending/retry_wait 等未完成目标上，不保证运行中请求绕过 busy。

## 强杀与重启验证

删除、停用分别覆盖以下 5 个故障点，共 10 个 SIGKILL 场景：

| 故障点 | 强杀前的断言 |
| --- | --- |
| 目标已落库、remove_cam 尚未执行 | desired_revision 增加，applied_revision 未变；原引擎通道及两路代理仍存在 |
| remove_cam 已执行、ACK 尚未返回 | 引擎通道已消失，两路代理仍存在，操作未确认 |
| 主代理删除完成、HTTP 应答尚未返回 | 引擎与主代理消失，子代理仍存在 |
| 子代理删除完成、HTTP 应答尚未返回 | 引擎与两路代理均消失，操作仍未确认 |
| 清理完成、最终数据库确认受阻 | SQLite 触发器故意阻止 applied_revision 更新；资源已停止但结果不能显示成功 |

每一项都核对：SIGKILL 确实终止原进程；旧请求没有成功响应；重启后相同目标版本最终确认成功；资源清理完成；事件及录像索引各保留一条；删除墓碑默认隐藏，停用记录仍可查询。

这是进程强杀测试，不是整机断电、磁盘损坏或相机断电测试。最后一个故障点利用 SQLite 触发器保持“资源已清理但不能确认”的窗口，解除注入后才允许重启完成确认；生产库没有这些测试触发器。

另有完整 businessd 重启测试：引擎及媒体状态保留，算法阈值、FPS、ROI 和驻留规则完整重放，已有媒体代理不被删除；WS 客户端重新连接后，按通道订阅的检测消息可以再次收到。测试子进程关闭继承的模拟服务器 FD，避免因测试进程继承连接而掩盖真实断线。

## 第一阶段接口约定

既有路径和 HTTP 业务响应 code/msg/data 包装保留。下表描述配置写入响应；GET 操作查询的 code=0 仅表示查询成功，操作是否完成仍看 data.status。

| 写入结果 | code | accepted | pending | 含义 |
| --- | --- | --- | --- | --- |
| 完整应用并持久确认 | 0 | true | false | 可以显示已生效 |
| 已保存、等待/重试/结果暂不可读 | 1003 | true | true | 保留 operation_id，查询进展 |
| 配置锁忙、SYNCING、断线或停止 | 1003 | false | false | 本次未接受，没有新增目标版本 |
| 引擎明确拒绝且判为终态 | 具体错误码 | true | false | apply_status=failed，目标已保存但未完整生效 |
| 已被后续目标替代 | 1003 | true | false | apply_status=superseded，该操作不会继续执行 |
| 校验、预算或意图落库失败 | 对应错误码 | false | false | 不生成半条目标与操作 |

pending/running/retry_wait 是未完成状态。failed/superseded 是当前操作的失败或替代结果。succeeded 表示该目标最近被完整确认；操作表追踪目标应用状态，并非不可变的审计日志：同一当前目标在新连接重放失败后可以重新进入 retry_wait。成功历史不能证明新引擎已持有配置，因此重连仍会重放。

错误处理按命令与阶段区分：

| 命令/阶段 | 1002 | 1003 |
| --- | --- | --- |
| ONLINE add_cam | 失败，不报告成功 | 一次 remove/add 保守重建，仍失败则重试 |
| ONLINE set_algos | 先保守恢复通道并重发全量；仍 1002 则 failed | 保留未完成状态，退避重试 |
| ONLINE remove_cam | 明确不存在，可继续媒体清理 | 不当作已删除，保留资源/确认差异并重试 |
| SYNCING add_cam | 本通道 degraded，保存恢复依据 | 一次保守重建，不能仅凭 duplicate 认为配置一致 |
| SYNCING set_algos | 本通道 degraded，转持久恢复 | 本通道 degraded，转持久恢复 |
| SYNCING remove_cam | 已不存在，可确认停止 | 本通道 degraded，转持久恢复 |

本阶段其他既有错误码保留：1001 参数、3001/3002 预算、4002 媒体传输、5001 内部存储等。错误文本是最近一次尝试的诊断；媒体补偿完成也不代表新目标已生效，最终仍以 revision/status 为准。补偿失败时会明确显示 compensation pending，解除故障后可重试收敛。

GET /api/system/info 的状态口径：

- ready：本地服务启动完成，不等于所有摄像头健康。
- lifecycle_state：STARTING/RUNNING/STOPPING。
- engine_state：DISCONNECTED/SYNCING/ONLINE。
- engine_write_ready：查询时配置服务运行、引擎在线且配置执行权可获得；不预留下一次写入权利，也不要求 pending_operations=0。
- pending_operations：待执行或重试的配置操作数。
- sync_degraded_cams：最近一次同步的失败通道统计，后台修复后仍保留该次统计，直到下一次同步更新。
- 每个摄像头的 apply_status/revision：目标与确认差异；runtime_state 只有在引擎在线且该状态确认时为 confirmed，否则 unknown。

列表的 enabled 表示目标启停，online 只表示当前通道在线口径，两者都不能替代完整配置的应用状态。GET /api/operations/{id} 不返回内部快照及 RTSP 凭据。

## 最终自动验收矩阵

PASS 限定为已有自动测试中的明确断言，未宣称穷尽所有调度顺序和硬件故障。第三批的未覆盖/PARTIAL 条目由本批对应测试补齐。

| 编号 | 结果 | 主要证据 |
| --- | --- | --- |
| A01 禁用算法后应用和重连 | PASS | compiler、camera_service、uds_client：实际集合及预算排除禁用项 |
| A02 空算法集合 | PASS | camera_service：清除检测，媒体与通道保留 |
| A03 换流/重新启用 | PASS | http_api、camera_service、business_recovery：完整算法、ROI、阈值与规则恢复 |
| A04 只重启 businessd | PASS | business_recovery：完整子进程重启，原引擎状态保留，重复通道安全重建、重新订阅可收检测 |
| A05 执行后 ACK 丢失 | PASS | camera_service：分别在 add_cam、set_algos 执行后断线，不假报完成，随后恢复目标 |
| A06 1002/1003 分类 | PASS | camera_service、uds_client：add/set/remove × ONLINE/SYNCING 分类断言 |
| A07 已有主代理/子代理失败 | PASS | zlm_client：已有主代理不被回滚删除 |
| A08 改源与补偿失败 | PASS | camera_service：补偿成功和补偿本身失败；状态/错误可查询，解除故障后恢复 |
| A09 ZLM 单独重启 | PASS | camera_service：UDS 持续在线时恢复缺失代理 |
| A10 意图/确认落库失败 | PASS | db、camera_service：原子回滚、拒绝假成功、解除确认故障后重试 |
| A11 事务隔离与失败 | PASS | db：第二线程隔离、嵌套 BEGIN 拒绝、COMMIT 失败与析构回滚 |
| A12 并发超预算 | PASS | camera_service：只准入预算内组合，未确认旧负载继续占预算 |
| A13 删除/停用中强杀 | PASS | business_recovery：2 类操作 × 5 个故障点，重启清理并保留历史 |
| A14 SYNCING 写请求 | PASS | camera_service：accepted=false、版本和代理无副作用 |
| A15 同版本也重放 | PASS | uds_client、business_recovery：历史确认不跳过新会话重放 |
| A16 端口冲突 | PASS | business_startup：Hook/WS/HTTP 端口占用均正常非零退出 |
| A17 就绪/部分失败/断线 | PASS | http_api、uds_client、business_startup：本地就绪与引擎状态分开，最近同步统计及当前摄像头状态各自准确 |
| A18 WS 半帧与积压 | PASS | ws_limits：partial-send/EAGAIN、帧/字节超限、active 完整、其他连接继续使用 |
| A19 WS 输入边界 | PASS | ws_limits：握手、累计分片、单帧、连接数、控制帧和路径边界 |
| A20 SIGTERM 与重复 stop | PASS | business_startup、ws_limits、http_api：等 ACK/截图时正常退出，重复清理安全 |
| A21 旧执行权与快速重连 | PASS | camera_service：断线唤醒旧等待，旧 lease 失效；保持旧 lease 时新同步不能抢入，释放后自动重连，不调用 stop/start |

## 复现与结果

- 全新 Linux 容器、源码只读挂载：29/29 个 CTest 测试程序通过，测试部分用时 39.29 秒。
- 阶段烟测：10/10 个测试程序通过，用时 27.72 秒；硬件头文件边界检查通过。
- 前端：3 个测试文件、27/27 个用例通过。
- git diff --check 通过。

原始日志在 validation/2026-10-29/phase1-batch4/，包括 linux-build-and-ctest.txt、smoke-results.txt、web-test-results.txt。真板、真实 RTSP/ZLM/RKNN 为 SKIP；设备与算法包条件仍未确定。

```powershell
.\scripts\test-linux.ps1 -SkipBuild
cd web
npm test
```

```bash
bash scripts/smoke_business_phase1.sh /path/to/configured-build
```

下一阶段进入 V2 任务与规则的业务模型、迁移和编译流程，继续沿用本阶段的持久目标、单写者和操作查询。真件验证另行补充；人脸、通知、外设及截图任务持久化仍按完整产品计划后续推进。当前改动尚未提交或推送 GitHub。
