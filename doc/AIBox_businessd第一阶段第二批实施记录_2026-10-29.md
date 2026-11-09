# businessd 第一阶段第二批实施记录

日期：2026-10-29。承接首批的事务和配置编译基础，接通配置持久执行与恢复。本批仍属于第一阶段 B0+B1，尚未进入 V2 任务、人脸、通知或外设实现。

## 当前流程

```text
HTTP → CameraService → 获取配置执行权 → 校验、编译、预算准入
                    → 短事务保存目标、算法、版本、操作快照
                    → 释放执行权 → 等待确认或返回已接受、待生效

ONLINE  → Reconciler 领取操作 → ZLM/UDS 编排 → 确认版本或持久重试
SYNCING → UdsClient 独占执行权 → 完整重放 → 失败转交持久操作
```

HTTP 不再直接编排摄像头的代理和引擎命令。数据库锁只覆盖本地事务，不跨越网络请求。

## 代码改动

| 文件 | 改动与目的 |
| --- | --- |
| business/camera_service.h/.cpp | 统一新增、编辑、启停、删除、算法配置；校验、预算和提交共享准入区间，提交后释放执行权再等结果 |
| business/reconciler.h/.cpp | 后台领取持久操作、退避重试、恢复遗留 running；代理补偿和定期恢复；完整 ACK 后确认版本 |
| business/resource_budget.h/.cpp | 从 HTTP 提取纯预算函数；steady/triggered 分开计算，triggered 按当前单 worker；模型内存去重，缺少资源测量时拒绝新增负载 |
| business/config_compiler.h/.cpp | 完整快照编码/解码；保留源 RTSP，引擎消费显式 ZLM 主机、端口及 cam{id} 代理；停用/删除目标编译为空算法集合 |
| business/db.h/.cpp | 追加 v5：完整确认快照、确认的主/子代理源地址、操作错误码；原子创建、领取、确认、重试、替代和 revision 检查 |
| business/uds_client.h/.cpp | 配置执行锁及 connection_epoch；断线先唤醒等待 ACK 的线程；新连接标记运行状态未知后重放；同步失败持久恢复 |
| business/http_api.h/.cpp | 接入 CameraService；新增操作查询；列表返回启停与应用状态；保留本地 ready 含义，追加引擎状态及待执行数量 |
| business/zlm_client.cpp | 检查异常响应包形态，避免 JSON 类型异常退出后台线程 |
| business/main.cpp | 生产装配；同步复用媒体恢复函数；截图线程退出守卫覆盖启动失败；截图队列限制为 128 条 |
| web/api.js、web/app.js | 保留错误响应 data；识别待生效配置、查询完成状态；停用摄像头不进入预览宫格 |

v1～v4 迁移未改写。内部快照和代理源地址可能包含 RTSP 凭据，操作查询不输出这些内容。目标以 revision 和完整快照标识，本批没有单独添加配置 hash。

## 业务行为

- 停用算法：下发剩余启用集合，空集合也执行清除；媒体代理保留。
- 停用摄像头：停止分析和代理，保留配置；重新启用时完整恢复。
- 删除摄像头：写 deleted_at 墓碑并停止资源；保留历史事件、录像索引和配置记录。
- 仅改名称：旧状态可靠时不重建代理或分析通道；存在未确认的旧操作时重新对账，避免错误跳过恢复。
- 修改源地址：只替换变化的代理；失败时尝试恢复旧源。补偿也失败则保留待恢复状态，不返回成功。
- ZLM 单独重启：ONLINE 执行器默认每 5 秒检查已确认的启用通道，重建缺失代理。
- 引擎断线：旧执行权失效，下一连接重新重放；旧结果不能确认新目标。
- 启动重放失败：已有目标转入待恢复；没有操作记录的旧摄像头生成恢复操作，无需再次重启 businessd。
- 旧 enabled=0 记录不自动启用，legacy_inactive 标记在后台恢复后保留，用户明确修改后进入正常操作状态。
- 未确认解除的旧需求继续占用预算；按较小的配置/实际 steady 核数准入；triggered 不随 steady 核数放大。

执行采用全局串行方式。正在执行某次配置时，其他写请求可能返回 busy，且不产生副作用；查询接口仍可使用。

## HTTP 约定

原摄像头路径及 code/msg/data 包装保留。code=0 表示完整应用并持久确认成功；目标已保存但尚未确认时，code=1003，data 中包含：

```json
{
  "id": 1,
  "accepted": true,
  "pending": true,
  "operation_id": "cfg-...",
  "target_revision": 2,
  "apply_status": "retry_wait"
}
```

accepted=false 表示此次配置未被接收。GET /api/operations/{operation_id} 返回状态、尝试次数、错误和时间，不返回内部快照。操作记录追踪目标版本的应用状态，同一目标在连接重放失败后可重新进入 retry_wait。

GET /api/system/info 新增 engine_state、engine_write_ready、pending_operations、sync_degraded_cams。sync_degraded_cams 是最近一次启动同步的统计；ready 继续表示本地 HTTP/WS/DB/Hook 已启动，不等同于引擎恢复。

## 验证

新增 test_camera_service、test_business_startup，扩充 DB、HTTP、配置编译及前端 API 测试，覆盖：

- 完整启停、过滤与清除算法、删除后历史保留、仅改名不重建。
- 已执行但 ACK 前断连；数据库重新打开后恢复目标；SYNCING 写请求无副作用。
- 并发准入不超售；未确认旧需求继续占预算；实际核分区限制准入。
- 换流失败补偿、ZLM 单独重启、异常响应格式、启动重放失败及旧数据恢复。
- 旧版本结果不能确认新版本；running 恢复；原子创建失败无孤立记录。
- WS/HTTP 端口占用时正常退出；无引擎时分别报告本地就绪与引擎就绪。
- 前端保留 accepted/pending/operation_id；busy 和最终失败仍走错误分支。

最终结果见 validation/2026-10-29。后端由全新容器以只读源码挂载运行；前端由本机 npm test 运行。这些结果覆盖 Linux x86、FakeEngine 和 MockZlm，不代表板卡、真实摄像头或 RKNN 验收。

最终后端 27/27 项测试通过，用时 20.52 秒；前端 3 个测试文件、26/26 个用例通过。记录分别为 ctest-results.txt 和 web-test-summary.txt。

```powershell
.\scripts\test-linux.ps1 -SkipBuild
cd web
npm ci --ignore-scripts --no-audit --no-fund
npm test
```

首次无测试镜像时，去掉 -SkipBuild。

## 剩余工作

第一阶段整体尚未完成。下一批优先处理 P1-07 的 WS 背压和输入边界，再补齐 P1-06 的退出/状态细节及 P1-08 验收矩阵。截图任务持久化、V2 任务、人脸、通知和真实硬件适配继续按后续阶段执行。
