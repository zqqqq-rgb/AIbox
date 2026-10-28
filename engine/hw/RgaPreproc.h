// RgaPreproc —— IPreproc 板端实现（任务分解 T3.2，详设 §12.4/§12.5，demo rga_letterbox.cpp 产品化）
// RGA 一趟完成：缩放 + letterbox 有效区写入 + NV12→RGB888 格式转换
// 输入双通道：Frame.y_fd（dma-buf 直通，零拷贝）优先，失败自动降 y_ptr（保险路径）
// 黄金对照：letterbox 参数一律由 LetterboxMath::compute_letterbox 计算（与 SoftLetterbox
// 同一数学源，T1.4 定稿），padding 填 0 与 SoftLetterbox 一致（逐像素比对的前提）
// K9 坑内建：im2d 成功返回 IM_STATUS_SUCCESS(=1) 而非 0，判据 ret <= 0
#pragma once

#include "interfaces/ipreproc.h"

namespace aibox {

class RgaPreproc : public IPreproc {
public:
    RgaPreproc() = default;
    ~RgaPreproc() override = default;

    // 调用方先设 out.width/out.height（模型输入尺寸）；run 填充 data 并回写 param
    bool run(const Frame& in, InferTensor& out, LetterBoxParam& param) override;
};

}  // namespace aibox
