// NpuMetricsSource —— IMetricsSource 板端实现（任务分解 T3.x，详设 §3.9）
// sysfs 采集：NPU 负载（debugfs rknpu/load）、DDR（devfreq dmc/load）、温度（thermal_zone）
// 文件缺失/格式变化 → 字段回 0（降级判定按 0 处理 = 不触发，绝不抛异常/阻塞）
// rq_steady_depth / disk_high 与 sysfs 无关，由 main 装配在 on_snapshot 回调里回填
#pragma once

#include <memory>
#include <string>

#include "arbiter/DegradeController.h"

namespace aibox {

class NpuMetricsSource : public IMetricsSource {
public:
    MetricsSnapshot sample() override;

private:
    // 读整个小文件（sysfs 均 < 4KB）；失败返回空串
    static std::string read_file(const std::string& path);
};

}  // namespace aibox
