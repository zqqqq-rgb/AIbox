// T2.6 单测（任务分解 §4-T2.6）：温度抖动滞回 + 五级默认表 + Monitor 采样节拍
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>

#include "arbiter/DegradeController.h"
#include "test_util.h"

using namespace aibox;

namespace {

MetricsSnapshot snap(int64_t ts_ms, float temp, float load = 0, float ddr = 0,
                     uint32_t depth = 0, bool disk = false) {
    MetricsSnapshot s;
    s.ts_ms = ts_ms;
    s.npu_temp_c = temp;
    s.npu_load_pct = load;
    s.ddr_pct = ddr;
    s.rq_steady_depth = depth;
    s.disk_high = disk;
    return s;
}

}  // namespace

int main() {
    // ---- 1. 温度 84→86→84→86 抖动注入：④ 只进入一次；恢复需 <75℃ 持续 30s ----
    {
        auto levels = DegradeController::default_levels();
        // 只保留 ④ 温度级，隔离判定
        levels.erase(std::remove_if(levels.begin(), levels.end(),
                                    [](const DegradeController::LevelCtl& l) {
                                        return l.level != 4;
                                    }),
                     levels.end());
        int enters = 0, exits = 0;
        levels[0].action = [&](bool enter) { enter ? ++enters : ++exits; };

        DegradeController dc(std::move(levels));
        // 抖动序列（hold=5s 内不连续满足 → 不进入）
        dc.on_snapshot(snap(0, 84));
        dc.on_snapshot(snap(1000, 86));   // 进入计时起点
        dc.on_snapshot(snap(2000, 84));   // 抖动 → 计时归零
        dc.on_snapshot(snap(3000, 86));
        dc.on_snapshot(snap(4000, 84));
        dc.on_snapshot(snap(5000, 86));
        CHECK(enters == 0);  // 从未连续 5s → 未进入（防抖生效）
        CHECK(dc.level() == 0);

        // 连续 86 持续 5s → 进入一次
        dc.on_snapshot(snap(6000, 86));
        dc.on_snapshot(snap(8000, 86));
        dc.on_snapshot(snap(10000, 86));
        CHECK(enters == 1);
        CHECK(dc.level() == 4);

        // 滞回带内（75~85）：84 抖动 8→9→10s，不退出也不重复进入
        dc.on_snapshot(snap(11000, 84));
        dc.on_snapshot(snap(12000, 86));
        dc.on_snapshot(snap(13000, 84));
        CHECK(enters == 1);  // "只进入一次"
        CHECK(exits == 0);
        CHECK(dc.level() == 4);

        // <75 但不足 30s → 不退出
        dc.on_snapshot(snap(20000, 74));
        dc.on_snapshot(snap(40000, 74));  // 持续 20s < 30s
        CHECK(exits == 0);
        CHECK(dc.level() == 4);

        // <75 持续 ≥30s（20s→55s 跨 35s）→ 退出
        dc.on_snapshot(snap(55000, 74));
        CHECK(exits == 1);
        CHECK(dc.level() == 0);

        // 退出后 84（<85 不满足进入）→ 不再进入；86 持续 5s → 第二次进入
        dc.on_snapshot(snap(60000, 84));
        dc.on_snapshot(snap(65000, 86));
        dc.on_snapshot(snap(71000, 86));
        CHECK(enters == 2);  // 重新武装后可再次进入（正常阶梯行为）
    }

    // ---- 2. 五级整体 level = max(活跃级)，变化回调（degrade 事件源） ----
    {
        DegradeController dc(DegradeController::default_levels());
        std::atomic<uint8_t> last_evt{255};
        dc.set_on_change([&](const proto::DegradeEvt& e) { last_evt.store(e.level); });

        // 时间线（虚拟时钟静息断言；各级时窗：③ enter 3s/rec 5s，
        // ② enter 10s/rec 10s，① enter 5s/rec 30s，⑤ hold 0）
        dc.on_snapshot(snap(0, 30, 90, 40, 0));     // ③ NPU 90% 开始计时
        dc.on_snapshot(snap(3500, 30, 90, 40, 0));  // ≥3s → ③ 进入
        CHECK(dc.level() == 3);
        CHECK(last_evt.load() == 3);

        dc.on_snapshot(snap(4000, 30, 90, 80, 0));   // ② DDR 80% 开始计时
        dc.on_snapshot(snap(15000, 30, 90, 80, 0));  // ≥10s → ② 进入
        CHECK(dc.level() == 3);  // ②③ 同活跃 → max=3

        dc.on_snapshot(snap(16000, 30, 50, 80, 0));  // ③ ≤60 恢复计时
        dc.on_snapshot(snap(22000, 30, 50, 80, 0));  // ≥5s → ③ 退出
        CHECK(dc.level() == 2);  // 只剩 ②

        dc.on_snapshot(snap(23000, 30, 50, 40, 40));  // ① 40>32 计时；② 40≤50 恢复计时
        dc.on_snapshot(snap(29000, 30, 50, 40, 40));  // ① ≥5s 进入；② 恢复仅 6s<10s 仍活跃
        CHECK(dc.level() == 2);  // max(①,②)=2

        dc.on_snapshot(snap(34000, 30, 50, 40, 40));  // ② 恢复满 10s → 退出
        CHECK(dc.level() == 1);  // 只剩 ①

        // ⑤ 磁盘外部置位（立即生效，hold=0）
        dc.on_snapshot(snap(35000, 30, 50, 40, 40, true));
        CHECK(dc.level() == 5);

        dc.on_snapshot(snap(36000, 30, 50, 40, 0, false));  // ⑤ 退出；① 0≤3 恢复计时
        CHECK(dc.level() == 1);  // ① 恢复需保持 30s
        dc.on_snapshot(snap(67000, 30, 50, 40, 0, false));  // ≥30s → ① 退出
        CHECK(dc.level() == 0);  // 全恢复
        CHECK(last_evt.load() == 0);
    }

    // ---- 3. Monitor：注入源，500ms 间隔采样（1.2s ≥ 2 次） ----
    {
        struct FakeSrc : IMetricsSource {
            std::atomic<int> calls{0};
            MetricsSnapshot sample() override {
                calls.fetch_add(1);
                MetricsSnapshot s;
                s.ts_ms = calls.load() * 500;
                s.npu_temp_c = 40;
                return s;
            }
        };
        auto src = std::make_shared<FakeSrc>();
        Monitor m(src, 100);  // 100ms 间隔加速测试
        std::atomic<int> got{0};
        m.set_on_snapshot([&](const MetricsSnapshot&) { got.fetch_add(1); });
        m.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        m.stop();
        CHECK(src->calls.load() >= 4);
        CHECK(got.load() == src->calls.load());  // 每次采样都送达订阅者
    }

    if (g_fail == 0) printf("degrade PASSED\n");
    return g_fail;
}
