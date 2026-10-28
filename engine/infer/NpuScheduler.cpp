#include "infer/NpuScheduler.h"

#include "common/Log.h"

#include <algorithm>
#include <chrono>

namespace aibox {

namespace {
inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

NpuScheduler::~NpuScheduler() { stop(); }

void NpuScheduler::start(size_t steady_workers) {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    running_ = true;
    paused_ = false;
    // steady worker 池：数量由 main 按 NpuCorePlan.steady_cores.size() 决定（默认 1 =
    // 历史双核行为）。桶级 in-flight（busy_）保证同桶串行 → 同 ctx 不并发、同通道保序；
    // 不同桶被不同 worker 认领 → 多核真并行（ctx 已按 plan 绑核，架构 §7.3）
    if (steady_workers < 1) steady_workers = 1;
    for (size_t i = 0; i < steady_workers; ++i)
        steady_ths_.emplace_back(&NpuScheduler::steady_loop, this);
    trig_th_ = std::thread(&NpuScheduler::triggered_loop, this);
}

void NpuScheduler::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    for (auto& t : steady_ths_)
        if (t.joinable()) t.join();
    steady_ths_.clear();
    if (trig_th_.joinable()) trig_th_.join();
    // 在途 Job 随桶/队列析构：frame 引用归零 → 自动回 FramePool（生命周期纪律）
}

SubmitResult NpuScheduler::submit_steady(Job&& j) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return SubmitResult::DROPPED_BACKPRESSURE;  // 未启动/停止中不收
    steady_meter_.in.fetch_add(1, std::memory_order_relaxed);

    Bucket& b = buckets_[j.algo->algo_id];
    b.weight = std::max<uint32_t>(1, j.algo->weight);  // 跟随 set_algos 动态更新
    if (b.q.size() >= b.cap()) {
        b.q.pop_front();  // newest-wins：保新鲜帧，牺牲旧帧（架构 §5.2）
        steady_meter_.drop.fetch_add(1, std::memory_order_relaxed);
    }
    j.submit_seq = submit_seq_.fetch_add(1, std::memory_order_relaxed);
    b.q.push_back(std::move(j));
    cv_.notify_all();  // 多 worker：广播（等待者竞争认领；认领空手者回 wait 重查）
    return SubmitResult::ACCEPTED;
}

SubmitResult NpuScheduler::submit_triggered(Job&& j) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return SubmitResult::DROPPED_BACKPRESSURE;
    trig_meter_.in.fetch_add(1, std::memory_order_relaxed);

    if (rq_.size() >= kTrigCap) {
        // 队列按 pri 升序稳定 → back() 即最差（pri 最大、同 pri 最晚）
        if (!rq_.empty() && j.pri < rq_.back().pri) {
            rq_.pop_back();  // 逐出最差，高优插队（详设 §3.6"深度>8 按 pri 拒新"）
            trig_meter_.drop.fetch_add(1, std::memory_order_relaxed);
        } else {
            trig_meter_.drop.fetch_add(1, std::memory_order_relaxed);
            return SubmitResult::DROPPED_LOW_PRI;
        }
    }
    j.submit_seq = submit_seq_.fetch_add(1, std::memory_order_relaxed);
    // 稳定插入：pri 升序、同 pri 保持 FIFO（队列 ≤8，线性找位成本可忽略）
    size_t i = rq_.size();
    while (i > 0 && rq_[i - 1].pri > j.pri) --i;
    rq_.insert(rq_.begin() + static_cast<long>(i), std::move(j));
    cv_.notify_one();
    return SubmitResult::ACCEPTED;
}

void NpuScheduler::set_triggered_paused(bool p) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        paused_ = p;
    }
    cv_.notify_all();
}

void NpuScheduler::set_channel_fps(uint32_t cam, uint8_t fps) {
    if (fps_cb_) fps_cb_(cam, fps);  // main 装配绑定 Sampler；调度器不反向依赖通道
}

size_t NpuScheduler::steady_depth() {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& kv : buckets_) n += kv.second.q.size();
    return n;
}

size_t NpuScheduler::triggered_depth() {
    std::lock_guard<std::mutex> lk(mu_);
    return rq_.size();
}

// 需已持 mu_。DRR 轮转选桶+整批认领：从 rr_ 起按稳定桶序（std::map）扫描，非空且不在飞
// 的桶补赤字并按 DRR 弹出整批服务（权重退化相等 → round-robin，详设 §3.6 注）；
// 饥饿桶赤字清零：防止空转期攒出巨额赤字，恢复后突发挤占他桶。
// 认领（busy_）后同桶他 worker 不可见 → 同 ctx 串行、同通道保序；桶间并行。
bool NpuScheduler::pick_locked(std::string& key, std::vector<Job>& batch) {
    if (buckets_.empty()) return false;
    const size_t n = buckets_.size();
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = (rr_ + i) % n;
        auto it = buckets_.begin();
        std::advance(it, static_cast<long>(idx));
        Bucket& b = it->second;
        if (b.q.empty()) {
            b.deficit = 0;
            continue;
        }
        if (busy_.count(it->first)) continue;  // 在飞桶：同 ctx 串行纪律，本轮跳过
        b.deficit += b.weight;
        while (b.deficit >= 1 && !b.q.empty()) {
            batch.push_back(std::move(b.q.front()));
            b.q.pop_front();
            b.deficit -= 1;
            steady_meter_.out.fetch_add(1, std::memory_order_relaxed);
        }
        if (!batch.empty()) {
            key = it->first;
            busy_.insert(key);
            rr_ = (idx + 1) % n;  // 下轮从下一桶起（轮转防首桶独占致他桶饥饿）
            return true;
        }
    }
    return false;
}

void NpuScheduler::steady_loop() {
    std::vector<Job> batch;
    batch.reserve(32);
    while (true) {
        std::string key;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] {
                if (!running_) return true;
                for (auto& kv : buckets_)
                    if (!kv.second.q.empty() && !busy_.count(kv.first)) return true;
                return false;
            });
            if (!running_) break;
            batch.clear();
            if (!pick_locked(key, batch)) continue;  // 谓词与认领同锁判定，理论不可达
        }
        for (auto& j : batch) execute(j);  // 锁外执行；j 析构 → frame 归还池
        batch.clear();
        {
            std::lock_guard<std::mutex> lk(mu_);
            busy_.erase(key);
        }
        cv_.notify_all();  // 释放认领：唤醒竞争同桶或等新 job 的 worker
    }
}

void NpuScheduler::triggered_loop() {
    while (true) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !running_ || (!paused_ && !rq_.empty()); });
            if (!running_) break;
            if (paused_ || rq_.empty()) continue;  // 降级③：worker 睡眠，队列保留
            j = std::move(rq_.front());
            rq_.pop_front();
            trig_meter_.out.fetch_add(1, std::memory_order_relaxed);
        }
        execute(j);  // 无分桶，pri 小者先（入队时已稳定排序）
    }
}

void NpuScheduler::execute(Job& j) {
    InferTensor t;
    t.width = j.algo->in_w;
    t.height = j.algo->in_h;
    t.data.resize(static_cast<size_t>(j.algo->in_w) * j.algo->in_h * 3);

    // 预处理/推理失败诊断（板上排障关键：静默空 targets 无法定位，频控防刷屏）
    static std::atomic<uint32_t> pre_fail{0}, infer_fail{0};
    bool pre_ok = false;
    // M2 CPU 路径：DmaBuffer(map) → Frame(ptr) 适配 preproc。
    // M3 板上：RgaPreproc 直接吃 fd（此处替换为 fd 通道适配，map 不再参与）。
    if (j.frame && j.frame->map && j.algo->preproc) {
        Frame f;
        f.pts_ms = j.frame->pts;
        f.width = j.frame->w;
        f.height = j.frame->h;
        f.stride_y = j.frame->w;
        f.stride_uv = j.frame->w;
        f.y_fd = j.frame->fd;
        f.uv_fd = -1;  // NV12 单 fd，UV 偏移由 hw 层解析（M3）
        auto* base = static_cast<const uint8_t*>(j.frame->map);
        f.y_ptr = base;
        f.uv_ptr = base + static_cast<size_t>(j.frame->w) * j.frame->h;
        pre_ok = j.algo->preproc->run(f, t, j.lb);
        if (!pre_ok) {
            uint32_t n = pre_fail.fetch_add(1, std::memory_order_relaxed);
            if (n < 3 || (n % 100) == 0)
                LOGW("sched", "preproc fail (n=%u) cam=%u seq=%llu", n, j.cam_id,
                     static_cast<unsigned long long>(j.frame_seq));
        }
    }

    std::vector<std::vector<float>> outs;
    uint64_t t0 = now_us();
    bool infer_ok = j.algo->infer->run(t, outs);
    uint64_t t1 = now_us();
    if (!infer_ok) {
        uint32_t n = infer_fail.fetch_add(1, std::memory_order_relaxed);
        if (n < 3 || (n % 100) == 0)
            LOGW("sched", "infer fail (n=%u) cam=%u", n, j.cam_id);
    }
    // npu_cost_ms 口径（demo 实测同款）：只测推理段，不含预处理/后处理
    steady_meter_.add_latency_us(t1 - t0);

    auto targets =
        j.algo->post ? j.algo->post(outs, j.lb) : AlgoInstance::default_post(outs, j.lb);
    // 排障探针（频控 1/50）：raw/post 阈值/过滤后数量 + raw 最大 conf——
    // 直接观测通道阈值是否到达 Job、manifest 阈值是多少、过滤前后差
    static std::atomic<uint32_t> probe_n{0};
    if (probe_n.fetch_add(1, std::memory_order_relaxed) % 50 == 0) {
        float max_conf = 0.f;
        for (const auto& t : targets) max_conf = std::max(max_conf, t.conf);
        LOGI("sched", "probe cam=%u algo=%s thr=%.2f raw=%zu max_conf=%.2f", j.cam_id,
             j.algo->algo_id.c_str(), j.conf_th, targets.size(), max_conf);
    }
    // 通道级阈值覆盖：post 闭包阈值取 manifest（构造期），set_algos 的 thr 在此
    // 生效——>0 时滤除低于通道阈值的检测（板上实证：配置页调 thr 不生效的根因）
    size_t before = targets.size();
    if (j.conf_th > 0.f) {
        std::vector<proto::DetTarget> kept;
        kept.reserve(targets.size());
        for (auto& t : targets)
            if (t.conf >= j.conf_th) kept.push_back(std::move(t));
        targets = std::move(kept);
    }
    if (targets.size() != before) {
        static std::atomic<uint32_t> filt_n{0};
        if (filt_n.fetch_add(1, std::memory_order_relaxed) % 50 == 0)
            LOGI("sched", "conf filter cam=%u %.2f: %zu -> %zu", j.cam_id, j.conf_th,
                 before, targets.size());
    }
    if (j.cb) j.cb(std::move(targets));
    // j.frame 引用随 Job 析构归零 → 回 FramePool；cb 内不得持有 frame（详设 §3.6）
}

}  // namespace aibox
