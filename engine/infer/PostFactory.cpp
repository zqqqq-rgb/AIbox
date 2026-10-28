#include "infer/PostFactory.h"

#include <algorithm>

#include "common/LetterboxMath.h"
#include "infer/Yolov8Post.h"

namespace aibox {

const std::vector<std::string>& known_post_types() {
    static const std::vector<std::string> kTypes = {"yolov8_zoo9", "det_generic",
                                                    "cls_top1"};
    return kTypes;
}

bool is_known_post_type(const std::string& post_type) {
    const auto& ts = known_post_types();
    return std::find(ts.begin(), ts.end(), post_type) != ts.end();
}

PostFn make_post_by_type(const std::string& post_type, const AlgoManifest& m,
                         const std::vector<std::pair<uint32_t, uint32_t>>& out_hw) {
    if (post_type == "yolov8_zoo9") {
        // zoo 9 输出布局校验：数量不符（如声明 zoo9 但模型是 1 输出）→ 空闭包 =
        // build 失败，注册表报 disabled（不静默用 default_post 解错坐标）
        if (out_hw.size() != 9) return {};
        YoloPostConfig cfg;
        cfg.num_cls = m.num_cls;
        cfg.input_w = m.in_w;
        cfg.input_h = m.in_h;
        cfg.conf_th = m.threshold > 0.f ? m.threshold : 0.45f;
        cfg.classes = m.classes;  // manifest 白名单（空 = 全类）
        return make_yolov8_post(cfg, out_hw);
    }
    if (post_type == "det_generic") {
        // 单输出 [n,6]：cls, conf, x, y, w, h（模型输入坐标系）→ inv_letterbox +
        // 裁剪到原图边界（阈值用 manifest.default_threshold，与 zoo9 口径一致）
        const float conf_th = m.threshold > 0.f ? m.threshold : 0.45f;
        const std::vector<int> classes = m.classes;  // manifest 白名单（空 = 全类）
        return [conf_th, classes](const std::vector<std::vector<float>>& outs,
                         const LetterBoxParam& lb) -> std::vector<proto::DetTarget> {
            std::vector<proto::DetTarget> ts;
            if (outs.empty()) return ts;
            const auto& o = outs[0];
            ts.reserve(o.size() / 6);
            for (size_t i = 0; i + 5 < o.size(); i += 6) {
                const float conf = o[i + 1];
                if (conf < conf_th) continue;
                const int cls = static_cast<int32_t>(o[i] + 0.5f);
                if (!classes.empty() &&
                    std::find(classes.begin(), classes.end(), cls) == classes.end()) {
                    continue;  // 白名单外
                }
                float x = o[i + 2], y = o[i + 3], w = o[i + 4], h = o[i + 5];
                inv_letterbox(x, y, w, h, lb);
                const float x0 = std::max(0.f, x), y0 = std::max(0.f, y);
                const float x1 = std::min(static_cast<float>(lb.src_w), x + w);
                const float y1 = std::min(static_cast<float>(lb.src_h), y + h);
                if (x1 <= x0 || y1 <= y0) continue;  // 完全出界
                proto::DetTarget t;
                t.cls = cls;
                t.conf = conf;
                t.x = x0;
                t.y = y0;
                t.w = x1 - x0;
                t.h = y1 - y0;
                ts.push_back(t);
            }
            return ts;
        };
    }
    if (post_type == "cls_top1") {
        // 单输出 [1,num_cls]（logits/probability 均可）→ argmax；无框语义 w=h=0
        //（前端按整幅判定消费：质检 NG/站坐/属性等二次分类）
        return [](const std::vector<std::vector<float>>& outs,
                  const LetterBoxParam&) -> std::vector<proto::DetTarget> {
            std::vector<proto::DetTarget> ts;
            if (outs.empty() || outs[0].empty()) return ts;
            const auto& o = outs[0];
            size_t best = 0;
            for (size_t i = 1; i < o.size(); ++i)
                if (o[i] > o[best]) best = i;
            proto::DetTarget t;
            t.cls = static_cast<int32_t>(best);
            t.conf = o[best];
            ts.push_back(t);
            return ts;
        };
    }
    return {};  // 未知类型（AlgoPackage 白名单已拦截，此处兜底）
}

}  // namespace aibox
