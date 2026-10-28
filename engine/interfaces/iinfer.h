// IInferEngine —— 推理引擎接缝（任务分解 T0.2）
// 实现：hw/RknnEngine（NPU Core0/Core1 分区运行）、core/fakes/FakeInfer（确定性伪推理）
#pragma once

#include <string>
#include <vector>

#include "interfaces/ipreproc.h"

namespace aibox {

class IInferEngine {
public:
    virtual ~IInferEngine() = default;

    // 加载模型（rknn zoo 的 9 输出 YOLOv8 系列，详设 §3.6）
    virtual bool load(const std::string& model_path) = 0;

    // 同步推理。outputs 为原始输出张量（float），后处理（DFL decode + NMS，
    // 详见 demo yolov8_post.cpp）由 core 逻辑层完成，与引擎解耦。
    virtual bool run(const InferTensor& in, std::vector<std::vector<float>>& outputs) = 0;
};

}  // namespace aibox
