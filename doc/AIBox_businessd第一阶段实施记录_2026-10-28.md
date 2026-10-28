# businessd 第一阶段实施记录

日期：2026-10-28。对应第一阶段方案 P1-01～P1-03 的首批实现，并提前修复通道重建丢失算法、代理回滚误删问题。第一阶段尚未整体验收。

## 已写入的代码

| 范围 | 当前实现 |
| --- | --- |
| 测试基础 | FakeEngine 保存实际通道、完整算法配置和命令顺序；重复 add 返回 1003，缺失通道返回 1002；MockZlm 验证复用代理与新建代理的区别 |
| 数据库事务 | Txn 成员锁覆盖事务生命周期；检查 BEGIN/COMMIT；拒绝嵌套事务；失败回滚保留错误；四处事务调用点检查结果 |
| 数据库 v4 | 新增 camera_config_state、config_operations；旧 enabled=0 保持停用并标记 legacy_inactive；不改写 v1～v3 迁移 |
| 配置提交基础 | submit_camera_config 原子更新已有摄像头的目标、算法列表、版本和操作快照；按 expected_revision 拒绝并发旧版本提交；重启后保留待执行操作 |
| 统一算法编译 | HTTP 配置、HTTP 通道重建、UDS 同步共用 compile_algos；过滤停用算法；空集合显式下发；校验阈值、帧率、ROI 和现有规则类型 |
| UDS 恢复 | add_cam 返回 1003 后有界执行 remove→add→完整 set_algos；只接受 remove 命令的 0/1002；不把重复通道视为配置已一致 |
| 媒体代理 | ProxyResult.created 区分新建和复用；子流失败时只回滚本次新建的主流代理 |
| Linux 入口 | tools/docker/Dockerfile.test 和 scripts/test-linux.ps1；源码只读挂载，构建目录及测试临时文件保留在一次性容器内 |

配置状态和持久操作目前是持久层基础，HTTP 尚未调用 submit_camera_config，后台执行器尚未接入。现有 HTTP 成功路径继续沿用同步下发，不宣称已经具备持久重试和最终一致性。

## 基线与回归

环境：Ubuntu 24.04 x86_64，Docker Engine 29.9.2；GCC 13、CMake 3.28.3、jsoncpp 1.9.5、SQLite 3.45.1、cpp-httplib 0.14.3。未使用 RKNN、真实摄像头或板卡。

原始基线首先出现两处编译阻塞：AlgoPackage.h 缺少 cstdint；HTTP/Hook 异常回调只适配 exception&，发行版要求 exception_ptr。补齐头文件并加入两种签名兼容后，可进入测试。

原有 rule_engine 测试遗漏把第一次事件加入累计集合，然后在检查失败后继续访问空集合导致崩溃；已补回首事件并保护下标访问。规则引擎业务实现没有因此调整。

验证过程：

1. 仅应用上述构建兼容与测试夹具修正后，原有 24 个测试程序全部通过。
2. 将新增的实际状态断言用于旧业务代码，事务、UDS、HTTP 和 ZLM 四个回归测试程序均失败；复现事务串写/错误丢失、重启降级、非法配置接受、重建配置丢失及误删已有代理。
3. 本次业务改动后的 25 个测试程序全部通过，包含新增 config_compiler 测试和扩充的 DB/UDS/HTTP/ZLM 测试。

4. 直接执行 scripts/test-linux.ps1，从 Dockerfile 构建镜像，在源码只读挂载的全新一次性容器内完成编译及 25/25 测试，脚本退出码为 0；测试用时 15.60 秒。

日志归档在本目录下 validation/2026-10-28：baseline-tests.log、old-code-regressions.log、phase1-tests.log、fresh-container-tests.log。

## 复现命令

在项目根目录的 Windows PowerShell 执行：

```powershell
.\scripts\test-linux.ps1
# 首次镜像构建成功后，可复用镜像：
.\scripts\test-linux.ps1 -SkipBuild
```

测试镜像通过官方 Alpine 镜像的公共 CA 包引导 Ubuntu 最小镜像使用 HTTPS 包源，随后安装 Ubuntu 自己的 ca-certificates。全程保留证书验证，避免本机网络下 HTTP 包源反复返回 502。

## 下一批与边界

按原计划继续完成：

1. P1-01 补齐 ACK 已执行但丢失、延迟、明确断连的可控故障接缝；MockZlm 保存源地址和进程重置场景。
2. P1-03 补齐完整摄像头编译结果、物理模型/steady/triggered 预算和显式 ZLM 回环消费 URL。
3. P1-04 提取 CameraService，把 HTTP 接到目标提交；实现代理补偿、摄像头启停/删除墓碑和配置准入闸门。
4. P1-05 接入 Reconciler、操作状态转换、失败重试、revision 确认和 connection_epoch，完成与 SYNCING 的单执行器交接。
5. P1-06～P1-08 完成启动回滚、截图队列边界、状态接口、WS 背压、前端待生效展示和阶段烟测。

当前 HTTP 修改流地址仍使用原编排方式，缺少完整补偿；数据库持久操作也尚未用于恢复这些中途失败。此批修复不把这部分描述为已完成。
