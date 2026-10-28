// RtspPuller —— ISource 板端实现（任务分解 T3.1 配套，详设 §3.2，demo pull_thread 产品化）
// ffmpeg libavformat 拉流；复用 demo 已验证的关键做法：
//   - rtsp_transport=tcp + stimeout（看门狗）+ max_delay 压低（架构 §9 回环坑）
//   - SPS/PPS extradata（SDP 里的 AVCC/HVCC）转 Annex-B 作为首包喂解码器
//     （K1：H.264 硬解必须先拿 SPS/PPS，否则 dec=0fps）
//   - pts 归零透传（检测通道绝对大时间戳 → 会话首帧基准，前端对齐前提，详设 §12.5）
//   - interrupt_callback 支持跨线程 close() 中断阻塞（看门狗/stop 场景）
// ISource 契约：read 返回 false = EOF/错误（重连归 CameraChannel src 线程，单层重试）
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "interfaces/isource.h"

struct AVFormatContext;

namespace aibox {

class RtspPuller : public ISource {
public:
    enum class Codec { H264, H265 };

    // connect_timeout_s：open 阶段（含 SDP 协商）的看门狗上限
    RtspPuller(std::string url, uint32_t connect_timeout_s = 5);
    ~RtspPuller() override;

    bool open() override;
    bool read(Packet& out) override;
    void close() override;

    // open 成功后有效（EngineTop 用于选择 MPP 解码器类型）
    Codec codec() const { return codec_; }

private:
    void cleanup_locked();  // 释放 fmt_（须保证无并发 read 在飞）

    std::string url_;
    uint32_t connect_timeout_s_;
    std::mutex mu_;
    std::atomic<bool> abort_{false};
    std::atomic<bool> reading_{false};  // close 等待在飞的 av_read_frame 退出
    AVFormatContext* fmt_ = nullptr;
    int vidx_ = -1;
    Codec codec_ = Codec::H264;
    bool sps_pending_ = false;
    std::vector<uint8_t> annexb_;  // 首包（SPS/PPS Annex-B），read 借用
    std::vector<uint8_t> pkt_buf_; // 码流包缓冲，read 借用（有效至下一次 read）
    int64_t pts_base_ = -1;
};

}  // namespace aibox
