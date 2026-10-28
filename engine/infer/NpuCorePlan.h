// NpuCorePlan —— NPU 核分配计划（多核调度配置面）
//
// 双队列 → 物理核映射：steady（常驻检测）/ triggered（触发与级联）各占一组互斥核。
// 载体：--npu-cores CLI（main 解析）；auto = /proc/device-tree/compatible 检测 SoC：
//   rk3588（3 核）→ steady=[0,1]  triggered=[2]  （steady 吞吐×2 + 长尾池化——常驻
//     丢帧是质量事故；triggered 单核 + pri 逐出兜底——触发晚一帧无谓，SLO 硬侧多拿核）
//   rk3576（2 核）→ steady=[0]    triggered=[1]  （与双核历史行为一致）
//   未知 SoC     → 按 2 核兜底
// 显式覆盖示例：--npu-cores steady=0:trig=1,2（密集抓拍/全级联场景把多余核给 triggered）
// 纪律：核 id 0~2 且不重复；两队列至少各 1 核（校验失败由调用方回退默认并告警）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aibox {

struct NpuCorePlan {
    std::vector<uint8_t> steady_cores;
    std::vector<uint8_t> triggered_cores;

    size_t steady_n() const { return steady_cores.size(); }
    size_t triggered_n() const { return triggered_cores.size(); }

    // 一致性校验：非空、id ≤2、不重复、两队列不相交
    bool valid(std::string* err = nullptr) const;

    // 按总核数给默认分配：2→[0]|[1]，3→[0,1]|[2]；其余返回 false
    static bool default_for(size_t total, NpuCorePlan& out, std::string* err = nullptr);

    // SoC auto：device-tree compatible 检测（rk3588→3 / rk3576→2 / 未知→2 兜底）
    static NpuCorePlan auto_detect(std::string* note = nullptr);

    // 显式解析："steady=0,1:trig=2"（队列名 steady|triggered|st|trig；失败 out 不动）
    static bool parse(const std::string& s, NpuCorePlan& out, std::string* err = nullptr);

    // 测试/单 worker 兜底：steady=[0] triggered=[1]（历史双核行为）
    static NpuCorePlan fallback() { return {{0}, {1}}; }
};

}  // namespace aibox
