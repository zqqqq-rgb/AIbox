// IDecoder —— 解码器接缝（任务分解 T0.2）
// 实现：hw/MppDecoder（MPP 硬解，dma-buf fd 输出）、core/fakes/FileDecoder（裸 NV12 直通）
#pragma once

#include "interfaces/types.h"

namespace aibox {

class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual bool open(uint32_t width, uint32_t height) = 0;

    // 同步语义：返回 true 时 out 可用（借用语义，下次 decode 前有效）。
    // 板上实现内部必须消化 MPP 两个确定性坑（demo 排障 K2/K3）：
    //   1) 非阻塞取帧返回 MPP_OK + NULL frame 时应重试而非上抛；
    //   2) put_packet 返回 -1012(BUFFER_FULL) 时先 pull 排空再重试。
    virtual bool decode(const Packet& in, Frame& out) = 0;

    virtual void close() = 0;
};

}  // namespace aibox
