# AIBox businessd 第一阶段执行方案 V1.0

创建日期：2026-10-26；更新日期：2026-10-28。对应总方案 B0+B1，名称为“基线建立与业务可靠性基础”。本阶段先解决已有摄像头/算法配置的正确性、恢复与可观测性；设备、算法包和外部对接条件尚未确定，不阻塞无硬件依赖的实现与测试。

本文件为阶段开发方案，已进入实施。首批改动及验证见 [第一阶段实施记录](AIBox_businessd第一阶段实施记录_2026-10-28.md)。阶段建议工作量为 6–9 个业务开发人日，真实板卡联调时间单列；首批测试通过不代表本阶段全部完成。

## 1. 阶段目标与边界

阶段结束应能证明：摄像头的新增、编辑、启停、删除和算法配置，经 businessd、ZLM、engine 后状态一致；出现超时、任一进程重启、配置中断时，能报告真实状态并按保存的目标恢复。

| 纳入本阶段 | 后续阶段 |
| --- | --- |
| 现有接口与测试基线；状态型 FakeEngine | V2 多任务/能力模板/规则数组 |
| CameraService 与统一配置编译 | 人脸注册、特征提取与比对 |
| 停用算法过滤、重建后完整重放、1003 恢复 | MQTT/Webhook 真实投递、通知路由 |
| 配置版本、持久操作、失败重试、媒体代理对账 | 外设真实驱动及联动执行 |
| 事务修复、并发预算串行化 | 告警/通知/动作的完整事务链与持久截图任务 |
| 启动回滚、退出、运行状态、WS 背压 | 新权限系统、算法包管理和全面运维界面 |

本阶段可以在 Linux x86 用 FakeEngine、MockZlm、SQLite 和临时目录验收；不把这些结果描述为已通过 RKNN、真实摄像头或真实硬件验收。

## 2. 统一业务语义

实施开始先冻结以下规则，避免 API、同步和重试各自理解：

1. 停用某算法：只停止该算法，摄像头预览、录像及其他算法继续。
2. 停用摄像头：停止该通道 AI 和其媒体代理，保留记录及算法配置；重新启用时完整恢复。该动作会停止该摄像头预览/录像，界面需明确提示。
3. 删除摄像头：停止运行资源，记录 deleted_at 墓碑；历史事件与录像索引保留。本阶段不物理清理历史业务数据。
4. 仅改名称：不重建流和 AI。改流地址只重建受影响的媒体流；子码流变化时恢复 AI 的完整算法配置。主码流地址变更可能产生受控录像中断，不承诺无缝切源。
5. 配置尚未应用和应用失败都必须能查询。保存了目标配置，不代表引擎已经生效；超时属于结果待确认，不能推断命令未执行。
6. 第一期只管理 SQLite 所属摄像头 ID，不删除来源不明的外部代理/通道。
7. SYNCING 期间只读可用，配置写入返回 busy 且不产生副作用；ONLINE 时允许提交配置操作。引擎断线后既有待执行操作保留，恢复后继续。

## 3. 处理结构与单写者约束

    HttpApi
       → CameraService：统一验证、准入、提交目标、等待/查询结果
       → ConfigCompiler：完整算法/规则与预算配置
       → Db：目标配置 + revision + config_operations

    SYNCING：UdsClient.do_sync 消费不可变 ReplaySnapshot
    ONLINE ：Reconciler 单线程领取待执行配置并调用 ZLM/UDS

    同一时刻仅允许一个配置编排执行器工作
    UDS 接收、ACK 分发和心跳仍由 UdsClient 自身线程处理

一期采用全局配置串行闸门，适合现有低频配置请求；不提前实现复杂通道并行和资源预留调度器。

- HttpApi 不再直接编排 remove/add/set_algos 或创建/删除 ZLM 代理。
- CameraService 不在 UDS 接收回调中阻塞等 ACK。
- 保留现有 do_sync，但改为消费统一编译后的快照；SYNCING 时 Reconciler 不发配置命令，HTTP 不改目标。
- ONLINE 后仅 Reconciler 执行运行期配置；CameraService 提交操作后等待其结果。预算检查和目标提交经同一配置闸门，两个请求不能同时读取旧预算通过准入。
- CameraService 在完成目标/操作提交后释放配置闸门，再等待操作结果；不能持有闸门等待需要同一闸门的 Reconciler，否则会死锁。执行器完成一次编排后再释放其执行权。
- 连接切换使用本地 connection_epoch 隔离旧操作，不改变 UDS 报文格式。断线先将旧 pending 请求全部失败唤醒、关闭旧执行准入；旧 Reconciler 在继续下一步前检查 epoch 并退出旧编排，释放执行权，新连接才能接管同步。UDS 接收线程不能阻塞等待一个正依赖它分发 ACK 的执行器；同步接管须通过可取消的交接或非阻塞获取实现。旧 epoch 的调用不能发送到新连接，旧结果也不能确认新连接状态。
- DB 锁只覆盖本地短事务，不覆盖 ZLM/UDS 网络等待。同步/操作结果按 target_revision 确认，旧版本 ACK 不得确认新版本。
- 预算以已接受目标加上未确认解除的旧生效需求保守计算；释放资源需有停止/替换确认，不能因为新目标已写入就假定旧负载消失。实现先全局串行并显式记录不确定状态。

## 4. 开发任务与交付顺序

### P1-01：建立可复现基线与故障测试支撑

改动：tests/fake_engine.h、tests/mock_zlm.h、tests/CMakeLists.txt；新增阶段测试脚本/报告。

交付：

- FakeEngine 保存摄像头集合、实际完整配置、命令顺序；重复 add 返回已有通道状态，remove 删除真实假状态，set_algos 全量替换。
- 可分别模拟“已执行但 ACK 丢失”“执行前拒绝”“延迟 ACK”“连接断开”。ACK 丢失通过可控传输接缝实现，不用 sleep 猜时序。
- 区分只断开业务连接、engine 重启清空内存，以及仅 businessd 重启而 engine 保留状态。
- MockZlm 区分新创建与已存在代理，保留代理源地址，支持某一步失败/超时与代理全部丢失。
- 记录基线测试结果、依赖版本、已有失败和已跳过项，复现下列 P1-02～P1-07 的具体问题。

完成判据：新增测试能在旧实现稳定复现关键问题；结果可以检查实际配置集合，而不只检查调用次数或 HTTP code。

### P1-02：先修复数据库事务并追加最小状态模型

改动：business/db.h、business/db.cpp、tests/test_db.cpp。

事务修复：

- Txn 持有成员 unique_lock<recursive_mutex>，从 BEGIN 到 COMMIT/ROLLBACK 覆盖事务生命周期。
- active_ 只在 BEGIN 成功后置位；commit 返回 bool；BEGIN 失败不得继续本事务写入，也不得析构时回滚其他事务。
- 明确拒绝未支持的嵌套事务；析构回滚保留原始失败原因。
- 检查 migrate、delete_camera_cascade、set_camera_algos、replace_algorithms 的事务开始与提交结果。业务写入通过持锁的 Db 方法执行，不在锁外跨线程使用裸 Stmt。

追加迁移，不改写 v1–v3。建议数据项：

| 位置 | 字段 |
| --- | --- |
| 摄像头配置状态 | desired_revision、applied_revision、apply_status、last_error、deleted_at、updated_at |
| config_operations | operation_id、camera_id、op_type、target_revision、target_config_json、previous_applied_json、status、attempts、next_retry_at、last_error、created_at、updated_at |

target_config_json 是该版本的完整目标快照，previous_applied_json 为恢复旧代理源等补偿保留依据；其中凭据不得通过普通查询/日志返回。

已有 enabled=0 记录无法自动区分旧版“停用”和“逻辑删除”：迁移后统一保持不启动，并标注 legacy_inactive；不猜测用户旧意图，不自动恢复或物理清除。

操作状态：pending → running → succeeded；临时失败 → retry_wait → running；不可恢复的参数/能力错误 → failed；未执行且已被新目标替代 → superseded。重启遗留 running 按结果未知转回待确认/重试。

applied_revision 只表示最后一次完整应用并成功确认的版本；连接断开后运行有效性单独标记 unknown。每次新 UDS 连接都必须重放，不能看到两个 revision 相等就跳过恢复。

完成判据：目标与操作记录同事务写入；失败无半条记录；第二线程不能混入事务；迁移重复执行安全，旧数据保留。

### P1-03：统一配置编译及资源准入

新增：business/config_compiler.h/.cpp、tests/test_config_compiler.cpp。

输入：摄像头目标、camera_algos、算法注册表、核预算及回环配置。输出：不可变 CompiledCameraConfig，包括完整 SetAlgos、消费 URL、目标版本、配置 hash 与预算使用项。

- 所有下发路径只包含 enabled=true 的算法；停用项可保留在目标配置中供编辑，但不运行、不计新目标预算。
- 统一阈值、帧率、ROI、rule_json 校验；非法 ROI 不再悄悄降为全屏；一期只接受现有 region_in/dwell 可执行规则。
- 空启用算法列表明确编译为 algos=[]，用于清除旧运行配置，不能跳过这次下发。
- HTTP 更新、通道重建、启动同步、后台重试全部复用同一输出。
- 按物理模型去重，steady/triggered 分组预算；核计划与 engine 指标/部署配置核对。triggered 当前只用一个 worker，不按配置的多个核放大预算。
- 源 RTSP 保留原地址；另生成 engine 使用的 ZLM 回环地址。实际 ZLM RTSP 端口成为显式配置，不写死。

完成判据：启用过滤、空配置清除、规则/ROI、预算四者一致；关闭算法后即使重连也不会重新执行。

### P1-04：提取 CameraService 与媒体编排

新增：business/camera_service.h/.cpp、tests/test_camera_service.cpp。
调整：business/http_api.h/.cpp、business/zlm_client.h/.cpp。

统一处理 create/update/enable/disable/delete/set_algos，流程为：

    进入配置闸门 → 检查生命周期/UDS 状态 → 校验并编译 → 预算检查
      → 短事务保存目标 + revision + 操作 → 提交 → 唤醒执行器
      → 在约定超时内等待结果，或返回可查询的待确认状态

媒体编排要求：

- ProxyResult 增加 created 标志，只回滚本次创建的代理；已存在的主代理不能因为子代理失败被删除。
- 修改源失败时，依据 previous_applied_json 尝试恢复旧源；恢复失败也必须记录，不假报补偿完成。
- 正常 AI 重建和 1003 恢复不删除 ZLM 代理；只有显式改源、停用或删除摄像头才处理相应媒体资源。
- 引擎重建始终包含完整 set_algos；名称修改不触发重建。
- 新记录处于 provisioning/pending 时只能由当前操作或明确的恢复快照处理，不被另一条同步路径当作已配置完成。

完成判据：摄像头管理和算法配置都从服务层进入；HTTP handler 中不再存在另一套跨 DB/ZLM/engine 编排逻辑。

### P1-05：统一同步与持久恢复

新增：business/reconciler.h/.cpp、tests/test_reconciler.cpp。
调整：business/uds_client.h/.cpp、business/main.cpp。

SYNCING：

1. 获得配置闸门，生成含 revision 的 ReplaySnapshot，区分启用、停用和删除墓碑。
2. 启用记录检查/恢复代理，添加 AI 通道并下发完整算法；停用/删除记录清理其 AI 与所管理媒体资源。
3. add_cam 返回 1003 时不直接当成功，保守执行 remove_cam → add_cam → set_algos；remove 的 0 或明确“通道不存在”1002 可继续，其他情况记录失败并重试。
4. 同步只确认该快照版本。部分通道失败记录 degraded，继续其他通道；进入 ONLINE 后后台针对失败操作重试。
5. 恢复检测订阅。该保守恢复会短暂重建 AI 通道；保留未变更的录像/预览代理，不承诺检测零中断。

ONLINE：

- 一个 Reconciler 执行待处理操作，串行完成媒体与引擎流程；相同摄像头/相同目标 hash 的重复请求复用未完成操作。
- set_algos ACK 丢失时可重发相同全量配置；add/remove 结果不确定时走保守重建，不根据超时推断未执行。
- 网络/断连临时错误退避重试；配置不存在、模型不可用等错误分类记录，避免无限高频重试。
- 低频核对期望代理与 ZLM 现状，ZLM 单独重启时可恢复，不能只依赖 UDS 断连触发。
- engine 完整应用后，事务确认 applied_revision 与 operation=succeeded；确认落库失败不返回已完成成功，恢复后继续核对。

完成判据：HTTP、SYNCING、后台恢复没有并发双写；删除/停用中途退出后不残留该业务管理的运行通道；不能仅因数据库版本一致就跳过新连接同步。

### P1-06：启动退出与状态兼容

调整：business/main.cpp、business/http_api.*；可提取 business/runtime_status.*、business/snapshot_worker.*。
新增：tests/test_business_runtime.cpp。

- 截图线程纳入 RAII，所有启动失败经统一清理路径，stop 可重复调用。
- 截图队列一期只做有界和退出策略：未开始任务丢弃并计数、正在执行的请求按既有超时结束；持久恢复留后续证据阶段，不在这里承诺。
- 退出先停止接收配置写入并标 stopping，再停止生产者/执行器和 UDS，最后释放数据库；等 ACK、截图和正在处理的 HTTP 都有有界停止机制。
- 保留 ready 的旧含义：DB、HTTP、WS、Hook 和本地线程已经启动。新增 engine_state、engine_write_ready、sync_degraded_cams、pending_operations 与 lifecycle_state。
- engine_write_ready 仅在 ONLINE 且本地配置服务可接受操作时为 true；它不代表每一台摄像头都健康，通道失败另列。

示例：引擎尚未启动时 ready=true，engine_state=disconnected，engine_write_ready=false；只读仍可用。避免改变 ready 语义导致 start_aibox/smoke 脚本误判本地服务启动失败。

完成判据：Hook/WS/HTTP 任一端口占用均非零正常退出而非 SIGABRT；SIGTERM 在空闲、等 ACK、截图中均可在配置退出时限内结束；重复 stop 不死锁。

### P1-07：修复 WS 慢消费者与边界

调整：business/ws_server.h/.cpp、tests/test_ws_hub.cpp。

- 分离 active_frame/offset 与尚未发送的 pending 队列，已开始发送帧不能淘汰。
- 同时限制待发帧数和字节数；meta 满时丢尚未开始的旧检测帧，events 慢连接超限则关闭并计数，正常客户端继续接收。
- 限制握手缓冲、单帧、分片累计和连接总量；上下限配置化，测试采用小容量确定性触发。
- 注入可编程发送函数，强制 partial-send → EAGAIN → 队列溢出，验证接收端仍可解码完整帧。

完成判据：不发生半帧字节被截断拼接；淘汰、发送、断连后字节计数正确，无单连接无限积压。

### P1-08：兼容接线、故障回归与交付

调整：business/CMakeLists.txt、tests/CMakeLists.txt、web/api.js 与必要状态提示；新增 scripts/smoke_business_phase1.sh。

- 既有 REST 路径和 code/msg/data 信封保留，新增 GET /api/operations/{id} 查询。
- code=0、data.ok=true 只在完整应用且最终状态持久确认后返回。SYNCING busy 返回非零且 accepted=false；已持久保存但等待/重试中返回非零、accepted=true、operation_id/apply_status/last_error，不伪装为已完成。
- 沿用 1003 表达“当前未完成/状态不允许”，以 data.accepted 和 apply_status 区分未接受与待确认；精确错误文本与结构在 P1-01 冻结。参数/预算拒绝仍使用既有对应错误码。
- ApiError 保留后端 data；页面遇到 accepted=true 时显示待应用、刷新操作状态，不能显示“保存成功”后静默丢掉失败。其余前端页面不在本阶段重做。
- GET 摄像头增加目标/已应用版本与状态；停用设备可以查询恢复，删除墓碑默认过滤。
- 新增阶段烟测只运行 businessd + 测试假件，不依赖仅板端可构建的 core-engine；现有 smoke_m4/m5 仍留作真件环境验证。

完成判据：阶段故障矩阵全部通过，旧成功路径回归通过；环境缺失项单独列 SKIP，不用全局退出码掩盖。

## 5. 必须通过的验收矩阵

| 编号 | 场景 | 核心断言 |
| --- | --- | --- |
| A01 | 停用一个算法后应用并重连 | 假引擎实际模型集合不含停用项；预算与下发一致 |
| A02 | 清空全部启用算法 | 发送 algos=[]；旧检测任务消失，媒体仍在 |
| A03 | 修改子码流或重新启用 | 帧率、阈值、ROI、规则全部恢复；不是只恢复通道 |
| A04 | 只重启 businessd，engine 保留通道 | 已有通道不永久 degraded；完整配置与订阅恢复 |
| A05 | add/set_algos 已执行但 ACK 丢失 | API 不假报完成；重试后实际配置与目标一致 |
| A06 | 任意 1003/1002 错误 | 根据命令和阶段分类，不全局吞掉错误当成功 |
| A07 | 已有主代理，创建子代理失败 | 已有主代理不被回滚删除 |
| A08 | 改源失败且补偿成功/失败 | 前者恢复旧源；后者如实暴露未恢复状态和重试依据 |
| A09 | ZLM 单独重启，UDS 持续连接 | 周期对账恢复所管理代理，不等 engine 重启 |
| A10 | DB 意图写入/最终确认失败 | 没有半条目标和操作；未确认操作不会显示已完成 |
| A11 | 事务内第二线程写入；BEGIN/COMMIT 失败 | 不混入其他写入；错误可见；不误回滚外层事务 |
| A12 | 并发两个配置请求合计超预算 | 只接受预算内组合，命令编排不互相穿插 |
| A13 | 删除/停用各步骤中杀 businessd | 重启按墓碑/目标完成清理；历史索引保留 |
| A14 | SYNCING 时提交写请求 | accepted=false，DB revision 不变，无网络写副作用 |
| A15 | 重连时数据库版本与历史应用版本相同 | 仍执行重放，不误以为新引擎已拥有配置 |
| A16 | HTTP/WS/Hook 端口占用 | 无 terminate/SIGABRT；资源和线程正常清理 |
| A17 | 引擎未起、同步、部分失败、断线 | ready 兼容，engine_write_ready 和通道状态准确 |
| A18 | WS partial-send + EAGAIN + 满队列 | active 帧完整；meta 只淘汰 pending；慢事件连接被关闭 |
| A19 | WS 分片/握手/连接数超限 | 资源受限，正常连接不受其他客户端拖累 |
| A20 | SIGTERM 时正等 ACK/截图；重复 stop | 按约定时限正常退出，无悬挂线程或死锁 |
| A21 | 旧操作等 ACK 时断线并立即重连 | 旧等待被唤醒，旧 epoch 不再发命令；同步接管无死锁，旧结果不确认新运行状态 |

确定性测试优先检查状态、命令序和已应用集合；只有需要性能/退出时限的用例才检查时间，避免依赖偶然调度。

## 6. 构建与验证入口

在 Linux/WSL2/Linux VM 中准备 jsoncpp、SQLite3、cpp-httplib 和 C++17/CMake 环境；本阶段不要求 MPP/RGA/RKNN。

    ./scripts/build.sh --tests
    ctest --test-dir build --output-on-failure
    ./scripts/check-headers.sh

为阶段用例加 businessd_phase1 标签后运行：

    ctest --test-dir build -L businessd_phase1 --output-on-failure
    ./scripts/smoke_business_phase1.sh

标签、阶段烟测和新增测试文件是本阶段待实现交付，不是当前已经存在并通过的命令。

真件条件具备后，再运行现有 build --hw --tests、smoke_m4/m5，并观察真实媒体代理、录像与检测恢复；单独记录实际经过时间与采样条件。

## 7. 提交与排期建议

| 开发顺序 | 任务 | 可独立评审的交付 |
| --- | --- | --- |
| 第 1 天 | P1-01 | 基线、复现用例、假件与接口/状态约定 |
| 第 2 天 | P1-02、P1-03 | 事务、迁移、统一编译与停用/空配置修复 |
| 第 3–4 天 | P1-04、P1-05 | 服务编排、持久操作、同步和后台恢复单写者 |
| 第 5–6 天 | P1-06、P1-07 | 生命周期、状态、WS 背压与对应回归 |
| 第 7 天及必要缓冲 | P1-08 | 接线、前端状态兼容、故障矩阵与报告 |

以上为约 6–9 个有效业务开发人日的顺序建议，初次环境部署或较多历史失败会增加投入。两个开发可并行做“配置控制面”和“事务/生命周期/WS”，但数据库与状态约定需先共同冻结。

建议按任务拆成 5–6 个可独立评审提交：测试支撑 → 事务与编译 → 配置服务与迁移 → 同步/恢复 → 生命周期/WS → 接线与验收。每个提交包含相关回归，不等所有模块合并后才第一次测试。

最终交付包：代码与追加迁移、兼容接口说明、状态/错误码说明、可重复故障测试、阶段烟测、基线与验收报告、剩余真件待验证清单。

本阶段完成标志：无需手动改数据库或重启全部进程，即可在已覆盖故障后恢复摄像头与算法配置；用户能查询未完成/失败原因，程序不把已接受、已发送或超时当作已生效。
