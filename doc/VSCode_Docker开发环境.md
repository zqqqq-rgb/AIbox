# VS Code + Docker 练习环境

2026-10-30 本机验证：`aibox-dev` 容器已创建，Debug 构建成功；C++ 测试 33/33、前端测试 27/27 通过；HTTP 创建草稿、启用、fake 报警落库和停用链路通过。GDB 已命中 `TaskService::save`。服务运行在 http://localhost:8080 ，引擎 ONLINE，待执行操作和同步降级摄像头均为 0。

VS Code 自动附加容器仍未建立连接，当前已验证可用的是**宿主 VS Code 的 Docker 任务**。下面提供的“附加容器”和容器内 F5 流程作为连接恢复后的使用方式；本次没有将远程编辑器连接记为已验证成功。

这套环境用于在 Windows 电脑上开发 Linux C++17 业务程序，无需板卡。包含 Ubuntu 24.04、GCC/G++、CMake、Ninja、GDB、JsonCpp、SQLite、cpp-httplib、Git、Python/pip/venv、Node/npm、FFmpeg 和 clang-format。前端测试依赖通过项目锁文件 `npm ci` 安装。

## 第一次准备

启动 Docker Desktop，确认使用 Linux containers。在项目根目录的 PowerShell 终端执行：

```powershell
.\scripts\dev.ps1 setup
.\scripts\dev.ps1 start
```

本次配置会实际创建 `aibox-dev:ubuntu24.04` 镜像和 `aibox-dev` 容器。后续可直接执行 `start`。如果镜像丢失，先重新执行 `setup`。

浏览器打开 http://localhost:8080 。这是静态前端与 REST 服务；8081 为 WebSocket，8090 为媒体 Hook。宿主端口仅绑定 127.0.0.1。

## 在 VS Code 中练习

宿主机 VS Code 也可以直接运行：在当前项目窗口按 Ctrl+Shift+B，或 F1 → Tasks: Run Task → **AIBox: Start Demo**。任务中的 Windows 配置会调用 Docker，实际编译与运行都发生在容器内。无需先成功附加容器。下面的容器窗口方式用于 Linux 智能提示与 F5 调试。

1. 按 F1，执行 **Dev Containers: Attach to Running Container...**。
2. 选择 **aibox-dev**，打开文件夹 **/workspace**。
3. 在容器窗口中安装提示的 C/C++、CMake Tools 扩展（扩展可能需要联网下载）。
4. Ctrl+Shift+B：执行默认任务 **AIBox: Build**。
5. F1 → Tasks: Run Task，选择 **AIBox: Test C++**、**AIBox: Test Web** 或 **AIBox: Start Demo**。
6. 在 `/workspace/business` 中修改代码，然后重新构建；重启 Demo 后新代码生效。

也可以在宿主项目窗口执行 **Dev Containers: Reopen in Container**，它使用 `.devcontainer/devcontainer.json`。该方式创建另一个容器，使用相同的开发数据卷；请先停止原 Demo，避免两个实例同时访问同一数据库或并行修改构建目录。日常推荐直接附加现有 `aibox-dev` 容器。

## 终端常用命令

Windows PowerShell：

```powershell
.\scripts\dev.ps1 build
.\scripts\dev.ps1 test
.\scripts\dev.ps1 web-test
.\scripts\dev.ps1 start
.\scripts\dev.ps1 status
.\scripts\dev.ps1 stop
.\scripts\dev.ps1 shell
```

VS Code 容器内终端：

```bash
cmake --build build-dev -j4
ctest --test-dir build-dev --output-on-failure
npm test --prefix web
python3 tools/dev/demo.py start
python3 tools/dev/smoke.py
python3 tools/dev/demo.py stop
```

只测试正在练习的草稿功能：

```bash
ctest --test-dir build-dev -R '^tasks$' --output-on-failure
```

## 断点调试

容器窗口 F5，选择 **AIBox: Debug task tests**，可在 `TaskService::save` 或 `Db::save_task` 设置断点。

调试业务进程：先执行 **AIBox: Stop Demo**，再 F5 选择 **AIBox: Debug businessd**。启动前任务会单独启动 fake 引擎与媒体模拟器，然后 GDB 启动业务进程。调试结束后执行 **AIBox: Stop Demo** 清理辅助进程。

启动失败时查看：

```bash
tail -n 60 build-dev/runtime/businessd.log
tail -n 60 build-dev/runtime/engine.log
tail -n 60 build-dev/runtime/supervisor.log
```

## 数据和模拟边界

- 源码由 `E:\aibox\aibox-main` 挂载到 `/workspace`，两端编辑的是同一份文件。
- `aibox-dev-build` 数据卷保存 Debug 构建、`build-dev/runtime/business.db`、日志和合成视频；停止或重启容器后保留。
- `aibox-dev-node-modules` 数据卷保存前端测试依赖，避免 Windows 和 Linux 的原生 npm 包混用。
- Demo 中的 `person` 模型是假件，只产生固定检测框；`model.rknn` 是占位文件，不能用于板卡。
- 媒体服务是控制接口模拟器，供摄像头配置、任务启停、报警落库练习使用。它不提供真实 RTSP/WebRTC 视频、截图或录像，网页的视频播放区域不会出现真实摄像头画面。
- 真实推理、视频播放、MPP/RGA/RKNN 性能及硬件联动，需要后续连接板卡、真实算法包和 ZLMediaKit 验证。
- Demo 采用独立数据库，不使用板端生产数据。测试也使用各自的内存库或临时目录。

## 新增配置文件

`.devcontainer/` 定义容器环境；`.vscode/tasks.json` 定义任务；`.vscode/launch.json` 定义调试入口；`tools/dev/` 提供初始化、进程管理和链路烟测；`scripts/dev.ps1` 提供 Windows 快捷命令。
