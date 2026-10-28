// AlgoInstance —— 物理模型的运行时实例（详设 §3.5 模型注册表 × §3.6 调度器）
// 一个物理模型 = 一个 NPU 上下文 = 一个调度桶；
// 多个逻辑算法（set_algos 的多个 algo 项）可共享同一实例（逻辑算法共享物理模型，架构 §4）
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "interfaces/iinfer.h"
#include "interfaces/ipreproc.h"
#include "ipc/Protocol.h"

namespace aibox {

// 输出解码闭包：签名携带本帧 LetterBoxParam——真模型（YOLOv8 zoo）输出在模型输入
// 坐标系，逆变换（inv_letterbox）必须按帧参数还原到原图坐标（M3 hw 集成，详设 §12.5）
using PostFn = std::function<std::vector<proto::DetTarget>(
    const std::vector<std::vector<float>>&, const LetterBoxParam&)>;

struct AlgoInstance {
    std::string algo_id;
    uint32_t weight = 1;  // DRR quantum = Σ勾选该模型的通道 fps（set_algos 时更新）
    uint32_t in_w = 640, in_h = 640;

    std::shared_ptr<IPreproc> preproc;    // x86: SoftLetterbox；板上: RgaPreproc
    std::shared_ptr<IInferEngine> infer;  // x86: FakeInfer；板上: RknnEngine

    // 输出解码：调度器只管时序不管模型内容，解码闭包在此注入
    PostFn post;

    // [n,6] 默认解码（与 FakeInfer 输出编码一致；FakeInfer 坐标已是原图系，lb 未用）
    static std::vector<proto::DetTarget> default_post(
        const std::vector<std::vector<float>>& outs, const LetterBoxParam&) {
        std::vector<proto::DetTarget> ts;
        if (outs.empty()) return ts;
        const auto& o = outs[0];
        ts.reserve(o.size() / 6);
        for (size_t i = 0; i + 5 < o.size(); i += 6) {
            proto::DetTarget t;
            t.cls = static_cast<int32_t>(o[i]);
            t.conf = o[i + 1];
            t.x = o[i + 2];
            t.y = o[i + 3];
            t.w = o[i + 4];
            t.h = o[i + 5];
            ts.push_back(t);
        }
        return ts;
    }
};

}  // namespace aibox
