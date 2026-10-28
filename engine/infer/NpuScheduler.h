// NpuScheduler —— 双队列多核调度器（任务分解 T2.1，详设 §3.6；NpuCorePlan 分核联动）
//
// steady 常驻：按物理模型分桶，桶间 DRR（赤字轮询，quantum = Σ通道 fps），
//              桶满丢最旧（newest-wins，架构 §5.2）——常驻检测只保新鲜帧；
//              worker 池 = plan.steady_cores.size()（默认 1），桶级 in-flight 保证同桶串行
// triggered 触发：优先级队列（pri 小者优先，同 pri FIFO），深度 ≥8 时按 pri 拒新/逐出最差
//                 （V1 固定单 worker——plan.triggered 多核仅首核生效，main 告警）
//
// 可测性设计：
//   - 执行体抽进 AlgoInstance（preproc+infer+post），x86 注入 FakeInfer 测纯时序
//   - Job.frame 允许为 nullptr（纯调度测试不构造像素）
//   - set_channel_fps 经回调联动通道 Sampler（回调由 main 装配注入，本类不反向依赖）
//
// 保序：同一桶（同通道同模型）在单 worker 内 FIFO 出队 + cb 串行执行 → 回调序==提交序；
//       cb 若递交线程池则序由池保证（M3），M2 直接在 worker 内同步执行
#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/DmaBuffer.h"
#include "common/EdgeMeter.h"
#include "common/NonCopyable.h"
#include "infer/AlgoInstance.h"
#include "interfaces/types.h"

namespace aibox {

struct Job {
    uint32_t cam_id = 0;
    int64_t pts = 0;  // us（透传至 detections.frame_pts，前端对齐锚）
    uint64_t frame_seq = 0;
    std::shared_ptr<DmaBuffer> frame;  // 持有引用至 cb 返回（fd 生命周期纪律）；测试可空
    LetterBoxParam lb;
    std::shared_ptr<AlgoInstance> algo;
    float conf_th = 0.f;  // 通道级阈值覆盖（set_algos 的 thr；<=0 用 post 闭包内阈值）
    uint8_t pri = 0;  // 触发队列: 0车牌/1人脸/2姿态/3分类（小者优先）
    std::function<void(std::vector<proto::DetTarget>&&)> cb;
    uint64_t submit_seq = 0;  // 全局提交序（同桶 FIFO 与同 pri 稳定性）
};

enum class SubmitResult { ACCEPTED, DROPPED_LOW_PRI, DROPPED_BACKPRESSURE };

class NpuScheduler : public NonCopyable {
public:
    using FpsCallback = std::function<void(uint32_t cam, uint8_t fps)>;

    NpuScheduler() = default;
    ~NpuScheduler();  // 自动 stop()

    // steady_workers：常驻 worker 数（main 按 NpuCorePlan.steady_cores.size() 传入；
    // 默认 1 = 历史双核行为，既有用例/部署零变化）。同桶串行由桶级 in-flight 保证，
    // 不同桶在多 worker 下真并行（ctx 已按 plan 绑核，架构 §7.3）
    void start(size_t steady_workers = 1);
    void stop();

    // 常驻：按 algo 分桶入队，桶满丢最旧，恒 ACCEPTED（丢弃已计 meter.drop）
    SubmitResult submit_steady(Job&& j);

    // 触发：深度≥8 时——新 job pri 优于队内最差者则逐出最差并准入；
    //       否则 DROPPED_LOW_PRI（trig_meter.drop++）
    SubmitResult submit_triggered(Job&& j);

    // 降级③：暂停 Core1 worker（队列仍可准入——详设 §3.6"队列仍有准入计数"），
    // 恢复后从队首继续消费
    void set_triggered_paused(bool p);

    // 降级①联动：main 装配时绑定 → CameraChannel::sampler_.configure
    void set_fps_callback(FpsCallback cb) { fps_cb_ = std::move(cb); }
    void set_channel_fps(uint32_t cam, uint8_t fps);

    const EdgeMeter& steady_meter() const { return steady_meter_; }
    const EdgeMeter& trig_meter() const { return trig_meter_; }

    size_t steady_depth();     // Σ桶深（Monitor 快照用）
    size_t triggered_depth();  // 触发队列深（降级①水位判定用）

private:
    struct Bucket {
        uint32_t weight = 1;  // Σ通道 fps（每次 submit 跟随 algo->weight 更新）
        uint32_t deficit = 0;  // DRR 赤字
        std::deque<Job> q;
        // 桶深上限 = ceil(weight × 0.5s)，防抖下限 2（详设 §3.6）
        uint32_t cap() const { return std::max<uint32_t>(2, (weight + 1) / 2); }
    };

    void steady_loop();
    // 需已持 mu_：DRR 轮转选一个可服务桶（非空且不在飞），按赤字整批弹出并认领（busy_）
    bool pick_locked(std::string& key, std::vector<Job>& batch);
    void triggered_loop();
    void execute(Job& j);  // preproc→infer(计延迟)→post→cb，worker 线程内串行

    std::mutex mu_;
    std::condition_variable cv_;
    bool running_ = false;
    bool paused_ = false;
    // std::map：稳定桶序是轮转起点（rr_）公平性的前提，unordered 迭代序不定会导致首桶独占
    std::map<std::string, Bucket> buckets_;  // key = algo_id（物理模型）
    std::deque<Job> rq_;  // 触发队列（pri 升序稳定：同 pri 按 submit_seq FIFO）
    static constexpr size_t kTrigCap = 8;

    std::vector<std::thread> steady_ths_;
    std::thread trig_th_;
    std::set<std::string> busy_;  // 在飞桶（algo_id）：认领期间他 worker 不得触碰同 ctx
    size_t rr_ = 0;               // DRR 轮转起点（防首桶独占致他桶饥饿）
    EdgeMeter steady_meter_, trig_meter_;
    FpsCallback fps_cb_;
    std::atomic<uint64_t> submit_seq_{0};
};

}  // namespace aibox
