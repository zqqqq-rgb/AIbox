# AIBox RKNN 模型转换与部署手册 V1.0

> 文档日期：2026-09-23
> 适用环境：RK3588 板（topeet，Ubuntu 22.04）、rknn-toolkit2 2.3.2
> 上游文档：《AIBox 软件方案 · 独立式进程架构 V1.1》§11.3（模型转换规范）、《AIBox 板端ZLM部署手册 V1.0》（姊妹篇）
> 定位：**实操手册**——从 ONNX 到板上 NPU 推理跑通的完整步骤，含已踩坑记录。对应架构 §14 V2 阶段的模型准备任务。
> 状态：✅ yolov8n 已按本流程转换并在板上验证通过（NPU 推理环节已闭环）

---

## 目录

```
1  全流程总览（五阶段）
2  工具链关系图（谁在PC、谁在板上）
3  阶段一：环境准备与版本锁定
4  阶段二：获取 ONNX（关键：必须是 model zoo 兼容版）
5  阶段三：模型转换（export_rknn.py 使用）
6  阶段四：板端部署与推理验证（rknn_model_zoo demo）
7  已踩坑记录
8  产品化迁移清单（算法包化）
附录  参数速查表
```

---

## 1. 全流程总览（五阶段）

```
[PC/板]            [PC/板]              [板]                [板]                  [板]
阶段一         →   阶段二           →   阶段三          →   阶段四            →   阶段五
环境与版本锁定     获取ONNX            模型转换             板端部署+demo验证      (产品化)
                                     (量化+精度自检)       (编译zoo的C++ demo)    算法包化
```

**核心纪律（贯穿全流程）**：

> **rknn-toolkit2（转换工具）与 librknnrt.so（板上运行时）必须严格同版本（当前锁定 2.3.2）**——.rknn 模型格式与运行时版本强绑定，版本错位 = 板上加载必失败。这是全链路唯一"零容忍"的版本对齐（架构 §2 铁律），已登记版本锁定表。

## 2. 工具链关系图（谁在 PC、谁在板上）

```
┌─ rknn-toolkit2 (Python包) ──────────────┐    ┌─ librknnrt.so (C库) ──────┐
│  装在: PC(x86) 或 板(aarch64) 皆可       │    │  装在: 板上               │
│  职责: ONNX → .rknn 转换(量化/编译)      │    │  职责: 板上加载.rknn并推理 │
│  本项目当前: 板上直接转换(验证期做法)     │    │  版本: 必须 = toolkit版本  │
│  产品化: PC/CI 转换(可复现)              │    └──────────────────────────┘
└─────────────────────────────────────────┘
              │                    │
              ▼                    ▼
┌─ rknn_model_zoo ─────────────────────────────────────────┐
│  官方样例集: 各算法的转换脚本 + 板端C++ demo(含前后处理)    │
│  本项目角色: ①验证推理链路 ②core-engine 前后处理参考代码   │
└──────────────────────────────────────────────────────────┘
```

三句话记忆：**toolkit 是编译器（ONNX→rknn），librknnrt 是运行时（板上跑），model_zoo 是官方样例集（教你跑对）**。

---

## 3. 阶段一：环境准备与版本锁定

### 3.1 板上运行时版本确认（第一件事！）

```bash
# 板上执行 —— 此版本决定 PC 端 toolkit 该装哪版
strings /usr/lib/librknnrt.so | grep -m1 -i "version"
cat /sys/kernel/debug/rknpu/version    # NPU 驱动版本(需≥0.9.2, 架构§14-V0)
```

### 3.2 toolkit 安装（板上验证期 / PC 产品化两形态）

```bash
# 形态A: 板上直接装(python3.8对应cp38 wheel)
pip3 install rknn_toolkit2-2.3.2-cp38-*.whl
python3 -c "from rknn.api import RKNN; print('OK')"   # 验证导入

# 形态B: PC/VM 上装(产品化, 配 venv)
python3 -m venv rknn-env && source rknn-env/bin/activate
pip install rknn_toolkit2-2.3.2-*.whl
```

### 3.3 版本锁定登记（架构 §15.2）

| 组件 | 本项目锁定值 | 校验命令 |
|---|---|---|
| rknn-toolkit2 | 2.3.2 | 日志首行 `I rknn-toolkit2 version` |
| librknnrt.so（板） | 2.3.2 配套 | `strings /usr/lib/librknnrt.so \| grep version` |
| rknn_model_zoo | 2.3.2 tag | `git tag` |
| NPU 驱动 | ≥0.9.2 | `cat /sys/kernel/debug/rknpu/version` |

---

## 4. 阶段二：获取 ONNX（关键岔路口）

### 4.1 ★最容易踩的坑：ONNX 必须是 model zoo 兼容版

zoo 的 C++ demo 后处理期望 **9 个输出**（三尺度 reg/cls/sum 分支），不同来源的 ONNX 输出数不同：

| ONNX 来源 | 输出数 | 能否配 zoo demo | 用途 |
|---|---|---|---|
| zoo 的 `download_model.sh` 下载 | **9** | ✅ | 官方预训练权重（本项目当前用） |
| `yolo export format=onnx` 原版 | 1 | ❌ 后处理对不上 | 不可直接用于 zoo demo |
| `yolo export format=rknn` | RKNN 优化分支 | ✅ | **自训练模型必走此路**（架构§11.3） |

**检查方法**（转换前必做）：

```bash
python3 -c "
import onnx
m = onnx.load('yolov8n.onnx')
print('outputs:', len(m.graph.output))
for o in m.graph.output: print(' ', o.name)
"
# 9 个输出 = zoo 版可用; 1 个输出 = ultralytics 原版, 换源或用 format=rknn 重导
```

### 4.2 获取命令

```bash
cd /root/aibox/rknn_model_zoo/examples/yolov8/model
bash download_model.sh    # 下载 zoo 版 yolov8n.onnx (网络慢: ghproxy.com 前缀代理)
```

---

## 5. 阶段三：模型转换（export_rknn.py）

### 5.1 转换脚本

使用项目内脚本：`thirdparty_component/export_rknn.py`（Windows 侧）/ 板上对应副本。核心流程四步，**顺序不可变**：

```python
r = RKNN()
r.config(mean_values=[[0,0,0]], std_values=[[255,255,255]],   # ① YOLO归一化/255
         target_platform='rk3588')                            #    ★量产重转 rk3576
r.load_onnx('yolov8n.onnx')                                  # ② 加载(9输出版)
r.build(do_quantization=True, dataset='./calib.txt')         # ③ INT8量化(校准集)
r.export_rknn('yolov8n.rknn')                                # ④ 导出
# 可选: r.accuracy_analysis(inputs=[...]) 量化精度自检(相似度>0.98优)
```

**API 调用铁律**：`load_onnx` 前必须先 `config()`（顺序错了报 "Please call rknn.config first!"——已踩坑，见 §7-1）。

### 5.2 量化校准集准备（决定你现场精度的关键一步）

校准集 = 几十张**你现场场景**的图（架构 §11.3：含逆光/夜视/雨雾更佳）。用摄像头抓图：

```bash
mkdir -p calib
for i in $(seq 1 30); do
  ffmpeg -rtsp_transport tcp -i "rtsp://admin:pwd@192.168.1.64:554/Streaming/Channels/102" \
    -frames:v 1 -vf "scale=640:640" calib/$i.jpg 2>/dev/null
  sleep 2    # 隔2秒一张, 期间让人走动/变换距离, 覆盖典型目标形态
done
ls calib/*.jpg | sed 's/^/.\//' > calib.txt
```

**注意**：calib.txt 内图片路径**相对脚本运行目录**——换目录跑脚本会找不到图（build 报 dataset image not found）。

### 5.3 运行与产物

```bash
python3 export_rknn.py
# 板上转换注意内存: build 阶段占 2~4GB, 先停 MediaServer, free -h 确认可用>2GB
```

产物 `yolov8n.rknn`（约 6~8MB）+ accuracy_analysis 报告（./snapshot，关注 overall similarity >0.98）。

---

## 6. 阶段四：板端部署与推理验证

### 6.1 编译 rknn_model_zoo 的 C++ demo

```bash
cd /root/aibox/rknn_model_zoo

# ★若脚本从 Windows 中转过, 先治 CRLF(见 §7-2):
find . -name "*.sh" -exec sed -i 's/\r$//' {} +
find . -name "CMakeLists.txt" -exec sed -i 's/\r$//' {} +
find . -name "*.cmake" -exec sed -i 's/\r$//' {} +

# 编译: -t 目标芯片 -a CPU架构 -d 示例名
bash build-linux.sh -t rk3588 -a aarch64 -d yolov8
# 产物: install/rk3588_linux_aarch64/rknn_yolov8_demo/
#   ├── rknn_yolov8_demo   (可执行: RGA预处理+rknn_run+decode/NMS后处理 全都有)
#   ├── model/             (模型+标签)
#   └── lib/               (配套运行时库, 随行部署=版本自洽)
```

### 6.2 运行验证（用你转换的模型 + 摄像头抓图）

```bash
cd /root/aibox/rknn_model_zoo/install/rk3588_linux_aarch64/rknn_yolov8_demo

# ① 库路径(相对路径陷阱: 必须在本目录执行, export 只对本终端有效)
export LD_LIBRARY_PATH=./lib

# ② 换上自己转的模型
cp /root/aibox-test/yolov8n.rknn model/
cp /root/aibox-test/coco_80_labels_list.txt model/ 2>/dev/null

# ③ 用摄像头抓图跑推理
./rknn_yolov8_demo model/yolov8n.rknn /root/aibox-test/frame0.jpg
```

### 6.3 验收判据（✅ 本项目已通过）

| # | 判据 | 实测结果记录 |
|---|---|---|
| 1 | 输出检测框，类别/位置与画面肉眼看吻合（有人=person 框压人） | ✅ |
| 2 | demo 打印单帧推理耗时 → **登记为 manifest 的 npu_cost_ms 初值** | ______ ms |
| 3 | 推理瞬间 `/sys/kernel/debug/rknpu/load` 负载跳动 | ✅ |
| 4 | accuracy_analysis 相似度 >0.98 | ______ |

> 后续按架构 §11.3：用 loop100 取中位数重测耗时，正式写入算法包 manifest 的 npu_cost_ms（T3.5 npu_bench 任务）。

---

## 7. 已踩坑记录（按时间序）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | `load_onnx: Please call rknn.config first!` | API 调用顺序错：load_onnx 前没调 config | **config() → load_onnx() → build() → export_rknn()** 顺序铁律 |
| 2 | `build-linux.sh: line 2: $'\r': command not found` | 脚本从 Windows 中转带 CRLF 行尾，bash 把 `\r` 当命令 | `find . -name "*.sh" -exec sed -i 's/\r$//' {} +`（CMakeLists/.cmake 一起治）；**纪律：Windows 过来的文本文件到 Linux 先过 sed** |
| 3 | demo 报 `error while loading shared libraries: librknnrt.so` | `export LD_LIBRARY_PATH=./lib` 未生效：换目录/新终端/不在 demo 目录 | 三个动作（cd 到 demo 目录、export、运行）必须同一终端连续执行；或用单次写法 `LD_LIBRARY_PATH=./lib ./rknn_yolov8_demo ...` |
| 4 | （预防性）转换 build 失败 dataset image not found | calib.txt 内路径相对运行目录而非文件所在目录 | 保持脚本与 calib 同目录执行，或 calib.txt 内写绝对路径 |
| 5 | （预防性）板上 build 阶段 OOM | 板上转换吃 2~4GB 内存 | 转换前停 MediaServer 等大进程，`free -h` 确认 |
| 6 | （预防性）自训模型 demo 后处理对不上 | 用了 ultralytics 原版 onnx（1 输出） | 自训模型用 `yolo export format=rknn` 导出（§4.1 表） |

---

## 8. 产品化迁移清单（当前验证 → 算法包化）

| # | 事项 | 对应架构 |
|---|---|---|
| 1 | 转换迁到 PC/CI（venv + 固定 wheel + docker），产出可追溯 | §15.2 版本锁定 |
| 2 | npu_cost_ms 用 npu_bench 工具 loop100 中位数实测（替代 demo 单次值） | §11.3 / 任务分解 T3.5 |
| 3 | npu_mem_kb 实测入库（`rknn_query(RKNN_QUERY_MEM_SIZE)`） | §11.4.3 |
| 4 | 模型打包为算法包目录（manifest.yaml + labels + postproc.yaml + selftest） | §11.4.1 |
| 5 | 校准集按现场场景扩充（逆光/夜视/雨雾各采样）并归档 | §11.3 |
| 6 | RK3576 量产模型重转换（target_platform='rk3576'）+ 精度回归 | §15.1 |
| 7 | demo 前后处理代码为 core-engine `infer/algos/` 的参考实现来源 | 详设 §3.6 |

---

## 附录：参数速查表

| 参数 | 值 | 依据 |
|---|---|---|
| mean_values | `[[0,0,0]]` | YOLO 归一化 (x-0)/255 → 0~1 |
| std_values | `[[255,255,255]]` | 同上 |
| target_platform（验证期） | `rk3588` | 架构 §11.3 |
| target_platform（量产） | `rk3576`（不可与 rk3588 混用） | 架构 §11.3 |
| do_quantization | True（INT8 非对称） | 架构 §11.3 |
| 校准集规模 | 30~100 张现场图 | 架构 §11.3 |
| 输入尺寸 | 640×640 | 模型定死 |
| build-linux.sh 参数 | `-t rk3588 -a aarch64 -d yolov8` | 芯片/架构/示例名 三独立参数 |
| onnx 输出数检查 | 9 = zoo 兼容版 | demo 后处理配套 |

---

*（完）本文档与《板端ZLM部署手册》构成 AIBox 硬件验证期的两份实操基线。踩坑记录 §7 部署新环境前必读。*
