#include "arbiter/DegradeController.h"

#include <chrono>

namespace aibox {

DegradeController::DegradeController(std::vector<LevelCtl> levels)
    : levels_(std::move(levels)) {
    st_.resize(levels_.size());
}

void DegradeController::on_snapshot(const MetricsSnapshot& s) {
    uint8_t max_active = 0;
    for (size_t i = 0; i < levels_.size(); ++i) {
        const auto& lv = levels_[i];
        auto& st = st_[i];
        float v = lv.value ? lv.value(s) : 0.f;

        if (!st.active) {
            if (v >= lv.enter_threshold) {
                if (st.cond_since_ms < 0) st.cond_since_ms = s.ts_ms;
                if (s.ts_ms - st.cond_since_ms >= static_cast<int64_t>(lv.hold_ms)) {
                    st.active = true;  // 进入：条件须持续 hold_ms（滞回进入）
                    st.cond_since_ms = -1;
                    st.rec_since_ms = -1;
                    if (lv.action) lv.action(true);
                }
            } else {
                st.cond_since_ms = -1;  // 抖动归零（84→86→84 不累计）
            }
        } else {
            if (v <= lv.recover_threshold) {
                if (st.rec_since_ms < 0) st.rec_since_ms = s.ts_ms;
                if (s.ts_ms - st.rec_since_ms >=
                    static_cast<int64_t>(lv.recover_hold_ms)) {
                    st.active = false;  // 退出：恢复条件须持续 recover_hold_ms
                    st.rec_since_ms = -1;
                    if (lv.action) lv.action(false);
                }
            } else {
                st.rec_since_ms = -1;  // 滞回带内不退出也不重置进入态
            }
        }
        if (st.active && lv.level > max_active) max_active = lv.level;
    }

    uint8_t prev = cur_level_.exchange(max_active, std::memory_order_relaxed);
    if (prev != max_active && on_change_) {
        proto::DegradeEvt evt;
        evt.level = max_active;
        for (size_t i = 0; i < levels_.size(); ++i) {
            if (st_[i].active && levels_[i].level == max_active) {
                evt.reason = levels_[i].reason;
                break;
            }
        }
        on_change_(evt);
    }
}

std::vector<DegradeController::LevelCtl> DegradeController::default_levels() {
    std::vector<LevelCtl> v(5);
    auto& l1 = v[0];
    l1.level = 1;
    l1.reason = "steady queue backpressure";
    l1.value = [](const MetricsSnapshot& s) { return static_cast<float>(s.rq_steady_depth); };
    l1.enter_threshold = 32;
    l1.recover_threshold = 3;  // 水位 <3 持续 30s 逐级回升（详设 §3.9-①）
    l1.hold_ms = 5000;
    l1.recover_hold_ms = 30000;

    auto& l2 = v[1];
    l2.level = 2;
    l2.reason = "ddr bandwidth high";
    l2.value = [](const MetricsSnapshot& s) { return s.ddr_pct; };
    l2.enter_threshold = 70;
    l2.recover_threshold = 50;
    l2.hold_ms = 10000;
    l2.recover_hold_ms = 10000;

    auto& l3 = v[2];
    l3.level = 3;
    l3.reason = "npu overload, triggered paused";
    l3.value = [](const MetricsSnapshot& s) { return s.npu_load_pct; };
    l3.enter_threshold = 85;
    l3.recover_threshold = 60;
    l3.hold_ms = 3000;
    l3.recover_hold_ms = 5000;

    auto& l4 = v[3];
    l4.level = 4;
    l4.reason = "temperature high, all fps=1";
    l4.value = [](const MetricsSnapshot& s) { return s.npu_temp_c; };
    l4.enter_threshold = 85;
    l4.recover_threshold = 75;  // 恢复需 <75℃ 持续（任务分解 §4-T2.6）
    l4.hold_ms = 5000;
    l4.recover_hold_ms = 30000;

    auto& l5 = v[4];
    l5.level = 5;
    l5.reason = "disk watermark (businessd)";
    l5.value = [](const MetricsSnapshot& s) { return s.disk_high ? 100.f : 0.f; };
    l5.enter_threshold = 100;
    l5.recover_threshold = 0;
    l5.hold_ms = 0;
    l5.recover_hold_ms = 0;
    return v;
}

// ---- Monitor ----
void Monitor::start() {
    if (running_.exchange(true)) return;
    th_ = std::thread(&Monitor::loop, this);
}

void Monitor::stop() {
    if (!running_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

void Monitor::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        MetricsSnapshot s = src_->sample();
        if (on_snapshot_) on_snapshot_(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
    }
}

}  // namespace aibox
