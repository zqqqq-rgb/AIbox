#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace aibox {
namespace biz {
// ---- NPU 预检（纯函数，独立单测）----

struct PrecheckModel {
    std::string algo_id; // 物理模型 = algo_id
    double cost_ms = 0;  // 仅 rknn_run 口径
    int64_t mem_kb = 0;  // ctx 内存
    bool triggered = false;
};

struct PrecheckUsage {
    std::string algo_id;
    int cam = 0;
    int fps = 1;
};

struct PrecheckInput {
    std::vector<PrecheckModel> models;  // 注册表全部（成本查表）
    std::vector<PrecheckUsage> usages;  // 全系统勾选（含本次设置的目标通道）
    int focus_cam = 0;                  // 本次设置的通道（建议文案指向它）
    int npu_cores = 2;                  // steady 分配核数
    int64_t npu_mem_total_kb = 2097152; // NPU ctx 内存上限（默认 2GB，可配）
};

struct PrecheckResult {
    double compute_pct = 0, mem_pct = 0;
    bool ok = false;
    int code = 0;    // 0 / 3001 / 3002
    std::string msg; // 超限时带建议："cam3 fps 5→3 可降至87%"
};

PrecheckResult npu_precheck(const PrecheckInput& in);

} // namespace biz
} // namespace aibox
