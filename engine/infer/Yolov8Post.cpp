// Yolov8Post 实现 —— demo yolov8_post.cpp 的纯逻辑移植（板上已验证的算法原样保留）
#include "infer/Yolov8Post.h"

#include "common/Log.h"

#include <algorithm>
#include <cmath>

namespace aibox {

namespace {

// DFL: 16 bin softmax 后加权求和得到一条边的回归值(0~15 的期望)
// K7 布局: reg 张量 NCHW [64,h,w]，通道 = edge*16 + bin，bin 间相距 hw
float dfl_softmax_expect(const float* base, size_t stride, int len) {
    float m = base[0];
    for (int i = 1; i < len; ++i) m = std::max(m, base[i * stride]);
    float sum = 0.f, acc = 0.f;
    for (int i = 0; i < len; ++i) {
        float e = std::exp(base[i * stride] - m);
        sum += e;
        acc += e * i;
    }
    return acc / sum;
}

}  // namespace

std::vector<YoloBox> yolov8_decode(const float* inputs[9], const int dims[9][2],
                                   const YoloPostConfig& cfg) {
    // 空指针防御：调用方（make_yolov8_post 闭包）输入张量数==9 但元素可为空
    // vector（异常路径），data()==nullptr 时直接返回空，防解引用崩
    for (int i = 0; i < 9; ++i) {
        if (!inputs[i]) return {};
    }
    std::vector<YoloBox> cands;
    cands.reserve(512);

    for (int s = 0; s < 3; ++s) {
        const int h = dims[s * 3][0], w = dims[s * 3][1];
        const int stride = cfg.strides[s];
        const float* reg = inputs[s * 3 + 0];  // [64, h, w]
        const float* cls = inputs[s * 3 + 1];  // [C,  h, w]
        const float* sum = inputs[s * 3 + 2];  // [1,  h, w]
        const size_t hw = static_cast<size_t>(h) * w;

        for (int gy = 0; gy < h; ++gy) {
            for (int gx = 0; gx < w; ++gx) {
                size_t off = static_cast<size_t>(gy) * w + gx;
                float obj = sum[off];  // zoo score_sum = Σcls ≥ cls_max（安全上界）
                if (obj < cfg.conf_th * 0.5f) continue;  // 粗筛(省 decode；减半=保守上界)

                float l = dfl_softmax_expect(reg + (0 * cfg.dfl_len) * hw + off, hw,
                                             cfg.dfl_len);
                float t = dfl_softmax_expect(reg + (1 * cfg.dfl_len) * hw + off, hw,
                                             cfg.dfl_len);
                float r = dfl_softmax_expect(reg + (2 * cfg.dfl_len) * hw + off, hw,
                                             cfg.dfl_len);
                float b = dfl_softmax_expect(reg + (3 * cfg.dfl_len) * hw + off, hw,
                                             cfg.dfl_len);

                float cx = (gx + 0.5f) * stride;
                float cy = (gy + 0.5f) * stride;
                YoloBox box;
                box.x = cx - l * stride;
                box.y = cy - t * stride;
                box.w = (l + r) * stride;
                box.h = (t + b) * stride;

                float best = 0.f;
                int bi = -1;
                // 最终分数 = 单类最大分（zoo 口径：python demo 的 score 分支恒 1，
                // C++ demo 的 score_sum 仅粗筛）——不乘 sum！sum=Σcls≥cls_max，
                // 相乘 ≈ 平方：真实分 0.5 的运动模糊帧被压到 0.25 低于阈值
                //（板上实证：静止能检、运动检不出、PC 端正常）
                if (!cfg.classes.empty()) {
                    for (const int c : cfg.classes) {
                        const float sc = cls[static_cast<size_t>(c) * hw + off];
                        if (sc > best) {
                            best = sc;
                            bi = c;
                        }
                    }
                } else {
                    for (int c = 0; c < cfg.num_cls; ++c) {
                        const float sc = cls[static_cast<size_t>(c) * hw + off];
                        if (sc > best) {
                            best = sc;
                            bi = c;
                        }
                    }
                }
                if (best < cfg.conf_th || bi < 0) continue;
                box.conf = best;
                box.cls = bi;
                cands.push_back(box);
            }
        }
    }
    return nms_per_class(std::move(cands), cfg.nms_iou);
}

std::vector<YoloBox> nms_per_class(std::vector<YoloBox>&& cands, float iou_th) {
    std::sort(cands.begin(), cands.end(),
              [](const YoloBox& a, const YoloBox& b) { return a.conf > b.conf; });
    std::vector<char> alive(cands.size(), 1);
    std::vector<YoloBox> out;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (!alive[i]) continue;
        const auto& a = cands[i];
        out.push_back(a);
        const float ax2 = a.x + a.w, ay2 = a.y + a.h;
        for (size_t j = i + 1; j < cands.size(); ++j) {
            if (!alive[j] || cands[j].cls != a.cls) continue;
            const auto& b = cands[j];
            float iw = std::max(0.f, std::min(ax2, b.x + b.w) - std::max(a.x, b.x));
            float ih = std::max(0.f, std::min(ay2, b.y + b.h) - std::max(a.y, b.y));
            float inter = iw * ih;
            float uni = a.w * a.h + b.w * b.h - inter;
            if (uni > 0 && inter / uni > iou_th) alive[j] = 0;
        }
    }
    return out;
}

std::function<std::vector<proto::DetTarget>(const std::vector<std::vector<float>>&,
                                            const LetterBoxParam&)>
make_yolov8_post(YoloPostConfig cfg,
                 const std::vector<std::pair<uint32_t, uint32_t>>& out_hw) {
    // 尺度张量布局随模型定稿一次，闭包内静态化（热升级 = 新 AlgoInstance = 新闭包）
    int dims[9][2];
    if (out_hw.size() >= 9) {
        for (int i = 0; i < 9; ++i) {
            dims[i][0] = static_cast<int>(out_hw[i].first);
            dims[i][1] = static_cast<int>(out_hw[i].second);
        }
    } else {
        LOGW("yolov8", "输出数=%zu != 9，post 闭包返回空（模型非 zoo 版）",
                     out_hw.size());
        cfg.conf_th = 2.f;  // 恒不触发，防御空 dims
    }

    return [cfg, dims](const std::vector<std::vector<float>>& outs,
                       const LetterBoxParam& lb) -> std::vector<proto::DetTarget> {
        if (outs.size() < 9) return {};
        const float* in[9];
        for (int i = 0; i < 9; ++i) in[i] = outs[i].data();
        auto boxes = yolov8_decode(in, dims, cfg);

        std::vector<proto::DetTarget> ts;
        ts.reserve(boxes.size());
        for (auto& b : boxes) {
            // 逆变换（模型系 → 原图系，详设 §12.5 契约：对外坐标 = 原始分辨率）
            float x = (b.x - lb.pad_x) / lb.scale;
            float y = (b.y - lb.pad_y) / lb.scale;
            float w = b.w / lb.scale;
            float h = b.h / lb.scale;
            // 裁剪到原图（letterbox 区域可能出框：模型可见 pad 区）
            x = std::clamp(x, 0.f, static_cast<float>(lb.src_w));
            y = std::clamp(y, 0.f, static_cast<float>(lb.src_h));
            w = std::clamp(w, 0.f, static_cast<float>(lb.src_w) - x);
            h = std::clamp(h, 0.f, static_cast<float>(lb.src_h) - y);
            if (w < 1.f || h < 1.f) continue;

            proto::DetTarget t;
            t.cls = b.cls;
            t.conf = b.conf;
            t.x = x;
            t.y = y;
            t.w = w;
            t.h = h;
            ts.push_back(t);
        }
        return ts;
    };
}

}  // namespace aibox
