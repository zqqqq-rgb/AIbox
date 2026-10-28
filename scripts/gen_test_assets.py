#!/usr/bin/env python3
"""T0.4 测试数据资产生成（纯标准库，离线可跑）

产出（默认写入 tests/assets/）：
  synthetic.nv12   裸 NV12 序列：Y=(x+y*W)%256 渐变 + 移动白块(Y=235)，UV=128
  presets.csv      FakeInfer 预设真值："pts_ms,frame_index,label,score,x,y,w,h"
                   最后一帧附一个越界框（右下角溢出，合成边界用例）

与 tests/test_fakes.cpp 的合成规则保持一致（白块 Y=235，可交叉验证）。

真实摄像头资产（任务分解 T0.4 第 1 项）需板端/ffmpeg 录制，见 tests/assets/README.md。

用法：
  python3 scripts/gen_test_assets.py                    # 默认 64x48, 30 帧, 40ms 帧间隔
  python3 scripts/gen_test_assets.py --w 640 --h 480 --frames 300 --square-step 4
"""
import argparse
import os

parser = argparse.ArgumentParser(description="生成 AIBox 合成测试资产")
parser.add_argument("--w", type=int, default=64, help="帧宽（偶数）")
parser.add_argument("--h", type=int, default=48, help="帧高（偶数）")
parser.add_argument("--frames", type=int, default=30)
parser.add_argument("--interval-ms", type=int, default=40)
parser.add_argument("--square", type=int, default=16, help="白块边长")
parser.add_argument("--square-step", type=int, default=2, help="白块每帧位移（右下方向）")
parser.add_argument("--out-dir", default=os.path.join(os.path.dirname(__file__), "..", "tests", "assets"))
args = parser.parse_args()

W, H = args.w, args.h
if W % 2 or H % 2:
    raise SystemExit("w/h 必须为偶数（NV12 要求）")


def make_frame(sq_x, sq_y, sq_size):
    y = bytearray(W * H)
    for r in range(H):
        for c in range(W):
            if sq_x <= c < sq_x + sq_size and sq_y <= r < sq_y + sq_size:
                y[r * W + c] = 235
            else:
                y[r * W + c] = (c + r * W) % 256
    uv = bytearray([128]) * (W * H // 2)
    return bytes(y) + bytes(uv)


def main():
    os.makedirs(args.out_dir, exist_ok=True)
    nv12_path = os.path.join(args.out_dir, "synthetic.nv12")
    csv_path = os.path.join(args.out_dir, "presets.csv")

    sq_x, sq_y = 0, 0
    with open(nv12_path, "wb") as f, open(csv_path, "w") as csv:
        csv.write("# pts_ms,frame_index,label,score,x,y,w,h\n")
        for i in range(args.frames):
            f.write(make_frame(sq_x, sq_y, args.square))
            csv.write(f"{i * args.interval_ms},{i},1,0.90,{sq_x},{sq_y},{args.square},{args.square}\n")
            # 边界用例：最后一帧附越界框（右下角溢出画面）
            if i == args.frames - 1:
                csv.write(f"{i * args.interval_ms},{i},2,0.80,{W - 4},{H - 4},16,16\n")
            sq_x = min(sq_x + args.square_step, W - args.square)
            sq_y = min(sq_y + args.square_step, H - args.square)

    size = os.path.getsize(nv12_path)
    print(f"OK: {nv12_path} ({args.frames} 帧, {size} 字节)")
    print(f"OK: {csv_path}")


if __name__ == "__main__":
    main()
