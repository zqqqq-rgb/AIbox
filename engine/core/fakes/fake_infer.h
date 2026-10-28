// FakeInfer —— 确定性伪推理（IInferEngine 的 x86 实现）
// 按 run 次数（= 帧序）查预设目标表返回，使 M1/M2 流水线测试可复现。
// 与 demo pipeline 的"推理+后处理"合体线程对应：直接输出 Detection，
// 真实实现为 hw/RknnEngine（原始张量）+ core 后处理（DFL decode + NMS）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "interfaces/iinfer.h"
#include "interfaces/types.h"

namespace aibox {

class FakeInfer : public IInferEngine {
public:
    // presets[i] = 第 i 次 run 的输出；超出后沿用最后一帧（长流水测试不抖动）
    explicit FakeInfer(std::vector<Targets> presets);

    // CSV 格式：每行 "pts_ms,frame_index,label,score,x,y,w,h"，按 frame_index 升序分帧
    static std::vector<Targets> load_presets_csv(const std::string& path);

    bool load(const std::string& model_path) override;
    bool run(const InferTensor& in, std::vector<std::vector<float>>& outputs) override;

    // 最近一次 run 的结果（供流水线/测试直接断言）
    const Targets& last_targets() const { return last_; }

private:
    std::vector<Targets> presets_;
    size_t idx_ = 0;
    Targets last_;
};

}  // namespace aibox
