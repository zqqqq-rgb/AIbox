#include "core/fakes/fake_infer.h"

#include <cstdio>
#include <cstdlib>

namespace aibox {

FakeInfer::FakeInfer(std::vector<Targets> presets) : presets_(std::move(presets)) {}

std::vector<Targets> FakeInfer::load_presets_csv(const std::string& path) {
    std::vector<Targets> presets;
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) {
        return presets;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        unsigned long long pts = 0, idx = 0;
        Detection d;
        if (sscanf(line, "%llu,%llu,%d,%f,%f,%f,%f,%f", &pts, &idx, &d.label, &d.score, &d.x,
                   &d.y, &d.w, &d.h) != 8) {
            continue;
        }
        if (presets.size() <= idx) {
            presets.resize(idx + 1);
        }
        presets[idx].frame_pts_ms = pts;
        presets[idx].frame_index = idx;
        presets[idx].boxes.push_back(d);
    }
    fclose(f);
    return presets;
}

bool FakeInfer::load(const std::string& /*model_path*/) {
    return true;  // 无模型可加载，恒成功
}

bool FakeInfer::run(const InferTensor& /*in*/, std::vector<std::vector<float>>& outputs) {
    // 帧序推进：超出预设后沿用最后一帧
    if (idx_ < presets_.size()) {
        last_ = presets_[idx_++];
    } else if (!presets_.empty()) {
        last_ = presets_.back();
    }

    // 输出编码：单张量 [n, 6] = {label, score, x, y, w, h}（模型坐标即原图像素坐标）
    outputs.assign(1, {});
    auto& out = outputs[0];
    out.reserve(last_.boxes.size() * 6);
    for (const auto& d : last_.boxes) {
        out.push_back(static_cast<float>(d.label));
        out.push_back(d.score);
        out.push_back(d.x);
        out.push_back(d.y);
        out.push_back(d.w);
        out.push_back(d.h);
    }
    return true;
}

}  // namespace aibox
