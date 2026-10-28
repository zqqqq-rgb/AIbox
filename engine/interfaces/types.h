// AIBox 核心数据结构定稿（任务分解 T0.2）
// 依据《AIBox 软件详细设计 V1.0》§2 数据结构、§3.1 数据通路
#pragma once

#include <cstdint>
#include <vector>

namespace aibox {

// 全链路统一像素格式：NV12（MPP 解码输出 / RGA 输入 / VOP 直扫）
enum class PixelFormat : uint8_t { kNV12 };

// 压缩码流包（ISource 产出，IDecoder 消费）
struct Packet {
    const uint8_t* data = nullptr;  // ES 数据（借用语义，有效期至下一次 read）
    uint32_t size = 0;
    uint64_t pts_ms = 0;            // 源时间戳（归零透传，前端 pts 对齐的锚点）
};

// 解码帧：dma-buf fd 与 CPU 指针双通道（详设 §2.1）
// 板上零拷贝路径走 fd（VDEC→RGA→NPU），x86 测试/兜底路径走 ptr
struct Frame {
    uint64_t pts_ms = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::kNV12;
    uint32_t stride_y = 0;
    uint32_t stride_uv = 0;          // NV12：UV 行距 = width（w/2 对 × 2 字节）
    uint32_t vstride_y = 0;          // Y 平面总行数（MPP 对齐补行，UV 平面偏移 =
                                     // stride_y*vstride_y）；0 表示 = height（紧凑布局）

    int y_fd = -1;                   // dma-buf fd，-1 表示无
    int uv_fd = -1;
    const uint8_t* y_ptr = nullptr;  // 借用语义（生产者复用前会覆盖）
    const uint8_t* uv_ptr = nullptr;
    uint64_t frame_index = 0;        // 源内单调递增，跟踪/FakeInfer 对齐用
};

// 检测框：原图像素坐标（前端 canvas 叠加直用，服务端不碰像素）
struct Detection {
    float x = 0, y = 0;              // 左上角
    float w = 0, h = 0;              // 宽高
    float score = 0;
    int label = 0;
    int track_id = -1;               // M2 跟踪后填充
};

// 一帧的检测目标集合（UDS JSON-lines 广播的载体）
struct Targets {
    uint64_t frame_pts_ms = 0;
    uint64_t frame_index = 0;
    std::vector<Detection> boxes;
};

// letterbox 几何参数：SoftLetterbox / RgaPreproc 共享的对照量（黄金对照）
// 逆变换（模型坐标→原图坐标）：x_src = (x_model - pad_x) / scale
struct LetterBoxParam {
    uint32_t src_w = 0, src_h = 0;
    uint32_t dst_w = 0, dst_h = 0;
    float scale = 1.0f;              // 等比缩放因子 = min(dst/src)
    float pad_x = 0, pad_y = 0;      // 对称 padding（像素）
};

}  // namespace aibox
