// FileDecoder —— 测试用解码器（IDecoder 的 x86 实现）
// 把每个 Packet 视作一帧裸 NV12（测试资产由 scripts/gen_test_assets.py 生成）。
// 演示 Frame 的 CPU 指针路径；板上真实现 hw/MppDecoder 走 dma-buf fd 路径。
#pragma once

#include "interfaces/idecoder.h"

namespace aibox {

class FileDecoder : public IDecoder {
public:
    bool open(uint32_t width, uint32_t height) override;
    bool decode(const Packet& in, Frame& out) override;
    void close() override;

    static constexpr uint32_t nv12_size(uint32_t w, uint32_t h) { return w * h * 3 / 2; }

private:
    uint32_t width_ = 0, height_ = 0;
    uint64_t frame_index_ = 0;
};

}  // namespace aibox
