// YOLOv8 后处理单测（任务分解 §4-T3.3/T3.4，x86 纯逻辑）
// 重点回归 demo K7 布局坑：reg 是 NCHW [64,h,w]，bin 是独立通道（stride=hw）——
// 若实现按 stride=1 连续读会把相邻像素当 bin，框坐标全错（本测试用单热点栅格
// 精确断言坐标，足以区分两种读法）
// 注：[T#] stderr 进度标记用于 SegFault 定位崩溃块（M3 验证通过后可移除）
#include <cmath>
#include <cstdio>
#include <vector>

#include "infer/Yolov8Post.h"
#include "test_util.h"

using namespace aibox;

namespace {

// 合成 zoo 9 输出：3 尺度（80/40/20 栅格 @ 640 输入），每尺度 reg/cls/sum 三张量
struct Synth {
    int dims[9][2];
    std::vector<std::vector<float>> outs;

    Synth(int c, int h0, int w0, int h1, int w1, int h2, int w2) : outs(9) {
        int hs[3] = {h0, h1, h2}, ws[3] = {w0, w1, w2};
        for (int s = 0; s < 3; ++s) {
            size_t hw = static_cast<size_t>(hs[s]) * ws[s];
            dims[s * 3 + 0][0] = hs[s];
            dims[s * 3 + 0][1] = ws[s];
            dims[s * 3 + 1][0] = hs[s];
            dims[s * 3 + 1][1] = ws[s];
            dims[s * 3 + 2][0] = hs[s];
            dims[s * 3 + 2][1] = ws[s];
            outs[s * 3 + 0].assign(64 * hw, 0.f);  // reg
            outs[s * 3 + 1].assign(static_cast<size_t>(c) * hw, 0.f);  // cls
            outs[s * 3 + 2].assign(hw, 0.f);  // sum
        }
    }

    // 在 (gx,gy) 单元放一个"理想"目标：DFL 单 bin 热点（期望值=bin 序号）
    void place(int s, int gx, int gy, int l, int t, int r, int b, int cls,
               float conf) {
        const int w = dims[s * 3][1];
        const size_t hw = static_cast<size_t>(dims[s * 3][0]) * w;
        const size_t off = static_cast<size_t>(gy) * w + gx;
        const int edges[4] = {l, t, r, b};
        for (int e = 0; e < 4; ++e)
            // 热点须够大：softmax 期望残留偏移 ~exp(-v)*Σbin，v=10 时 ~0.004bin×stride8
            // = 0.03px，会击穿 0.01 容差；v=100 时 exp(-100) 下溢，期望精确等于 bin 序号
            outs[s * 3 + 0][static_cast<size_t>(e * 16 + edges[e]) * hw + off] = 100.f;
        outs[s * 3 + 1][static_cast<size_t>(cls) * hw + off] = conf;
        outs[s * 3 + 2][off] = 1.f;
    }
};

YoloPostConfig test_cfg() {
    YoloPostConfig c;
    c.num_cls = 1;
    c.input_w = 640;
    c.input_h = 640;
    c.conf_th = 0.5f;
    c.nms_iou = 0.45f;
    return c;
}

}  // namespace

static void run() {
    // ---- 1. DFL 解码 + 栅格几何（scale0 stride=8 单目标）----
    {
        std::fprintf(stderr, "[T1]\n");
        Synth S(1, 80, 80, 40, 40, 20, 20);
        // 期望 bin: l=2,t=3,r=4,b=5 → cx=(10.5)*8=84, cy=(20.5)*8=164
        // x = 84-2*8=68, y = 164-3*8=140, w=(2+4)*8=48, h=(3+5)*8=64
        S.place(0, 10, 20, 2, 3, 4, 5, 0, 0.9f);
        const float* in[9];
        for (int i = 0; i < 9; ++i) in[i] = S.outs[i].data();
        auto boxes = yolov8_decode(in, S.dims, test_cfg());

        CHECK_MSG(boxes.size() == 1, "期望 1 框, 实得 %zu", boxes.size());
        if (boxes.size() == 1) {
            CHECK_NEAR(boxes[0].x, 68.f, 0.01f);
            CHECK_NEAR(boxes[0].y, 140.f, 0.01f);
            CHECK_NEAR(boxes[0].w, 48.f, 0.01f);
            CHECK_NEAR(boxes[0].h, 64.f, 0.01f);
            CHECK_NEAR(boxes[0].conf, 0.9f, 0.001f);
            CHECK(boxes[0].cls == 0);
        }
    }

    // ---- 2. 多尺度 + conf 粗筛（sum 分支过半阈值才 decode）----
    {
        std::fprintf(stderr, "[T2]\n");
        Synth S(2, 80, 80, 40, 40, 20, 20);
        S.place(0, 5, 5, 1, 1, 1, 1, 0, 0.8f);   // 保留
        S.place(1, 10, 10, 2, 2, 2, 2, 1, 0.6f);  // 保留（cls=1）
        S.place(2, 3, 3, 1, 1, 1, 1, 1, 0.2f);    // conf 0.2 < 0.5 → 滤除
        // sum 弱目标：cls 强但 sum=0.2 → 粗筛即跳过（0.2 < 0.5*0.5）；
        // 若无粗筛，最终分也 = cls 0.9 ≥ 0.5（sum 不参与最终分数，zoo 口径）
        {
            const int w = S.dims[6][1];
            const size_t hw2 = static_cast<size_t>(S.dims[6][0]) * w;
            const size_t off = 3 * w + 4;
            S.outs[7][0 * hw2 + off] = 0.9f;  // scale2 cls（outs[7]）强
            S.outs[8][off] = 0.2f;            // sum 弱
        }
        auto cfg2 = test_cfg();
        cfg2.num_cls = 2;  // cls=1 的目标须 num_cls≥2 才可见（decode 扫 c<num_cls）
        const float* in[9];
        for (int i = 0; i < 9; ++i) in[i] = S.outs[i].data();
        auto boxes = yolov8_decode(in, S.dims, cfg2);
        CHECK_MSG(boxes.size() == 2, "期望 2 框, 实得 %zu", boxes.size());
        for (auto& b : boxes) CHECK(b.cls == 0 || b.cls == 1);
    }

    // ---- 3. NMS：同类高 IoU 抑制 / 异类不抑制 ----
    {
        std::fprintf(stderr, "[T3]\n");
        std::vector<YoloBox> cands;
        YoloBox a;
        a.x = 100, a.y = 100, a.w = 50, a.h = 50, a.conf = 0.9f, a.cls = 0;
        YoloBox b = a;
        b.conf = 0.6f;  // IoU=1 与 a 完全重叠 → 抑制
        YoloBox c = a;
        c.cls = 1;      // 异类 → 保留
        cands.push_back(b);
        cands.push_back(a);
        cands.push_back(c);
        auto out = nms_per_class(std::move(cands), 0.45f);
        CHECK_MSG(out.size() == 2, "NMS 期望 2 框, 实得 %zu", out.size());
        if (out.size() == 2) {
            CHECK_NEAR(out[0].conf, 0.9f, 0.001f);  // 高分者胜
            CHECK(out[1].cls == 1);
        }
    }

    // ---- 4. make_yolov8_post 闭包：逆 letterbox 变换（详设 §12.5 契约）----
    {
        std::fprintf(stderr, "[T4]\n");
        Synth S(1, 80, 80, 40, 40, 20, 20);
        S.place(0, 10, 20, 2, 3, 4, 5, 0, 0.9f);  // 模型系 (68,140,48,64)
        std::vector<std::pair<uint32_t, uint32_t>> out_hw = {
            {80, 80}, {80, 80}, {80, 80}, {40, 40}, {40, 40}, {40, 40},
            {20, 20}, {20, 20}, {20, 20}};
        auto post = make_yolov8_post(test_cfg(), out_hw);

        // 640x480 → 640x640: scale=1, pad_y=80（letterbox 同源参数）
        LetterBoxParam lb;
        lb.src_w = 640;
        lb.src_h = 480;
        lb.dst_w = 640;
        lb.dst_h = 640;
        lb.scale = 1.f;
        lb.pad_x = 0;
        lb.pad_y = 80;
        auto ts = post(S.outs, lb);
        std::fprintf(stderr, "[T4 ret=%zu]\n", ts.size());
        CHECK_MSG(ts.size() == 1, "期望 1 目标, 实得 %zu", ts.size());
        if (ts.size() == 1) {
            // y_src = (140-80)/1 = 60（逆变换正确性）
            CHECK_NEAR(ts[0].y, 60.f, 0.01f);
            CHECK_NEAR(ts[0].x, 68.f, 0.01f);
            CHECK_NEAR(ts[0].w, 48.f, 0.01f);
            CHECK_NEAR(ts[0].h, 64.f, 0.01f);
        }
    }

    // ---- 5. 逆变换 + 原图边界裁剪（框下半截越出原图被 clamp）----
    {
        std::fprintf(stderr, "[T5]\n");
        Synth S(1, 80, 80, 40, 40, 20, 20);
        // 模型系: cy=(70.5)*8=564, y=564-32=532, h=64 → 原图系 y=532-80=452,
        // 452+64=516 > 480 → h 裁到 480-452=28（真实部分裁剪断言）
        S.place(0, 40, 70, 4, 4, 4, 4, 0, 0.9f);
        // out_hw 必须与 Synth 实际缓冲一致（80/40/20）——若虚报 80x80，
        // decode 按 hw=6400 越界读 1600 元素的缓冲 → SEGFAULT（本用例曾犯此错）
        std::vector<std::pair<uint32_t, uint32_t>> out_hw = {
            {80, 80}, {80, 80}, {80, 80}, {40, 40}, {40, 40}, {40, 40},
            {20, 20}, {20, 20}, {20, 20}};
        auto post = make_yolov8_post(test_cfg(), out_hw);
        LetterBoxParam lb;
        lb.src_w = 640;
        lb.src_h = 480;
        lb.dst_w = 640;
        lb.dst_h = 640;
        lb.scale = 1.f;
        lb.pad_x = 0;
        lb.pad_y = 80;
        auto ts = post(S.outs, lb);
        std::fprintf(stderr, "[T5 ret=%zu]\n", ts.size());
        CHECK_MSG(ts.size() == 1, "期望 1 目标, 实得 %zu", ts.size());
        if (ts.size() == 1) {
            CHECK_NEAR(ts[0].x, 292.f, 0.01f);
            CHECK_NEAR(ts[0].y, 452.f, 0.01f);
            CHECK_NEAR(ts[0].w, 64.f, 0.01f);
            CHECK_NEAR(ts[0].h, 28.f, 0.01f);  // 裁剪: 64 → 28
        }
    }

    // ---- 6. 非 zoo 输出数防御：post 闭包恒返回空 ----
    {
        std::fprintf(stderr, "[T6 pre]\n");
        auto post = make_yolov8_post(test_cfg(), {{80, 80}});  // 1 个输出
        LetterBoxParam lb;
        std::fprintf(stderr, "[T6 post]\n");
        auto ts = post({{}, {}, {}, {}, {}, {}, {}, {}, {}}, lb);
        std::fprintf(stderr, "[T6 ret=%zu]\n", ts.size());
        CHECK(ts.empty());
    }

    // ---- 7. 类白名单：只解码白名单通道，非白名单类不产出（板上 bus 残留实证）----
    {
        std::fprintf(stderr, "[T7]\n");
        // 3 类模型：cls=0 人 0.9、cls=5 bus 0.8、cls=7 truck 0.7
        Synth S(8, 80, 80, 40, 40, 20, 20);
        S.place(0, 10, 20, 2, 3, 4, 5, 0, 0.9f);
        S.place(0, 40, 30, 3, 3, 3, 3, 5, 0.8f);
        S.place(0, 60, 40, 2, 2, 2, 2, 7, 0.7f);
        const float* in[9];
        for (int i = 0; i < 9; ++i) in[i] = S.outs[i].data();

        // 7a. 无白名单（兼容存量包）：3 框全出
        auto cfg_all = test_cfg();
        cfg_all.num_cls = 8;
        auto all = yolov8_decode(in, S.dims, cfg_all);
        CHECK_MSG(all.size() == 3, "无白名单期望 3 框, 实得 %zu", all.size());

        // 7b. 白名单 {0}：只剩 person，bus/truck 不产出
        auto cfg_p = test_cfg();
        cfg_p.num_cls = 8;
        cfg_p.classes = {0};
        auto person = yolov8_decode(in, S.dims, cfg_p);
        CHECK_MSG(person.size() == 1, "白名单{0} 期望 1 框, 实得 %zu", person.size());
        if (person.size() == 1) CHECK(person[0].cls == 0);

        // 7c. 多类白名单 {0,7}：person + truck，bus 不产出
        auto cfg_pt = test_cfg();
        cfg_pt.num_cls = 8;
        cfg_pt.classes = {0, 7};
        auto pt = yolov8_decode(in, S.dims, cfg_pt);
        CHECK_MSG(pt.size() == 2, "白名单{0,7} 期望 2 框, 实得 %zu", pt.size());
        bool has_bus = false;
        for (auto& b : pt)
            if (b.cls == 5) has_bus = true;
        CHECK(!has_bus);
    }

    // ---- 8. 分数语义回归：最终分 = 单类最大分，不乘 sum 分支（zoo 口径）----
    // bug 版 conf = cls×sum（sum=Σcls）≈ 平方：真实分 0.6 的低分帧被压到
    // 0.42 < 0.5 丢弃——板上实证"静止能检、运动检不出、PC 端正常"的根因
    {
        std::fprintf(stderr, "[T8]\n");
        Synth S(2, 80, 80, 40, 40, 20, 20);
        S.place(0, 10, 10, 1, 1, 1, 1, 0, 0.6f);  // cls=0.6（sum 被 place 写 1.0）
        const int w = S.dims[0][1];
        S.outs[2][10 * w + 10] = 0.7f;  // 覆盖 sum = 0.7（其余类占 0.1）
        const float* in[9];
        for (int i = 0; i < 9; ++i) in[i] = S.outs[i].data();
        auto boxes = yolov8_decode(in, S.dims, test_cfg());  // conf_th=0.5
        // bug 版: 0.6*0.7=0.42 < 0.5 → 0 框；修复版: 0.6 ≥ 0.5 → 1 框
        CHECK_MSG(boxes.size() == 1, "score 语义（不乘 sum）期望 1 框, 实得 %zu",
                  boxes.size());
        if (boxes.size() == 1) CHECK_NEAR(boxes[0].conf, 0.6f, 0.001f);
    }
}

TEST_MAIN("yolov8_post")
