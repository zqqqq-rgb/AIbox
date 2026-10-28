#include "common/EdgeMeter.h"

namespace aibox {

EdgeSnapshot diff(const EdgeCounters& now, const EdgeCounters& base, double dt_ms) {
    EdgeSnapshot s;
    const double dt_s = dt_ms / 1000.0;
    if (dt_s <= 0.0) {
        return s;  // 零窗口：速率无意义，返回全 0
    }
    const uint64_t d_in = now.in - base.in;
    const uint64_t d_out = now.out - base.out;
    s.in_rate = static_cast<double>(d_in) / dt_s;
    s.out_rate = static_cast<double>(d_out) / dt_s;
    s.drop_rate = static_cast<double>(now.drop - base.drop) / dt_s;
    s.sampled_rate = static_cast<double>(now.sampled - base.sampled) / dt_s;
    const uint64_t d_lat = now.latency_us_sum - base.latency_us_sum;
    s.avg_latency_ms = d_out > 0 ? static_cast<double>(d_lat) / d_out / 1000.0 : 0.0;
    return s;
}

}  // namespace aibox
