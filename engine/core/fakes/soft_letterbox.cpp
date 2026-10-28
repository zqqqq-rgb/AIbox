#include "core/fakes/soft_letterbox.h"

#include <cstddef>

#include "common/LetterboxMath.h"

namespace aibox {

namespace {

inline uint8_t clamp_u8(float v) {
    return v < 0.f ? 0 : (v > 255.f ? 255 : static_cast<uint8_t>(v + 0.5f));
}

// BT.601 limited range：Y[16,235], U/V[16,240]
inline void nv12_to_rgb(const uint8_t* y_plane, uint32_t stride_y,
                        const uint8_t* uv_plane, uint32_t stride_uv,
                        uint32_t sx, uint32_t sy, uint8_t* rgb) {
    const float yv = static_cast<float>(y_plane[sy * stride_y + sx]);
    const uint8_t* uv = uv_plane + (sy / 2) * stride_uv + (sx / 2) * 2;
    const float u = static_cast<float>(uv[0]) - 128.f;
    const float v = static_cast<float>(uv[1]) - 128.f;
    const float yc = yv - 16.f;
    rgb[0] = clamp_u8(1.164f * yc + 1.596f * v);           // R
    rgb[1] = clamp_u8(1.164f * yc - 0.391f * u - 0.813f * v);  // G
    rgb[2] = clamp_u8(1.164f * yc + 2.018f * u);           // B
}

}  // namespace

bool SoftLetterbox::run(const Frame& in, InferTensor& out, LetterBoxParam& param) {
    if (in.y_ptr == nullptr || in.uv_ptr == nullptr || out.width == 0 || out.height == 0) {
        return false;
    }

    // 黄金数学：与 RgaPreproc（M3）共用 compute_letterbox，参数逐值一致（T1.4）
    param = compute_letterbox(in.width, in.height, out.width, out.height);
    if (param.scale <= 0.f) {
        return false;
    }

    const uint32_t dst_w = out.width, dst_h = out.height;
    out.data.assign(static_cast<size_t>(dst_w) * dst_h * 3, 0);  // 先整体填黑（padding 区）

    const float inv_scale = 1.f / param.scale;
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const float fy = (dy - param.pad_y) * inv_scale;
        if (fy < 0.f || fy >= in.height) {
            continue;  // pad 行保持黑色
        }
        const uint32_t sy = static_cast<uint32_t>(fy);
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const float fx = (dx - param.pad_x) * inv_scale;
            if (fx < 0.f || fx >= in.width) {
                continue;  // pad 列保持黑色
            }
            nv12_to_rgb(in.y_ptr, in.stride_y, in.uv_ptr, in.stride_uv,
                        static_cast<uint32_t>(fx), sy,
                        &out.data[(static_cast<size_t>(dy) * dst_w + dx) * 3]);
        }
    }
    return true;
}

}  // namespace aibox
