// T1.4 单测（任务分解 §4-T1.4）
// 1) 黄金表 4 组 —— s/pad 逐值相等
// 2) 正逆变换往返误差 —— s=1 组 <1e-6；非整倍率组（854→640）受 float32 参数存储
//    限制 <1e-3（亚像素，详设 §12.5 精度足够）
// 3) ROI 裁剪路径 —— 裁剪偏移还原正确
// 4) SoftLetterbox 与 compute_letterbox 黄金一致
#include <cmath>
#include <tuple>

#include "common/LetterboxMath.h"
#include "core/fakes/file_decoder.h"
#include "core/fakes/soft_letterbox.h"
#include "test_util.h"

using namespace aibox;

namespace {

void run() {
    // --- 黄金表（任务分解 T1.4 DoD） ---
    {
        LetterBoxParam p = compute_letterbox(640, 480, 640, 640);
        CHECK_NEAR(p.scale, 1.f, 1e-6f);
        CHECK_NEAR(p.pad_x, 0.f, 1e-6f);
        CHECK_NEAR(p.pad_y, 80.f, 1e-6f);

        p = compute_letterbox(640, 360, 640, 640);
        CHECK_NEAR(p.scale, 1.f, 1e-6f);
        CHECK_NEAR(p.pad_x, 0.f, 1e-6f);
        CHECK_NEAR(p.pad_y, 140.f, 1e-6f);

        p = compute_letterbox(854, 480, 640, 640);
        CHECK_NEAR(p.scale, 640.f / 854.f, 1e-6f);
        CHECK_NEAR(p.pad_x, (640.f - 854.f * (640.f / 854.f)) / 2.f, 1e-3f);
        CHECK_NEAR(p.pad_y, (640.f - 480.f * (640.f / 854.f)) / 2.f, 1e-3f);

        // 1080p 主码流子框 ROI → 模型（裁剪路径基准参数）
        p = compute_letterbox(640, 360, 640, 640);  // ROI 框 640×360
        CHECK_NEAR(p.pad_y, 140.f, 1e-6f);
    }

    // --- 往返误差 ---
    {
        // s=1 组：往返误差 = 0（<1e-6）
        for (auto [sw, sh, dh] : {std::tuple{640u, 480u, 640u}, {640u, 360u, 640u}}) {
            (void)dh;
            LetterBoxParam p = compute_letterbox(sw, sh, 640, 640);
            for (float x : {0.f, 100.f, 320.f, static_cast<float>(sw)}) {
                for (float y : {0.f, 50.f, static_cast<float>(sh)}) {
                    float mx = x * p.scale + p.pad_x, my = y * p.scale + p.pad_y;
                    float rx = mx, ry = my, rw = 50.f, rh = 30.f;
                    inv_letterbox(rx, ry, rw, rh, p);
                    CHECK(std::abs(rx - x) < 1e-6f);
                    CHECK(std::abs(ry - y) < 1e-6f);
                    CHECK(std::abs(rw - 50.f) < 1e-6f);
                    CHECK(std::abs(rh - 30.f) < 1e-6f);
                }
            }
        }

        // 非整倍率组：<1e-3（float32 参数存储的固有精度，亚像素级）
        // 原图框 (400,240,50,30) → 正向变换到模型坐标 → 逆变换还原
        LetterBoxParam p = compute_letterbox(854, 480, 640, 640);
        float bx = 400.f * p.scale + p.pad_x, by = 240.f * p.scale + p.pad_y;
        float bw = 50.f * p.scale, bh = 30.f * p.scale;  // 宽高经正向缩放
        inv_letterbox(bx, by, bw, bh, p);
        CHECK(std::abs(bx - 400.f) < 1e-3f);
        CHECK(std::abs(by - 240.f) < 1e-3f);
        CHECK(std::abs(bw - 50.f) < 1e-3f);
        CHECK(std::abs(bh - 30.f) < 1e-3f);
    }

    // --- ROI 裁剪路径：1080p 原图裁 (100,200,640,360) → 640×640 模型 ---
    {
        const float crop_x = 100.f, crop_y = 200.f;
        LetterBoxParam p = compute_letterbox(640, 360, 640, 640);
        // 原图目标框 (350,500,100,50) → 裁剪局部 (250,300,100,50) → 模型坐标
        float mx = 250.f * p.scale + p.pad_x, my = 300.f * p.scale + p.pad_y;
        float rx = mx, ry = my, rw = 100.f, rh = 50.f;
        inv_letterbox(rx, ry, rw, rh, p, crop_x, crop_y);
        CHECK_NEAR(rx, 100.f + 250.f, 1e-6f);
        CHECK_NEAR(ry, 200.f + 300.f, 1e-6f);
        CHECK_NEAR(rw, 100.f, 1e-6f);
        CHECK_NEAR(rh, 50.f, 1e-6f);
    }

    // --- SoftLetterbox 黄金一致（同输入下 param 逐值相等） ---
    {
        const uint32_t W = 64, H = 48;
        std::vector<uint8_t> nv12(FileDecoder::nv12_size(W, H), 0);
        Packet pkt;
        pkt.data = nv12.data();
        pkt.size = static_cast<uint32_t>(nv12.size());
        FileDecoder dec;
        CHECK(dec.open(W, H));
        Frame fr;
        CHECK(dec.decode(pkt, fr));

        SoftLetterbox lb;
        InferTensor t;
        t.width = 32;
        t.height = 32;
        LetterBoxParam param;
        CHECK(lb.run(fr, t, param));
        LetterBoxParam gold = compute_letterbox(W, H, 32, 32);
        CHECK_NEAR(param.scale, gold.scale, 1e-6f);
        CHECK_NEAR(param.pad_x, gold.pad_x, 1e-6f);
        CHECK_NEAR(param.pad_y, gold.pad_y, 1e-6f);
    }
}

}  // namespace

TEST_MAIN("test_letterbox_math")
