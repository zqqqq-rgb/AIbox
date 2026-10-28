// Monitor / DegradeController —— 资源仲裁与五级降级（任务分解 T2.6，详设 §3.9，架构 §8.2）
//
// DegradeController：五级阶梯，每级独立"触发保持/恢复保持"滞回状态机
//   ① 常驻队列水位 > 阈值 持续 5s  → 逐通道降 fps（action→set_channel_fps）
//   ② DDR > 70% 持续 10s           → VOP 抽帧关闭（预览抽帧前端处理，V1 简化）
//   ③ NPU 负载 > 85%               → 触发推理暂停（set_triggered_paused）
//   ④ 温度 > 85℃ 持续 5s           → 全通道 fps=1 + cpufreq 限频
//   ⑤ 磁盘水位（businessd 外部置位） → 事件限流
//   恢复条件比进入更苛刻（如 ④ 恢复需 <75℃ 持续 30s）——滞回防抖动
//
// 时间基准：on_snapshot 使用快照自带的 ts_ms（测试注入虚拟时钟，板上 Monitor 填墙钟）
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/NonCopyable.h"
#include "ipc/Protocol.h"

namespace aibox {

// 一次采样快照（Monitor 每 500ms 汇总；字段含义见架构 §8.2）
struct MetricsSnapshot {
    int64_t ts_ms = 0;
    float npu_load_pct = 0;    // 三核峰值（sysfs）
    float ddr_pct = 0;         // DDR 带宽占用（DFI）
    float npu_temp_c = 25;     // NPU 温度
    uint32_t rq_steady_depth = 0;  // 常驻队列总深度（Σ桶）
    bool disk_high = false;    // 磁盘水位（businessd 经 UDS 置位，engine 只响应）
};

// 指标源接缝：板上实现读 sysfs/DFI（M3 hw 层）；x86 测试注入合成序列
struct IMetricsSource {
    virtual ~IMetricsSource() = default;
    virtual MetricsSnapshot sample() = 0;
};

// ---- DegradeController ----
class DegradeController : public NonCopyable {
public:
    struct LevelCtl {
        uint8_t level = 1;
        std::string reason;
        // 指标取值器 + 双阈值（进入 ≥ enter，恢复 ≤ recover，recover < enter 构成滞回带）
        std::function<float(const MetricsSnapshot&)> value;
        float enter_threshold = 0, recover_threshold = 0;
        uint32_t hold_ms = 0, recover_hold_ms = 0;
        std::function<void(bool enter)> action;
    };

    explicit DegradeController(std::vector<LevelCtl> levels);

    // 每次快照驱动各级独立计时判定；整体 level = max(活跃级)，
    // 变化时回调 on_change（main 装配 → UDS degrade 事件）
    void on_snapshot(const MetricsSnapshot& s);

    uint8_t level() const { return cur_level_.load(std::memory_order_relaxed); }
    void set_on_change(std::function<void(const proto::DegradeEvt&)> cb) {
        on_change_ = std::move(cb);
    }

    // 设计默认五级（架构 §8.2 阈值/保持时间定稿）；action 由 main 按需绑定
    static std::vector<LevelCtl> default_levels();

private:
    struct LevelState {
        bool active = false;
        int64_t cond_since_ms = -1;  // 进入条件持续起点
        int64_t rec_since_ms = -1;   // 恢复条件持续起点
    };

    std::vector<LevelCtl> levels_;
    std::vector<LevelState> st_;
    std::atomic<uint8_t> cur_level_{0};
    std::function<void(const proto::DegradeEvt&)> on_change_;
};

// ---- Monitor：500ms 采样线程 ----
class Monitor : public NonCopyable {
public:
    Monitor(std::shared_ptr<IMetricsSource> src, uint32_t interval_ms = 500)
        : src_(std::move(src)), interval_ms_(interval_ms) {}
    ~Monitor() { stop(); }

    void start();
    void stop();
    void set_on_snapshot(std::function<void(const MetricsSnapshot&)> cb) {
        on_snapshot_ = std::move(cb);
    }

private:
    void loop();

    std::shared_ptr<IMetricsSource> src_;
    uint32_t interval_ms_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::function<void(const MetricsSnapshot&)> on_snapshot_;
};

}  // namespace aibox
