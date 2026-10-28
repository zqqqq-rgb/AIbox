// MppDecoder —— IDecoder 板端实现（任务分解 T3.1，详设 §3.3，demo mpp_dec.cpp 产品化）
//
// demo 五个确定性坑全部内建（排障 K1~K6，任务分解 §7 附表要求逐条自检）：
//   K2 非阻塞 decode_get_frame 返回 MPP_OK+NULL → break 返回，禁止 continue 死循环
//   K3 decode_put_packet 返 -1012(BUFFER_FULL) → 先 pull 排空再重试（活锁根因：
//      info-change 无人应答 → 解析线程卡死 → put 永远 BUFFER_FULL）
//   K4 帧 buffer 组在 info-change 分支挂载（DRM 类型 → mpp_buffer_get_fd 直出 dma-buf）
//   K5 非阻塞超时 MPP_SET_OUTPUT_TIMEOUT=0 合法（非 0 才会阻塞）
//   K6 fast-parse/immediate-out 须在 mpp_init 前设置；RTSP 包已按 AU 对齐，
//      不用 PARSER_SPLIT_MODE
//
// 生命周期纪律（详设 §7.7）：decode 返回的 Frame 是 MPP 内部 buffer 的借用——
// 有效期至下一次 decode/close。M3 首版走"保险 memcpy"路径（CameraChannel 拷入
// FramePool），fd 直出短路拷贝是 T3.1 24h 实测的决策点，直出模式保留接口（fd 通道）
#pragma once

#include <cstdint>
#include <memory>

#include "interfaces/idecoder.h"

namespace aibox {

class MppDecoder : public IDecoder {
public:
    enum class Codec { H264, H265 };

    MppDecoder();
    ~MppDecoder() override;  // Impl 前置声明，析构需完整类型

    // IDecoder：默认 H.264（与 T0.2 接口兼容；分辨率由 SPS 决定，参数仅作期望值）
    bool open(uint32_t width, uint32_t height) override;
    bool open(uint32_t width, uint32_t height, Codec codec);
    bool decode(const Packet& in, Frame& out) override;
    void close() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aibox
