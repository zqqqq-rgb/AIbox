# businessd 第二阶段第二批实施记录

日期：2026-10-29。范围：V2 引擎规则执行、来源事件闭环及自动化验证。第一批完成草稿、数据库任务模型和预检，本批将这些定义转换成明确的 V2 引擎契约。

## 本批完成内容

1. 引擎增加 `get_capabilities` 和独立的 `set_rules_v2` 命令。旧的 `set_algos` 保持原有结构。业务 UDS 客户端每次提交 V2 配置前协商规则版本、类型、独立阈值/采样及来源字段；旧引擎或声明不完整时返回不支持，不发送 V2 配置。
2. 同一摄像头中的模型各自跟踪；同一模型只推理一次，各条规则分别按自己的阈值、FPS、ROI、驻留时间和防抖状态判定。同名事件不会跨任务合并。
3. 每次全量配置更新生成新的运行代数。旧配置仍在推理队列中、乱序结果、删除摄像头后的结果均不能进入新配置；源 PTS 回退会重建时钟与状态并使在途旧结果失效。
4. 未变化的规则保留驻留、防抖和采样状态。删除同模型的另一条规则、调整共享模型的整体 FPS/阈值、仅更新任务 revision，不会让保留规则重新计时或重复报警。
5. V2 阈值在 YOLO 解码/NMS 前传入，避免低于 manifest 默认阈值的目标提前丢弃；跟踪器也使用 V2 模型阈值建立新轨迹。V1 继续使用原有默认行为。板端工厂已接线，实际 RKNN/MPP/RGA 构建与效果需要板卡验证。
6. 报警携带 `task_id`、`rule_id`、`task_revision`、`rule_version`。businessd 将来源与事件在一个事务中保存，HTTP `/api/events` 返回这些字段。旧事件返回 0。

## 代码与修改原因

| 文件 | 作用与原因 |
| --- | --- |
| `engine/ipc/Protocol.h/.cpp` | 定义规则数组、能力声明、来源事件与严格校验，避免旧引擎静默忽略新字段。最多 64 个模型、256 条规则，每条 ROI 最多 128 点；来源必须为正数且在 SQLite 有符号整数范围内。 |
| `engine/rules/RuleRuntime.h/.cpp` | 新增线程安全运行层，将原有单规则判定封装成独立实例，隔离模型跟踪与规则状态，并拒绝过期结果。 |
| `engine/main.cpp` | 装配新命令和运行层，推理回调携带模型与代数，发送带来源的报警。 |
| `engine/infer/AlgoInstance.h`、`NpuScheduler.h/.cpp` | 增加显式阈值后处理接口，0 也是有效 V2 阈值；按帧将阈值传入解码器。 |
| `engine/model/ModelRegistry.h/.cpp` | 注入动态阈值解码工厂，保留硬件依赖隔离。 |
| `engine/track/TrackerHub.h/.cpp` | 新轨迹置信度阈值可配置，避免 V2 低阈值目标永远无法进入状态规则。 |
| `business/task_compiler.h/.cpp` | 将聚合任务计划严格转换成 V2 协议，带上各规则来源和独立参数；编译错误时不生成半套配置。 |
| `business/task_service.cpp` | 预检增加 `wire_valid`、`required_rule_config_version`、`engine_capabilities_checked`，说明尚缺持久化启用链路。 |
| `business/uds_client.h/.cpp` | 增加能力查询，V2 配置提交前进行能力协商。 |
| `business/db.h/.cpp`、`main.cpp`、`http_api.cpp` | schema v7 新增 `event_rule_sources`；事件与来源原子写入，查询返回来源，历史记录不依赖当前任务是否存在。 |
| `engine/CMakeLists.txt`、`tests/CMakeLists.txt` | 新增 `core-engine-fake` 测试目标，直接编译同一引擎入口，在 x86 上跑真实进程装配。 |
| `tests/test_rule_runtime.cpp`、`tests/test_engine_v2.cpp` | 新增运行隔离、协议、来源事务及真正的引擎/businessd 子进程测试。 |
| `tests/test_tasks.cpp`、`tests/test_db.cpp` | 更新 schema 升级夹具并验证预检的协议转换结果。 |
| `scripts/smoke_business_phase2.sh` | 将运行层和端到端来源事件纳入第二阶段烟测。 |

迁移只追加 v7，没有修改 v1–v6 历史脚本。`event_rule_sources` 通过 event_id 关联事件，删除事件会清除来源；不关联可被编辑/删除的任务表，以保留历史来源。来源写入失败时，事件主记录也回滚。

## 验证范围与结果

`rule_runtime` 覆盖协议往返、缺失/重复/越界来源、非法规则类型/ROI/FPS/阈值、重复 JSON 字段、旧协议拒收规则数组；模型及规则隔离；不同 FPS 与阈值；同名事件独立报警；保留驻留状态；失效代数、乱序结果、源时钟重启；低置信度目标；显式阈值解码；来源事务失败回滚；聚合计划转成 V2 协议。

`engine_v2` 启动真实 `core-engine-fake` 与真实 `businessd` 子进程，使用临时算法包/NV12 文件及 MockZlm。四条规则共享一个模型，两条符合条件、另一条阈值过高、另一条 ROI 不包含目标：只有前两条分别报警。测试通过 UDS 协商/提交，再从 HTTP 和 SQLite 验证来源及版本，正常退出进程并重开数据库验证历史。另模拟不声明能力的旧引擎，确认不会收到 V2 配置。

最终全新 Linux 容器、源码只读挂载：32/32 个 CTest 测试程序通过，用时 41.28 秒；第二阶段烟测 6/6 通过，用时 4.15 秒；硬件头文件边界检查与 `git diff --check` 通过。构建无编译警告。原有摄像头配置、强杀恢复、HTTP/UDS 和 WS 回归仍通过。

原始输出保存在 `validation/2026-10-29/phase2-batch2/linux-build-and-ctest.txt` 和 `smoke-results.txt`。

```powershell
.\scripts\test-linux.ps1 -SkipBuild
```

```bash
bash scripts/smoke_business_phase2.sh /path/to/configured-build
```

## 当前边界与下一批

任务 REST API 仍只保存草稿，`enabled=true` 继续被拒绝。`apply_supported=false`，预检不会连接引擎或提交配置，`wire_valid=true` 只表示协议转换合法。集成测试通过专用 UDS 控制客户端提交配置，用于验证引擎执行和业务事件接收，不代表任务 REST 启用流程已经完成。

下一批按顺序接入：多摄像头目标配置与任务操作持久化 → 根据真实核分区做资源准入 → Reconciler 确认与重启重放 → 时间计划边界切换 → 任务页面及完整启停恢复验收。E1 的配置 revision ACK/查询、模型级能力声明也仍需完善，当前 `get_capabilities` 是规则协议能力声明。

本批未进行板卡、真实模型、真实摄像头或外设联调，没有运行用户生产数据库；新增进程测试使用临时目录。本批未修改前端源码，未重复运行前端测试。工作保留在本地，未提交或推送 GitHub。
