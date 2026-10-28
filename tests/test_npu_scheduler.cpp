// T2.1 单测（任务分解 §4-T2.1 五组用例）
// DRR 公平性 / 触发准入 / 优先级 / 降级联动 / 同桶保序
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "infer/NpuScheduler.h"
#include "media/Sampler.h"
#include "test_util.h"

using namespace aibox;

namespace {

// 可注入耗时的假推理引擎（公平性测试需要假负载来维持饱和）
class SleepInfer : public IInferEngine {
public:
    explicit SleepInfer(int sleep_ms) : sleep_ms_(sleep_ms) {}
    bool load(const std::string&) override { return true; }
    bool run(const InferTensor&, std::vector<std::vector<float>>& outputs) override {
        if (sleep_ms_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_));
        outputs.assign(1, {});  // 空输出：调度测试不关心内容
        return true;
    }

private:
    int sleep_ms_;
};

std::shared_ptr<AlgoInstance> make_algo(const std::string& id, uint32_t weight, int sleep_ms = 0) {
    auto a = std::make_shared<AlgoInstance>();
    a->algo_id = id;
    a->weight = weight;
    a->infer = std::make_shared<SleepInfer>(sleep_ms);
    a->post = [](const std::vector<std::vector<float>>&, const LetterBoxParam&) {
        return std::vector<proto::DetTarget>{};
    };
    return a;
}

Job make_job(std::shared_ptr<AlgoInstance> algo, uint8_t pri,
             std::function<void(std::vector<proto::DetTarget>&&)> cb) {
    Job j;
    j.cam_id = 1;
    j.algo = std::move(algo);
    j.pri = pri;
    j.cb = std::move(cb);
    return j;
}

}  // namespace

int main() {
    // ---- 1. DRR 公平性：桶A权重24 / 桶B权重6，双端饱和 1.5s → 服务比 ∈[3.8,4.2] ----
    {
        NpuScheduler s;
        s.start();
        auto algo_a = make_algo("yolo", 24, 1);  // 1ms 假负载维持饱和
        auto algo_b = make_algo("face", 6, 1);
        std::atomic<uint64_t> exec_a{0}, exec_b{0};
        std::atomic<bool> stop{false};
        auto producer = [&](std::shared_ptr<AlgoInstance> algo,
                            std::atomic<uint64_t>& exec) {
            while (!stop.load(std::memory_order_relaxed)) {
                Job j = make_job(algo, 0, [&](std::vector<proto::DetTarget>&&) {
                    exec.fetch_add(1, std::memory_order_relaxed);
                });
                s.submit_steady(std::move(j));
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        };
        std::thread ta(producer, algo_a, std::ref(exec_a));
        std::thread tb(producer, algo_b, std::ref(exec_b));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        // 饱和窗口结束瞬间快照（排空残余前，此时双桶仍饱和，比例才有效）
        double a = static_cast<double>(exec_a.load());
        double b = static_cast<double>(exec_b.load());
        stop.store(true);
        ta.join();
        tb.join();
        CHECK(a > 100 && b > 25);  // 样本量足够才有比例意义
        double ratio = a / b;
        CHECK_MSG(ratio >= 3.8 && ratio <= 4.2, "DRR 24:6 ratio=%f", ratio);

        // 排空后守恒：executed == accepted == in - drop（newest-wins 丢的永不执行）
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(s.steady_depth() == 0);
        auto cs = snapshot(s.steady_meter());
        uint64_t accepted = cs.in - cs.drop;
        CHECK(exec_a.load() + exec_b.load() == accepted);
        s.stop();
    }

    // ---- 2. 准入：触发队列填 8 后再 submit（pri 不优于队内最差）→ DROPPED_LOW_PRI ----
    {
        NpuScheduler s;
        s.start();
        auto algo = make_algo("plate", 2);
        s.set_triggered_paused(true);  // 暂停 worker：队列只进不出（准入仍开放）
        for (uint8_t pri = 0; pri < 8; ++pri) {
            auto r = s.submit_triggered(make_job(algo, pri, nullptr));
            CHECK(r == SubmitResult::ACCEPTED);
        }
        CHECK(s.triggered_depth() == 8);
        auto c0 = snapshot(s.trig_meter());
        // 队内 pri 0..7（队尾=7）：pri=7 不优于最差 → 拒；若提交 pri=3 则 3<7
        // 会逐出队尾反而被接受——准入语义是"优才进"，不是"占位"
        auto r = s.submit_triggered(make_job(algo, 7, nullptr));
        CHECK(r == SubmitResult::DROPPED_LOW_PRI);
        auto c1 = snapshot(s.trig_meter());
        CHECK(c1.drop == c0.drop + 1);  // 计数+1
        CHECK(s.triggered_depth() == 8);
        s.set_triggered_paused(false);
        s.stop();
    }

    // ---- 3. 优先级：混入 pri 0~3，出队顺序严格按 pri 升序（同 pri FIFO） ----
    {
        NpuScheduler s;
        s.start();
        auto algo = make_algo("face", 2);
        s.set_triggered_paused(true);
        const uint8_t pris[] = {3, 1, 0, 2, 3, 1, 0};
        std::vector<uint8_t> got;
        std::mutex mu;
        for (uint8_t p : pris) {
            Job j = make_job(algo, p, nullptr);
            j.cb = [&, p](std::vector<proto::DetTarget>&&) {
                std::lock_guard<std::mutex> lk(mu);
                got.push_back(p);
            };
            CHECK(s.submit_triggered(std::move(j)) == SubmitResult::ACCEPTED);
        }
        s.set_triggered_paused(false);  // 恢复消费：worker 从队首按 pri 升序取
        for (int i = 0; i < 1000 && got.size() < 7; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lk(mu);
        CHECK(got.size() == 7);
        for (size_t i = 1; i < got.size(); ++i) CHECK(got[i - 1] <= got[i]);
        s.stop();
    }

    // ---- 4. 降级联动：set_channel_fps → 回调改通道 Sampler 原子值 ----
    {
        NpuScheduler s;
        Sampler ch_sampler(3);
        uint32_t got_cam = 0;
        s.set_fps_callback([&](uint32_t cam, uint8_t fps) {
            got_cam = cam;
            ch_sampler.configure(fps);
        });
        s.set_channel_fps(7, 1);
        CHECK(got_cam == 7);
        CHECK(ch_sampler.fps() == 1);  // Sampler 原子值已变更
        s.set_channel_fps(7, 3);
        CHECK(ch_sampler.fps() == 3);
    }

    // ---- 5. 保序：同通道 100 job（weight 大到永不丢帧），回调序==提交序 ----
    {
        NpuScheduler s;
        s.start();
        auto algo = make_algo("yolo", 201, 0);  // cap=101 > 100，无丢帧
        std::mutex mu;
        std::vector<uint64_t> seqs;
        for (uint64_t i = 0; i < 100; ++i) {
            Job j = make_job(algo, 0, nullptr);
            j.cb = [&, i](std::vector<proto::DetTarget>&&) {
                std::lock_guard<std::mutex> lk(mu);
                seqs.push_back(i);
            };
            CHECK(s.submit_steady(std::move(j)) == SubmitResult::ACCEPTED);
        }
        for (int i = 0; i < 2000 && seqs.size() < 100; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard<std::mutex> lk(mu);
        CHECK(seqs.size() == 100);
        for (size_t i = 0; i < seqs.size(); ++i) CHECK(seqs[i] == i);  // 严格保序
        auto cs = snapshot(s.steady_meter());
        CHECK(cs.drop == 0);  // 量好桶深就不会有 newest-wins 丢帧
        s.stop();
    }

    // ---- 6. 多 worker（NpuCorePlan 分核）：同桶串行不重叠 + 各桶保序 + 守恒 ----
    {
        NpuScheduler s;
        s.start(2);  // steady 双 worker：两桶可被并行认领（板上 = steady 物理核数）
        auto algo_a = make_algo("yolo", 201, 0);  // cap=101：100 job 无丢帧
        auto algo_b = make_algo("face", 201, 0);
        std::mutex mu;
        std::vector<uint64_t> seq_a, seq_b;
        std::atomic<int> fly_a{0}, fly_b{0};
        std::atomic<uint64_t> overlap{0};
        auto submit = [&](std::shared_ptr<AlgoInstance> algo, std::atomic<int>& fly,
                          std::vector<uint64_t>& sink, uint64_t i) {
            Job j = make_job(algo, 0, nullptr);
            j.cb = [&, i](std::vector<proto::DetTarget>&&) {
                // 同桶重叠检测：busy_ 认领保证任意时刻同桶至多 1 在飞（同 ctx 纪律）
                if (fly.fetch_add(1, std::memory_order_relaxed) != 0)
                    overlap.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                fly.fetch_sub(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(mu);
                sink.push_back(i);
            };
            CHECK(s.submit_steady(std::move(j)) == SubmitResult::ACCEPTED);
        };
        for (uint64_t i = 0; i < 100; ++i) submit(algo_a, fly_a, seq_a, i);
        for (uint64_t i = 0; i < 100; ++i) submit(algo_b, fly_b, seq_b, i);
        for (int i = 0; i < 4000 && (seq_a.size() < 100 || seq_b.size() < 100); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        {
            std::lock_guard<std::mutex> lk(mu);
            CHECK(seq_a.size() == 100);
            CHECK(seq_b.size() == 100);
            for (size_t i = 0; i < seq_a.size(); ++i) CHECK(seq_a[i] == i);  // 同桶严格保序
            for (size_t i = 0; i < seq_b.size(); ++i) CHECK(seq_b[i] == i);
        }
        CHECK(overlap.load() == 0);  // 同桶（同 ctx）零并发
        auto cs = snapshot(s.steady_meter());
        CHECK(cs.drop == 0);  // 量大桶深 → newest-wins 不触发
        s.stop();
    }

    if (g_fail == 0) printf("npu_scheduler PASSED\n");
    return g_fail;
}