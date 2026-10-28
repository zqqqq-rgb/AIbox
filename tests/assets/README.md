# tests/assets/ —— 测试数据资产（任务分解 T0.4）

## 1. 合成资产（已入库，离线可再生）

```bash
python3 scripts/gen_test_assets.py            # 生成 synthetic.nv12 + presets.csv
python3 scripts/gen_test_assets.py --w 640 --h 480 --frames 300   # 更大规模
```

- `synthetic.nv12` —— 裸 NV12 序列：渐变背景 + 移动白块，规则与 `tests/test_fakes.cpp` 一致
- `presets.csv` —— FakeInfer 预设真值（白块位置框），最后一帧附越界框（边界用例）

## 2. 真实摄像头 H.264 裸流（60s，板端录制）

用于 M1/M2 的 RtspPuller/MppDecoder 真实链路测试。板端录制命令：

```bash
# 方式一：ffmpeg 从 RTSP 抓存为裸流（不改封装，纯 ES）
ffmpeg -rtsp_transport tcp -i rtsp://<camera>/... -t 60 -c copy -an test_cam.264

# 方式二：直接用摄像头厂商工具导出主码流 ES
```

录制后放本目录 `test_cam.264`（不入 git，体积大时放共享盘并在 M1 用例里引用路径）。
FileSource 走 ES 时 chunk_size 需按帧边界切分，M1 配合 FileDecoder 的 ES 直通版使用。
