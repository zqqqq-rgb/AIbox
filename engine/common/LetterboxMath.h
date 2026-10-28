// Letterbox 数学 —— compute/inv 黄金对照量（任务分解 T1.4，契约详设 §12.5）
// SoftLetterbox（core/fakes）与 RgaPreproc（hw，M3）必须产出逐值一致的参数
// 逆变换（模型坐标→原图坐标）：x_src = (x_model - pad_x) / s
// ROI 裁剪路径：先裁剪再 letterbox，逆变换需加回 crop 偏移（详设 §12.5）
#pragma once

#include <cstdint>

#include "interfaces/types.h"  // LetterBoxParam

namespace aibox {

// 等比缩放 + 对称 padding：s = min(dst/src)，pad 居中
// 内部以 double 计算（float 存储时 854→640 这类非整倍率往返误差可控在亚像素）
LetterBoxParam compute_letterbox(uint32_t src_w, uint32_t src_h, uint32_t dst_w,
                                 uint32_t dst_h);

// 模型坐标 → 原图坐标（原地逆变换）
// crop_x/crop_y：ROI 裁剪路径传裁剪框在原图中的左上角（非 ROI 路径传 0）
void inv_letterbox(float& x, float& y, float& w, float& h, const LetterBoxParam& p,
                   float crop_x = 0.f, float crop_y = 0.f);

}  // namespace aibox
