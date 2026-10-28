// IPreproc —— 预处理接缝：Frame(NV12) → 模型输入（缩放 + letterbox + 格式转换）
// 实现：hw/RgaPreproc（RGA 硬件，im2d 接口）、core/fakes/SoftLetterbox（CPU 黄金对照 + 兜底）
#pragma once

#include "interfaces/types.h"

namespace aibox {

// 模型输入张量：RGB888 打包（letterbox 区域为 0 填充）
struct InferTensor {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> data;  // size = width * height * 3
};

class IPreproc {
public:
    virtual ~IPreproc() = default;

    // 调用方先设 out.width/out.height（模型输入尺寸）；run 填充 data 并回写 param。
    // 黄金对照：同一输入下 SoftLetterbox 与 RgaPreproc 的 param 及输出应一致（demo 排障 K9）
    virtual bool run(const Frame& in, InferTensor& out, LetterBoxParam& param) = 0;
};

}  // namespace aibox
