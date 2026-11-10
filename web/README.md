# web/ —— 静态 SPA（businessd 托管，零构建链）

2026-11-09：新增 `access.js` / `access.css`（设备登录与重登）、`connection_model.js`（HTTPS/WSS 和扫描参数），摄像头页支持选网卡与范围，任务页支持按需加载快照。当前前端测试 53/53，通过真实 REST 的浏览器检查见 `doc/validation/2026-11-09/remote-scan/`。部署时复制全部生产 `.js`、`.css`、`index.html` 和 `vendor/`，不能仅按下方旧文件清单拷贝。

2026-11-10：已补本机 Nginx HTTPS/WSS 的 7 组浏览器联调，证据见 `doc/validation/2026-11-10/proxy-browser/`。该验证使用自签证书、fake 推理和模拟媒体，不代表真实视频或公网已验收。

前端设计依据：《AIBox_前端设计_独立式_V1.0》（impl/ 下，页面/契约/对齐方案/工控深色视觉规范）。

## 文件结构

```
web/
├── index.html              # T5.2 SPA 壳：侧边导航 + 顶栏 + 内嵌工控深色 CSS（零构建）
├── app.js                  # 哈希路由 + 八页面（含 tasks）
├── task_page.js / task_model.js # V2 任务 CRUD、启停、时间计划、区域编辑与表单转换
├── api.js                  # T5.2 REST 封装：envelope 解包、§8.2 错误码→toast、Bearer 预留、8s 超时
├── ws.js                   # T5.2 WS 管理：/ws/events 常驻 + /ws/meta/{cam} 引用计数租借 + 1s→8s 重连
├── player.js               # T5.2 CamPlayer：ZLMRTCClient 封装（signaling 完整 URL 直喂 zlmsdpUrl）
├── ui.js                   # T5.2 toast/confirmModal/badge/三态视图
├── overlay.js / overlay_core.js  # T5.1 检测框叠加（纯函数核心 + DOM 薄壳）
├── tests/                  # Vitest：区域叠加、任务表单、REST 受理语义、模块导入
├── package.json / .gitignore     # 仅 dev 测试（vitest），node_modules 不入库
└── vendor/ZLMRTCClient.js  # WebRTC 播放内核（306KB，取自 ZLMediaKit www/）
```

## 测试

```bash
cd web
npm install        # 仅 dev 依赖 vitest
npm test           # 当前 33/33：overlay_core 21 + api 6 + task_model 4 + 模块导入 2
```

## 部署拷贝纪律（板上）

交付物 = `*.js + index.html + vendor/`（**零构建链，无 node_modules**）：

```bash
rsync -a --exclude node_modules --exclude package*.json --exclude tests \
      web/ board:/opt/aibox/web/
# businessd.conf 指向：{"static_dir": "/opt/aibox/web"}
# vendor/ZLMRTCClient.js 须与板上 ZLM 版本同源（板上老版 MediaServer 的 www/ 目录
# 有旧版则优先用板上的，避免信令字段漂移）
```

## 手工验收清单（T5.2 DoD，板上 businessd 托管后浏览器过一遍）

- [ ] 八路由切换正常，刷新保持当前页（#/grid 默认）
- [ ] 算法任务：新建草稿、多摄像头/算法/规则、区域绘制、时间窗口、启停、部署状态和执行详情
- [ ] 定时任务：窗口外等待、窗口内运行；停用确认后编辑；列表轮询不覆盖表单或预检结果
- [ ] 宫格 1/4/9 切换；子码流出画面（WebRTC）；在线徽标/时钟正常
- [ ] 检测框压在人身上（frame_w/frame_h → fitRect 对齐）；换网络无需校准
- [ ] 单击放大 → 宫格实例销毁只剩 1 路（单路有声）；返回 → 重建
- [ ] kill core-engine → 顶栏 engine 灯红 + WS 横幅；systemd 拉起后自动恢复（框重锚）
- [ ] 摄像头：添加（engine 失败时黄色 note 提示）、编辑、删除（级联确认）
- [ ] 摄像头：扫描网络 → 发现设备卡片（已入库的灰显"已添加"）→ 卡片"添加"弹窗
      输入用户名/密码 → 自动 ONVIF 取流入库；错密码 toast 2002 留在弹窗可重试
- [ ] 算法配置：保存热生效 + npu_est 显示；3001 拒绝 toast 不关表单
- [ ] 算法库：disabled 置灰；回滚确认；registry_ready 推送后整表刷新
- [ ] 事件：实时区头插 + toast 卡片点击跳转；历史筛选；快照路径/占位不破版
- [ ] 回放：segments 检索 + MP4 直链播放
- [ ] 设置：rss/disk 占位显示 "--"；degrade 事件顶栏变橙

## overlay 对齐方案速记（前端设计 §6.3）

- **锚定双模式**（`MetaOverlay` 构造参数 `mode`，仅在 `_videoEdgeSec` 一处分叉）：
  - `webrtc`（产品主模式）：`anchorFp = frame_pts - currentTime×1e6`（MediaStream 无播放缓冲，播放头≈实时边缘）
  - `mse`（demo 同款）：`anchorFp = frame_pts - buffered.end×1e6`（缓冲前沿≈实时；★忌用 currentTime）
- 锚定仅一次（`computeAnchor` 对已有锚原样返回）；player/WS 重连后 `reset()` 重锚
- 逐帧对齐：弹出所有 `frame_pts ≤ currentTime×1e6 + anchor` 取最新；track 超 1s 离场剔除；
  detQ 容量 64 丢最旧；`frame_w/frame_h`（D3 契约字段）为 fitRect 输入
