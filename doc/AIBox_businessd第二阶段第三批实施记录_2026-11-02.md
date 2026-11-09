# businessd 第二阶段第三批实施记录

日期：2026-11-02。范围：无时间计划的 V2 任务持久化启停、多摄像头恢复及验收。承接第二批的规则引擎与来源事件，本批完成从任务 REST API 到持久化操作、引擎确认和重启重放的链路。

## 已完成的行为

1. 新增 `PUT /api/tasks/{id}/enabled`，请求携带当前 `revision` 和布尔值 `enabled`。任务修改使用版本比较，旧版本请求被拒绝；同版本、同目标的重试不重复增加任务版本或创建操作。
2. 启停时在同一 SQLite 事务中修改任务目标、递增版本、保存所有关联摄像头的不可变目标快照和部署关联。任意摄像头保存失败，整个事务回滚；提交之后才允许网络下发。跨摄像头实际生效是逐台完成，接口会报告尚未确认的状态。
3. schema v8 新增 `task_deployments`，保存任务版本与摄像头操作/目标版本的关联。继续复用 `config_operations` 和 Reconciler 重试，不另建一套配置执行器。
4. 同摄像头、同模型的 V2 任务共享推理，模型采用最大 FPS 和最小阈值，各规则保留自身参数及来源。旧摄像头算法接口与 V2 任务合并；停用某一任务只移除它的规则。摄像头启停、地址修改和重放保留其余任务配置。
5. 启用前查询 V2 协议能力及引擎核分区，拒绝缺失、重复、重叠或越界的核编号；按分配核数和配置内存预算准入。未确认旧快照继续预留资源，避免部分下发时超卖。
6. businessd 重启或引擎重连时，优先从已保存的摄像头目标快照恢复完整 V2 配置。旧引擎不声明 V2 能力时保持未确认/降级状态，不静默转换成 V1。
7. 任务详情返回 `deployments`、每台摄像头目标/确认版本、操作 ID 和应用状态。支持 `draft`、`pending`、`running`、`disabled`、`suspended`、`partially_suspended` 等状态。运行中或停用尚未确认的任务不能编辑/删除。
8. 修复停用任务重新保存草稿仍沿用旧部署确认的问题。草稿保存事务解除旧部署关联，保留操作历史；新版本返回 `draft`、`applied=false`，避免将旧版本确认当成新草稿已应用。

## 代码位置和修改原因

| 文件 | 修改及原因 |
| --- | --- |
| `business/db.h/.cpp`、`db_tasks.cpp` | schema v8、任务启停原子事务、部署关联和编辑/删除保护，确保任务目标与全部摄像头操作一致保存。 |
| `business/config_compiler.h/.cpp` | 在摄像头快照中存储完整 V2 协议，合并 legacy 与 V2；恢复时不靠重新推测规则来源。 |
| `business/camera_service.h/.cpp` | 启停准入、资源预留、多摄像头提交和应用结果汇总，与摄像头写入共用串行准入及连接租约。 |
| `business/task_service.h/.cpp`、`http_api.cpp` | 启停 REST、运行状态、部署进度和版本检查，区分已受理与实际确认。 |
| `business/uds_client.h/.cpp`、`reconciler.cpp` | 重放完整 V2 快照，同步期间协商能力，避免同步线程等待自身 ACK 或覆盖 V2 配置。 |
| `tests/fake_engine.h`、`test_task_runtime.cpp` | 可控能力/失败注入；覆盖版本冲突、事务回滚、共享任务、资源准入、部分失败重试与旧引擎重连。 |
| `tests/test_engine_v2.cpp` | 真实 businessd 与 core-engine-fake 子进程，通过 REST 启停两任务/两摄像头，并验证来源事件和进程重启。 |
| `tests/test_business_recovery.cpp`、`tests/CMakeLists.txt` | 新增独立 `task_recovery` CTest，实际 SIGKILL businessd，验证多摄像头任务恢复。 |
| `scripts/smoke_business_phase2.sh` | 将任务启停与强杀恢复纳入阶段烟测，更新尚未交付范围。 |

本次继续开发重点补齐强杀恢复、草稿部署状态修复及验收记录；此前保留的本批启停实现一并完成回归。未提交或推送 GitHub。

## 验证

Docker Desktop 起初因 WSL `CreateInstance/E_UNEXPECTED` 无法启动。关闭 WSL 后重启 Docker Desktop，服务恢复；未重置 Docker 数据，现有容器、镜像和构建卷保留。

在 `aibox-dev` 容器中编译通过。完整 CTest **34/34 通过，用时 48.17 秒**；第二阶段烟测 **8/8 通过**，硬件头文件边界检查通过。

`task_recovery` 包含启用/停用各四个强杀位置：第一条 V2 命令执行前、执行后但 ACK 返回前、第二台摄像头命令执行前、执行后数据库确认被触发器拒绝。每个场景都验证：

- 崩溃前任务版本和两台摄像头目标均已提交，未确认状态没有冒充成功。
- 重启后原操作和不可变快照继续生效，目标版本不重复增长。
- 两台摄像头均恢复正确的目标任务规则，共享模型的另一任务继续保留。
- 媒体代理不因任务启停或恢复被删除。
- REST 最终返回 `running` 或 `disabled`，且 `applied=true`。

全量日志在 `validation/2026-11-02/phase2-batch3/ctest.log`，阶段烟测在同目录 `smoke-results.txt`。测试使用内存数据库或临时目录，未对开发 Demo 数据库注入故障。

开发 Demo 已恢复启动，`http://localhost:8080` 可访问；系统状态为 `ready=true`、`engine_state=ONLINE`、`engine_write_ready=true`，未完成操作和降级摄像头均为 0。`git diff --check` 通过。此次未修改前端功能，未重复运行前端测试。

复现命令（项目根目录 PowerShell）：

```powershell
.\scripts\dev.ps1 build
.\scripts\dev.ps1 test
docker exec aibox-dev bash scripts/smoke_business_phase2.sh /workspace/build-dev
```

## 接口使用与状态含义

草稿仍通过 `POST /api/tasks` 创建，`PUT /api/tasks/{id}` 修改；不能在草稿保存接口直接写 `enabled=true`。启停使用单独接口，例如：

```http
PUT /api/tasks/123/enabled
Content-Type: application/json

{"revision":1,"enabled":true}
```

成功受理后任务版本增加，下一次停用必须使用最新版本。查询 `GET /api/tasks/123` 获取 `revision`、`status`、`applied`、`deployments`；操作诊断沿用 `GET /api/operations/{operation_id}`。

`accepted=true` 表示目标事务已保存，不能因响应 `code=1003` 就认定保存失败：下发未在等待窗口完成时会同时返回已受理和未确认状态，应继续查询。`applied=true` 表示当前目标得到确认；目标为停用时返回 `disabled`，不代表任务正在运行。

`preview` 仍是只读编译与估算：`apply_supported=false` 表示该预检调用本身不下发，启用须走上述单独接口。断连或同步期间不能提交新的启停；已持久化操作由恢复流程继续处理。

## 边界与下一批

本批仅支持无时间计划的任务启用。有 `schedules` 的任务可保存及预检，运行启用会明确拒绝。下一批接入按时间边界切换、错过时间窗口的重启恢复及任务管理页面。

本批没有完成真实板卡/RKNN/MPP/RGA、真实摄像头或 ZLMediaKit 视频验收。fake 引擎与 MockZlm 验证控制链路和恢复语义；模型级能力声明、配置版本查询/ACK 的 E1 扩展，以及人脸底库、通知和硬件联动仍按完整产品计划推进。
