// SoftLetterbox —— CPU 黄金对照实现（IPreproc 的 x86 实现）
// 双重角色（详设 §3.5）：
//   1) RgaPreproc 的黄金对照：同输入下几何参数与输出必须一致（demo 排障 K9）
//   2) RGA 不可用时的 CPU 兜底路径（降级阶梯）
// 实现：最近邻采样 + NV12→RGB888（BT.601 limited range），letterbox 区域填 0
#pragma once

#include "interfaces/ipreproc.h"

namespace aibox {

class SoftLetterbox : public IPreproc {
public:
    bool run(const Frame& in, InferTensor& out, LetterBoxParam& param) override;
};

}  // namespace aibox
