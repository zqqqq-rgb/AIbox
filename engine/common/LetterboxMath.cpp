#include "common/LetterboxMath.h"

#include <algorithm>

namespace aibox {

LetterBoxParam compute_letterbox(uint32_t src_w, uint32_t src_h, uint32_t dst_w,
                                 uint32_t dst_h) {
    LetterBoxParam p;
    p.src_w = src_w;
    p.src_h = src_h;
    p.dst_w = dst_w;
    p.dst_h = dst_h;
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        p.scale = 0.f;
        return p;  // 非法输入：调用方（RGA/SoftLetterbox）应先行校验
    }
    const double s = std::min(static_cast<double>(dst_w) / src_w,
                              static_cast<double>(dst_h) / src_h);
    p.scale = static_cast<float>(s);
    p.pad_x = static_cast<float>((dst_w - src_w * s) / 2.0);
    p.pad_y = static_cast<float>((dst_h - src_h * s) / 2.0);
    return p;
}

void inv_letterbox(float& x, float& y, float& w, float& h, const LetterBoxParam& p,
                   float crop_x, float crop_y) {
    const double s = p.scale;
    const double nx = (static_cast<double>(x) - p.pad_x) / s;
    const double ny = (static_cast<double>(y) - p.pad_y) / s;
    x = static_cast<float>(crop_x + nx);
    y = static_cast<float>(crop_y + ny);
    w = static_cast<float>(w / s);
    h = static_cast<float>(h / s);
}

}  // namespace aibox
