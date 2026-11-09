#include "resource_budget.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
namespace aibox {
namespace biz {
// ---- NPU 预检（纯函数）----

namespace {

double compute_pct_of(const std::vector<PrecheckUsage>& usages,
                      const std::map<std::string, PrecheckModel>& models, int npu_cores) {
    std::map<std::string, int> fps_sum;
    for (const auto& u : usages)
        if (u.fps > 0)
            fps_sum[u.algo_id] += u.fps;
    double steady = 0, triggered = 0;
    for (const auto& kv : fps_sum) {
        const auto it = models.find(kv.first);
        if (it != models.end()) {
            auto& cost = it->second.triggered ? triggered : steady;
            cost += it->second.cost_ms * kv.second;
        }
    }
    const double denom = npu_cores * 1000.0 * 0.8; // ms/s × 80% 水位
    return denom > 0 ? std::max(steady / denom, triggered / 800.0) * 100.0 : 0.0;
}

} // namespace

PrecheckResult npu_precheck(const PrecheckInput& in) {
    std::map<std::string, PrecheckModel> models;
    for (const auto& m : in.models)
        models[m.algo_id] = m;

    PrecheckResult r;
    if (in.npu_cores < 1 || in.npu_mem_total_kb <= 0) {
        r.code = 1001;
        r.msg = "invalid NPU budget configuration";
        return r;
    }
    for (const auto& usage : in.usages) {
        if (!models.count(usage.algo_id)) {
            r.code = 1002;
            r.msg = "algo not available: " + usage.algo_id;
            return r;
        }
        const auto& model = models.at(usage.algo_id);
        if (!std::isfinite(model.cost_ms) || model.cost_ms <= 0 || model.mem_kb < 0) {
            r.code = 1003;
            r.msg = "model resource measurements are unavailable";
            return r;
        }
    }
    r.compute_pct = compute_pct_of(in.usages, models, in.npu_cores);

    // 内存：去重物理模型 Σ ctx
    std::map<std::string, int> seen;
    int64_t mem = 0;
    for (const auto& u : in.usages) {
        if (seen[u.algo_id]++)
            continue;
        const auto it = models.find(u.algo_id);
        if (it != models.end())
            mem += it->second.mem_kb;
    }
    r.mem_pct =
        in.npu_mem_total_kb > 0
            ? static_cast<double>(mem) / (static_cast<double>(in.npu_mem_total_kb) * 0.8) * 100.0
            : 0.0;

    r.ok = r.compute_pct <= 100.0 && r.mem_pct <= 100.0;
    if (r.ok)
        return r;

    if (r.compute_pct > 100.0) {
        r.code = 3001;
        // 建议：focus_cam 的勾选逐档降 fps（→3 / →1）试算，取首个达标档
        for (int target : {3, 1}) {
            std::vector<PrecheckUsage> trial = in.usages;
            for (auto& u : trial)
                if (u.cam == in.focus_cam && u.fps > target)
                    u.fps = target;
            const double pct = compute_pct_of(trial, models, in.npu_cores);
            if (pct <= 100.0) {
                std::ostringstream os;
                os << "cam" << in.focus_cam << " fps →" << target << " 可降至"
                   << static_cast<int>(pct + 0.5) << "%";
                r.msg = os.str();
                return r;
            }
        }
        r.msg = "请降低检测 fps 或减少勾选通道";
    } else {
        r.code = 3002;
        r.msg = "NPU 内存预检超限，请减少勾选的模型数";
    }
    return r;
}

} // namespace biz
} // namespace aibox
