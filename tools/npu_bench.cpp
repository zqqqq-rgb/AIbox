// npu_bench —— 模型耗时实测工具（任务分解 T3.5，架构 §11.3/§7.5 口径纪律）
//
// 双口径输出（防混用，任务分解 §M3-T3.5 硬性要求）：
//   npu_cost_ms = 仅 rknn_run 段（纯 NPU；回填 manifest.json 的权威口径）
//   e2e_ms      = run() 全段（inputs_set + rknn_run + outputs_get + 反量化；参考值）
// 用法：
//   npu_bench --model yolov8n.rknn [--core steady|triggered|0|1|2] [--loop 100] [--conf 0.45]
// 输出末行为 manifest 回填片段（npn_cost_ms = loop 中位数，四舍五入两位小数）
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hw/RknnEngine.h"

using aibox::InferTensor;
using aibox::RknnEngine;

namespace {

uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double pct(std::vector<uint64_t> v, double p) {
    if (v.empty()) return 0;
    auto idx = static_cast<size_t>(p * (v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)] / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model, core = "steady";
    int loop = 100;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--model") model = next();
        else if (s == "--core") core = next();
        else if (s == "--loop") loop = std::atoi(next().c_str());
        else {
            std::fprintf(stderr,
                         "用法: npu_bench --model x.rknn [--core steady|triggered|0|1|2] "
                         "[--loop N]\n");
            return 2;
        }
    }
    if (model.empty() || loop < 1) {
        std::fprintf(stderr, "--model 必填\n");
        return 2;
    }

    RknnEngine eng;
    if (!eng.load(model)) {
        std::fprintf(stderr, "[npu_bench] load fail: %s\n", eng.last_error().c_str());
        return 1;
    }
    // --core：角色（steady|triggered）或 NpuCorePlan 显式物理核（0/1/2，分核实测口径）
    int core_id;
    if (core == "steady") {
        core_id = 0;
    } else if (core == "triggered") {
        core_id = 1;
    } else if (core.size() == 1 && core[0] >= '0' && core[0] <= '2') {
        core_id = core[0] - '0';
    } else {
        std::fprintf(stderr, "[npu_bench] --core 须为 steady|triggered|0|1|2: %s\n",
                     core.c_str());
        return 2;
    }
    if (!eng.set_core_index(core_id)) {
        std::fprintf(stderr, "[npu_bench] set_core fail: %s\n", eng.last_error().c_str());
        return 1;
    }
    std::printf("[npu_bench] model=%s core=%s(Core%d) in=%ux%u out_hw=", model.c_str(),
                core.c_str(), core_id, eng.in_w(), eng.in_h());
    for (size_t i = 0; i < eng.out_hw().size(); ++i)
        std::printf("%s%ux%u", i ? "," : "", eng.out_hw()[i].first,
                    eng.out_hw()[i].second);
    std::printf(" mem=%ukB\n", eng.mem_kb());

    // 输入：640×640×3 固定图案（NPU 时耗与像素内容无关，信噪由硬件保证）
    InferTensor in;
    in.width = 640;
    in.height = 640;
    in.data.resize(static_cast<size_t>(in.width) * in.height * 3);
    for (size_t i = 0; i < in.data.size(); ++i) in.data[i] = static_cast<uint8_t>(i & 0xff);

    std::vector<uint64_t> run_us, e2e_us;
    run_us.reserve(loop);
    e2e_us.reserve(loop);
    std::vector<std::vector<float>> outs;
    for (int i = 0; i < loop; ++i) {
        uint64_t t0 = now_us();
        if (!eng.run(in, outs)) {
            std::fprintf(stderr, "[npu_bench] run fail: %s\n", eng.last_error().c_str());
            return 1;
        }
        uint64_t t1 = now_us();
        run_us.push_back(eng.last_run_us());  // K12 口径：仅 rknn_run 段
        e2e_us.push_back(t1 - t0);            // 含 outputs_get + 反量化
    }
    std::sort(run_us.begin(), run_us.end());
    std::sort(e2e_us.begin(), e2e_us.end());

    std::printf("[npu_bench] loop=%d\n", loop);
    std::printf("npu_cost_ms(仅 rknn_run): 中位 %.2f  P99 %.2f  max %.2f\n",
                pct(run_us, 0.50), pct(run_us, 0.99), run_us.back() / 1000.0);
    std::printf("e2e_ms(inputs+run+get+dequant): 中位 %.2f  P99 %.2f  max %.2f\n",
                pct(e2e_us, 0.50), pct(e2e_us, 0.99), e2e_us.back() / 1000.0);
    std::printf("manifest 回填片段: \"npu_cost_ms\": %.2f   // 权威口径=仅 rknn_run 中位\n",
                pct(run_us, 0.50));
    return 0;
}
