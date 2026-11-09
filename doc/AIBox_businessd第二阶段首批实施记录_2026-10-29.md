# businessd 第二阶段首批实施记录

后续进度：规则数组、独立运行状态和来源事件已在 [第二批实施记录](AIBox_businessd第二阶段第二批实施记录_2026-10-29.md)中实现。本文保留首批交付时的验证结果与边界。

日期：2026-10-29。承接 B0+B1，开始 B2「V2 算法任务与规则」。本批交付任务数据模型、legacy 适配、草稿 API、纯任务编译和预检；B2 整体尚未完成。

## 已交付行为

- 旧摄像头算法配置迁移为 legacy 任务，原 REST 接口、预算、配置操作和重连恢复继续使用。
- 新任务可离线保存、查询、按 revision 更新和删除；规则具有稳定 ID、key 和版本，支持多摄像头关联。
- 预检将同摄像头、同 algo_id 的推理需求合并，FPS 取最大、模型预过滤阈值取最小；各任务规则的阈值、FPS、ROI、task_id/rule_id 和版本单独保留。
- 周计划支持 UTC 和固定偏移时区、跨日窗口及周日到周一边界，可指定时间做预检。
- 新任务保存为 enabled=false 的草稿。启用请求返回 1003、accepted=false；当前 engine 尚未声明或实现规则数组、独立规则采样及来源 ID 的完整契约，本批不把草稿写入旧引擎协议。

编译结果是业务执行计划，不是可以直接发送给现有 engine 的 set_algos 报文。计划保留独立规则语义，不等于当前引擎已经执行这些规则。新任务暂未增加 Web 管理页面，现阶段通过 API 使用。

## 代码与数据变化

| 文件 | 内容与原因 |
| --- | --- |
| business/task_types.h | 任务、模型需求、规则、周计划和能力模板的类型定义 |
| business/db.cpp、db.h | 追加 schema v6；历史 v1～v5 不改写；旧算法配置写入适配为 legacy 任务 |
| business/db_tasks.cpp | 任务图事务保存、读取、软删除及 revision 检查；子记录失败回滚整个图 |
| business/task_compiler.h/.cpp | 无网络、无数据库的纯编译；模型需求共享，独立规则来源保留；时段判断与异常配置诊断 |
| business/task_service.h/.cpp | 严格校验、草稿生命周期、能力支持信息、预算预估、V2 启用闸门 |
| business/http_api.cpp | 新增任务和能力 API，沿用鉴权及停止期拒写；新任务 JSON 限制 2 MiB，拒绝重复键、尾随 JSON 和未知业务字段 |
| tests/test_tasks.cpp | 迁移、迁移失败回滚、legacy 唯一写入源、稳定 ID、事务、并发版本、共享模型、时段、重开数据库和损坏数据验证 |
| tests/test_db.cpp、test_http_api.cpp | 旧库测试适配只读视图；验证旧算法接口更新 legacy 源，新任务草稿不覆盖原运行配置 |
| business/CMakeLists.txt、tests/CMakeLists.txt | 注册新源文件和 tasks 测试，增加 businessd_phase2 标签 |
| scripts/smoke_business_phase2.sh | 第二阶段首批可重复验收入口，未完成运行链路输出 SKIP |

v6 新建 capabilities、algorithm_tasks、task_cameras、task_algorithms、task_rules、task_schedules。已有摄像头各生成一个 legacy 任务，原始阈值、帧率、启停、ROI、规则文本及显示配置保留；迁移不自行修复旧坏数据，也不自动启用旧停用设备。

camera_algos 从表变为只读视图，其数据来自 legacy 任务。Db::set_camera_algos 和原摄像头算法 API 在原事务内更新任务源；不存在另一份可独立写入的旧配置。直接 INSERT/UPDATE/DELETE 该视图会明确失败。旧版本程序不适合直接回写升级后的库；数据库回退需要兼容程序或升级前备份。

同一规则 key 保留时 rule_id 不变；规则正文或关联模型改变时 version 增加，只改任务名称不增加规则版本。删除一个 key 后再增加同名规则属于新规则。整个草稿删除使用墓碑，保留其定义；legacy 生命周期由摄像头控制，不能通过新任务 API 修改或删除。

## API 与使用样例

实际端口和 Authorization 沿用 businessd 配置。所有业务响应继续使用 code/msg/data 包装。

| 方法与路径 | 当前行为 |
| --- | --- |
| GET /api/capabilities | 11 种规则模板目录及支持状态；region_in/dwell 可验证草稿，其余为 unsupported；所有 V2 运行能力当前为 false |
| GET /api/tasks | 未删除任务，含 legacy；默认不列出已删除摄像头的 legacy 任务 |
| GET /api/tasks/{id} | 任务详情、revision、规则 ID/版本和计划 |
| POST /api/tasks | 创建草稿，enabled 缺省 false；新建 revision 缺省或为 0 |
| PUT /api/tasks/{id} | 按写入字段全量替换，必须携带当前正整数 revision；旧版本返回 1003 |
| DELETE /api/tasks/{id} | 请求体必须是 {"revision":当前版本}，只软删除 V2 草稿 |
| POST /api/tasks/preview | 指定摄像头、时间和是否纳入全部草稿，返回编译计划与估算，不提交配置 |

此处目录是规则模板，不是设计中的 103 项业务能力清单；业务能力到模型包、级联和标定条件的完整映射尚未导入。模板列出不代表存在可用模型。

创建草稿的请求体示例（camera_ids 和 algo_id 替换为实际记录；模型可用性由预检预算另外检查）：

```json
{
  "name": "入口驻留",
  "enabled": false,
  "camera_ids": [1],
  "algorithms": [
    {"algo_id": "person", "threshold": 0.6, "fps": 3, "enabled": true}
  ],
  "rules": [
    {
      "key": "entrance-dwell",
      "algo_id": "person",
      "capability_id": "dwell",
      "roi": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
      "params": {"dwell_sec": 12, "debounce_sec": 0},
      "enabled": true
    }
  ],
  "schedules": [
    {"timezone": "UTC+08:00", "weekdays": 31, "start_minute": 480, "end_minute": 1080}
  ]
}
```

POST /api/tasks 成功的 code=0 表示草稿已持久保存，data.saved=true、data.applied=false、data.status=draft，不代表检测已生效。这是草稿接口；原摄像头配置写入的 code=0 仍保持第一阶段「完整应用并确认」语义。

PUT 使用上述写入字段并补 revision，不回传 id/kind/status 等只读输出字段；规则身份通过 key 保持。每个任务最多 64 个摄像头、64 个算法、256 条规则和 64 个时段；ROI 最多 128 点。当前参数只开放 event_type、dwell_sec、debounce_sec；类别过滤、标定、级联等字段会拒绝，不会静默忽略。

weekdays 为位掩码：周一=1、周二=2、…、周日=64，多日相加。窗口开始包含、结束不包含；start_minute 大于 end_minute 表示跨日，凌晨部分归前一天的开始日。全天使用 0～1440；相同起止值无效。当前拒绝 IANA/DST 时区，例如 Asia/Shanghai 需显式写成固定 UTC+08:00；不将固定偏移描述为夏令时支持。

V2 debounce_sec=0 编译时继承当前默认 30 秒；本批没有新增可编辑的全局告警设置。legacy 缺省仍为原来的 5 秒，显式 0 也保持旧含义。

预检示例：

```json
{"camera_id": 1, "include_drafts": true, "at_ms": 1792972800000}
```

at_ms 可以省略，使用当前系统 UTC 时间。include_drafts=true 表示模拟全部尚未启用的 V2 草稿；false 只看当前 legacy/已启用定义。时段在预检时间求值，尚未启用后台计划切换执行器。

返回的 models 按 algo_id 聚合，rules 保留来源及各自参数。valid 只表示配置编译检查没有错误；budget.ok 是独立资源估算，二者都不等于可运行。返回值始终包括 applied=false、apply_supported=false、estimated_only=true 和引擎契约缺失原因。缺失、禁用或无成本测量的模型会使预算预估失败。

预算使用配置的 steady 核数/总内存，覆盖所有纳入模拟的任务及其有效摄像头，并保留旧未确认负载；同摄像头模型取最大 FPS，模型内存去重。没有实时核分区确认或写入预留，属于估算，不能替代下一批实际提交时的准入。高级模型依赖、不同任务规则采样及类别语义的真实执行仍待 E1/E2。

## 验证结果

- 最终全新 Linux 容器、源码只读挂载：30/30 个 CTest 测试程序通过，测试部分用时 39.94 秒。
- 第二阶段首批烟测：4/4 个测试程序通过，用时 2.45 秒；硬件头文件边界检查通过。
- 原有摄像头配置、ACK 丢失、自动重连、10 个强杀恢复场景及 WS 回归仍通过。
- 新增测试验证：两个任务在一个摄像头共享 person，FPS 合并为 7、阈值合并为 0.4，同时保留两套来源与 ROI；另一路摄像头的同任务负载也计入预算。
- 故意在 v6 迁移中途制造名称冲突后，原 camera_algos 表和数据恢复、新任务表不残留；解除故障后迁移成功。
- 新建/更新草稿子记录故障时，任务版本、名称、关联及规则全部回滚；并发更新仅一个版本成功。
- 数据库重开后草稿与规则 ID 保留；旧坏配置、坏时区和损坏规则参数在预检中显示错误。
- HTTP 验证草稿可离线保存；enabled=true 被拒绝；旧运行算法保留且不被草稿覆盖；停止/鉴权沿用同一 HTTP 包装。

原始输出在 validation/2026-10-29/phase2-batch1/：linux-build-and-ctest.txt、smoke-results.txt。git diff --check 通过。本批没有修改前端源码，未重复运行前端测试；此前的 27/27 是第一阶段历史结果。

```powershell
.\scripts\test-linux.ps1 -SkipBuild
```

```bash
bash scripts/smoke_business_phase2.sh /path/to/configured-build
```

烟测明确 SKIP：引擎规则数组、真实 task_id/rule_id 事件、后台时段应用、板卡与真实摄像头联调。用户当前生产数据库未由本轮测试启动或升级，数据库修改发生在测试的内存库/临时目录。

## 下一批顺序

1. 实现并协商 E1/E2：协议版本、规则数组能力、实际支持类型及来源 ID；旧 engine 保持拒绝 V2 启用的行为。
2. 扩展纯规则运行层和引擎装配：共享推理时阈值、采样、ROI、防抖按 task/rule 隔离；事件携带来源及版本。先验证 region_in/dwell，再逐项开放其他类型。
3. 将任务应用与多摄像头配置操作一起持久化；按实际核分区做准入，经现有 Reconciler 确认。接入时段边界调度与重启核对。
4. 补任务页面、来源事件存储和端到端测试：两个任务共享模型且独立报警，停用一个不影响另一个，重启/超时后仍一致。

当前是可测试的业务端基础交付，尚不能把 V2 新任务标为生产运行。代码已在本地工作区及 VS Code 中更新，未提交或推送 GitHub。
