# businessd 第一阶段第三批实施记录

日期：2026-10-29。承接第二批的持久配置执行，本批完成 P1-07 的 WS 背压和输入边界，补齐 P1-06 的配置准入关闭、退出顺序与状态，并交付 P1-08 的自动化烟测入口和逐项验收差距。仍属于 B0+B1；未开始 V2、人脸、通知或外设适配。

## 代码与原因

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| business/ws_server.h/.cpp | 将 active_frame/offset 与 pending 队列分开；保留当前帧；同时限制帧数和编码字节数 | 原队列满时会淘汰已发出部分字节的帧，破坏后续 WS 帧解码 |
| business/ws_server.h/.cpp | meta 仅淘汰未开始的检测文本；超大检测丢弃；events 或控制帧无法容纳时关闭该连接并计数 | 检测可保留最新状态；事件不能静默丢弃后继续假装流完整。事件历史仍以数据库查询为准 |
| business/ws_server.h/.cpp | 握手响应也采用续传；限制握手、单帧、累计分片、连接数；检查掩码、RSV、控制帧、续帧和摄像头 ID | 防止半握手、累计分片和持续输入无限积压；握手超时按连接建立时间计算 |
| business/ws_server.h/.cpp | 每次连接读写最多 64 KiB，accept 最多 64 次；订阅回调移到锁外；退出逐组退订 | 减少单连接占用事件循环；允许回调重入查询/发送；避免清理时遗漏订阅 |
| business/main.cpp | 配置 ws_limits；WS 退出守卫；先关闭配置准入，再关闭 UDS 和 HTTP；截图入队/队列丢弃计数 | 启动失败时退订回调引用的 UDS 必须仍存活；退出期间不继续接收配置；事件保留与截图丢弃分开处理 |
| business/camera_service.h/.cpp、business/reconciler.h/.cpp | begin_shutdown/request_stop；准入与停止交接；写就绪查询；最终确认失败后等待再试 | 让退出先阻止新配置、唤醒等待，再回收线程；避免确认库不可用时高频重复应用 |
| business/http_api.h/.cpp | lifecycle_state；停止期间写接口返回 accepted=false；engine_write_ready 使用配置执行权可用性；输出 WS 统计 | 本地就绪、引擎在线和可接受写入需要分别表达；待重试数量不等同于写闸门关闭 |
| business/zlm_client.cpp/.h | 连接、发送、接收共享截止时间；限制响应为 16 MiB | 原逐次 poll 超时会被持续慢响应刷新，无法约束退出等待 |
| business/CMakeLists.txt | PUBLIC 链接 PkgConfig::CPP_HTTPLIB，继承完整编译选项 | Ubuntu 的 httplib 共享库使用 OpenSSL/zlib/brotli 宏；原先只链接库、丢弃宏，客户端对象布局不一致，会写坏相邻内存 |
| tests/test_ws_limits.cpp | 可编程 send 强制 partial-send/EAGAIN；字节/帧超限、正常连接隔离、输入边界、握手续传、重复 stop | 对真实 socket 字节流和资源计数作确定性断言，不依赖恰好填满内核缓冲 |
| tests/test_business_startup.cpp | Hook/WS/HTTP 端口冲突；真实 businessd 子进程等待 ACK/截图时 SIGTERM | 验证进程正常退出而非只验证某个类的析构 |
| tests/test_db.cpp、test_camera_service.cpp、test_http_api.cpp、test_zlm_client.cpp、mock_zlm.h | 注入最终确认失败、停止期拒写、忙状态、慢响应等 | 引擎执行成功但数据库确认失败不能对外报告成功；慢响应不能无限延长超时 |
| tests/CMakeLists.txt、scripts/smoke_business_phase1.sh | businessd_phase1 标签与一条命令烟测；无测试时失败 | 防止入口空跑却显示通过；明确模拟测试与真件验证的范围 |

httplib 的问题由新增截图进程测试在 Release 构建中复现。调试器观察到 httplib::Client 构造函数覆盖相邻 shared_future 的状态指针；pkg-config 的编译宏此前未传递。修正后重新完成独立容器构建和全量验证。本批最终结果替代原先缺少这些构建宏时的验证结论。

## 运行配置与可观测性

配置文件可选增加以下对象，不配置时使用这些默认值：

```json
{
  "ws_limits": {
    "max_tx_frames": 1024,
    "max_tx_bytes": 4194304,
    "max_frame_bytes": 1048576,
    "max_message_bytes": 1048576,
    "max_handshake_bytes": 8192,
    "max_connections": 128
  }
}
```

限制按每连接计算，字节预算包含完整的 active allocation，完成发送后释放；实际已发送的前缀不会提前释放预算。tx/handshake 字节下限 256；其他项须为正整数。配置加载同时限制 tx 为 64 MiB、单帧/消息为 16 MiB、握手为 64 KiB、连接数为 4096、队列帧数为 65536。

GET /api/system/info 增加 lifecycle_state=STARTING/RUNNING/STOPPING；ready 保留本地服务已就绪的含义。engine_write_ready 还要求配置服务运行、UDS 在线及配置执行权可获得。它是查询时的状态，不预留下一次写入的执行权。

生产装配返回 websocket 对象，包括 connections、buffered_bytes、buffered_frames、dropped_meta、slow_connections、rejected_connections、invalid_input。累计计数在进程重启后归零，缓冲统计包含握手响应。现有 total_dropped 继续表示检测消息丢弃数量；事件慢连接单独统计。

截图队列上限 128。未配置截图、队列已满、停止后入队及退出时尚未开始的任务都计入 discarded，事件数据库记录继续保留。正在执行的截图按原参数结束：首帧等待 3 秒加 2 秒 HTTP 余量。网络 I/O 使用共同截止时间；系统 DNS 解析及文件系统阻塞不在硬退出时限承诺内，当前进程测试使用本地 IPv4 和临时磁盘。

sync_degraded_cams 仍是最近一次同步统计，不代表实时的全通道健康数量。前端的重连历史补拉交互没有在本批重做。

## 验证结果

- 新容器、源码只读挂载：28/28 个 CTest 测试程序通过，测试部分用时 26.70 秒。
- 阶段烟测覆盖 9 个测试程序；包含真实 businessd 子进程以及 FakeEngine、MockZlm、SQLite、TCP WS 客户端。
- 截图退出场景：首个截图请求不响应，另有 130 条事件进入；131 条事件均保留，128 个排队任务和 2 个溢出任务共计 discarded=130，进程按超时正常退出。
- 数据库确认触发器故意失败时，目标已被引擎执行，但 applied_revision 保持旧值、操作不报告成功；解除故障后重试完成。
- 本批未修改前端代码，因此未重复运行前端测试；第二批的 26/26 用例结果是此前验证记录。

原始输出：validation/2026-10-29/phase1-batch3/linux-build-and-ctest.txt、smoke-results.txt。所有结果仅覆盖 Linux x86 与模拟件；真板、真实 RTSP/ZLM/RKNN 明确为 SKIP。

```powershell
.\scripts\test-linux.ps1 -SkipBuild
```

```bash
bash scripts/smoke_business_phase1.sh /path/to/configured-build
```

## 验收矩阵与剩余差距

PASS 表示表内具体自动化断言通过；PARTIAL 表示仍有原计划断言未覆盖，不能当作该项完整验收。以下同时核对前两批已有测试，未把所有测试算成本批新增。

| 编号 | 当前状态 | 证据或仍缺的验证 |
| --- | --- | --- |
| A01 | PASS | uds_client 重建后实际算法集合只含启用项；预算/禁用过滤另有 service/compiler 回归 |
| A02 | PASS | camera_service 清空算法实际下发空集合，通道和媒体保留 |
| A03 | PASS | http_api 换流完整重建；camera_service 启停后恢复；uds_client 重放 ROI 和规则 |
| A04 | PARTIAL | UdsClient 对象重建、引擎保留状态已通过；尚缺完整 businessd 进程重启并核对订阅的组合测试 |
| A05 | PARTIAL | set_algos 已执行、ACK 前断连已覆盖；add_cam 在 ACK 丢失时的独立故障点仍需补齐 |
| A06 | PARTIAL | duplicate 1003、missing 1002、重试和无副作用 busy 已覆盖；尚未穷举各命令阶段的错误分类 |
| A07 | PASS | zlm_client 已有主代理、子代理添加失败，不删除原主代理 |
| A08 | PARTIAL | 换流失败补偿成功、解除故障后恢复已覆盖；补偿本身也失败的明确断言仍缺 |
| A09 | PASS | camera_service 保持 UDS 在线，清空模拟 ZLM 后恢复代理 |
| A10 | PASS | db 原子提交失败无半条目标；确认失败回滚；service 不假报完成且可恢复 |
| A11 | PASS | db 第二线程隔离、嵌套 BEGIN 拒绝、延迟外键 COMMIT 失败、析构回滚 |
| A12 | PASS | camera_service 同时准入两路超预算请求，只接受一份；旧占用保留至确认停止 |
| A13 | 未覆盖 | 尚缺删除/停用在各执行步骤 SIGKILL、重启后清理和历史保留的完整故障矩阵；当前仅有生命周期、操作恢复及重开 DB 的分项测试 |
| A14 | PASS | camera_service SYNCING 写入拒绝，revision、代理和记录不变 |
| A15 | PASS | uds_client 重连仍完整重放，已有通道 duplicate 后保守重建 |
| A16 | PASS | business_startup 三类端口冲突均正常非零退出 |
| A17 | PARTIAL | 本地 ready、无引擎、忙与停止状态已有断言；sync_degraded_cams 的历史统计口径仍需在最终接口说明中冻结 |
| A18 | PASS | ws_limits 强制半帧/EAGAIN，帧数与字节超限，active 完整、计数释放、正常连接持续收取 |
| A19 | PASS | ws_limits 握手/单帧/累计分片/连接超限、非法路径、控制帧，正常连接继续使用 |
| A20 | PASS | business_startup 等 ACK/截图时 SIGTERM 正常退出；WS/API 的重复停止路径通过；适用本地 IPv4/磁盘条件 |
| A21 | PARTIAL | connection_epoch、旧等待唤醒和随后重放已覆盖；尚缺旧调用仍在收尾时自动立即重连的精确竞态测试 |

因此本批交付可用，但第一阶段整体不标记完成。下一批优先补齐 A13 的进程崩溃矩阵，再补 A04/A05/A08/A21 的组合故障点与最终错误/状态约定，之后再进入 V2 任务阶段。截图持久化和真实通知/硬件驱动仍属后续范围。

所有代码仍在本地工作区，尚未为本批创建提交或推送 GitHub。
