// DmaBuffer —— 全链路流动的帧句柄（任务分解 T1.3，详设 §2.1）
// "传 fd 不传像素"（架构 §6.2）：板上一帧 NV12 只以 dma-buf fd 流过 VDEC→RGA→NPU
// 无析构语义：生命周期由 FramePool 经 shared_ptr 管理
#pragma once

#include <cstdint>

namespace aibox {

// DRM fourcc NV12（详设 fmt 字段取值）
constexpr uint32_t kDrmFmtNV12 = 'N' | ('V' << 8) | ('1' << 16) | ('2' << 24);

struct DmaBuffer {
    int fd = -1;              // dma-buf 文件描述符（x86 模拟：分配器发的合成句柄）
    uint32_t w = 0, h = 0;    // 分辨率（子码流如 640×480）
    uint32_t fmt = kDrmFmtNV12;
    int64_t pts = 0;          // 源 RTP 时间戳(90kHz)，随帧透传
    uint64_t frame_seq = 0;   // 抽帧计数（仅 AI 路径赋值，详设 §12.6 帧标识）
    void* map = nullptr;      // CPU 映射地址（x86 模拟必非空；板上 CPU 兜底路径用）
                              // 详设 §2.1 未列此字段，沿用 demo 实测的 fd+ptr 双通道结论
};

}  // namespace aibox
