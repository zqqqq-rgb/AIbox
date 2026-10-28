// T2.2 单测（任务分解 §4-T2.2）：迁移表逐行 + RECONFIG 计时与隔离 + 看门狗恰好一次
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "core/fakes/file_decoder.h"
#include "media/CameraChannel.h"
#include "test_util.h"

using namespace aibox;

namespace {

constexpr uint32_t kW = 64, kH = 48;  // 小帧加速拷贝
constexpr size_t kNv12 = static_cast<size_t>(kW) * kH * 3 / 2;

// 可脚本化的假源：
//   第 1 次 open 后可读 grant 个包；此后每次 open 后可读 reopen_grant 个（<0=无限）
//   hang_when_empty: 包耗尽后 read 阻塞（模拟"拉流会话僵死但 TCP 未断"——
//                    看门狗 close() 是唯一解法），close 后 read 返回 false
class ScriptSource : public ISource {
public:
    ScriptSource(int grant, int pkt_ms, int reopen_grant, bool hang_when_empty)
        : grant_(grant), pkt_ms_(pkt_ms), reopen_grant_(reopen_grant), hang_(hang_when_empty) {}

    bool open() override {
        std::lock_guard<std::mutex> lk(mu_);
        ++open_count_;
        grant_left_ = (open_count_ == 1) ? grant_ : reopen_grant_;
        closed_ = false;
        return true;
    }

    bool read(Packet& out) override {
        bool got = false;
        while (true) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (closed_) return false;
                if (grant_left_ != 0) {
                    if (grant_left_ > 0) --grant_left_;
                    pts_ += pkt_ms_;
                    out.data = es_.data();
                    out.size = static_cast<uint32_t>(es_.size());
                    out.pts_ms = static_cast<uint64_t>(pts_);
                    got = true;
                    break;
                }
            }
            if (!hang_) return false;  // EOF：调用方决定重连节奏
            std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 僵死轮询
        }
        // 帧间真实节奏：pts 推进 pkt_ms，墙钟也必须隔 pkt_ms——否则 N 帧瞬间耗尽，
        // 看门狗时间线与"源在持续出帧"的前提不符
        std::this_thread::sleep_for(std::chrono::milliseconds(pkt_ms_));
        return got;
    }

    void close() override {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }

    int opens() const {
        std::lock_guard<std::mutex> lk(mu_);
        return open_count_;
    }

private:
    std::vector<uint8_t> es_ = std::vector<uint8_t>(kNv12, 0x80);  // 伪 ES（NV12 大小）
    mutable std::mutex mu_;
    int grant_, pkt_ms_, reopen_grant_;
    bool hang_;
    int open_count_ = 0;
    int grant_left_ = 0;
    int64_t pts_ = 0;
    bool closed_ = false;
};

std::unique_ptr<ISource> mk_factory_holder(std::shared_ptr<ScriptSource>* holder, int grant,
                                           int pkt_ms, int reopen_grant, bool hang) {
    // 工厂创建并记入 holder（非拥有观察句柄：unique_ptr 仍是唯一所有者，
    // 测试只借 opens() 观察；通道 stop() 必须先于测试作用域结束——各用例已满足）
    auto s = std::make_unique<ScriptSource>(grant, pkt_ms, reopen_grant, hang);
    if (holder) {
        *holder = std::shared_ptr<ScriptSource>(s.get(), [](ScriptSource*) {});
    }
    return s;
}

CameraChannel::SourceFactory mk_factory(std::shared_ptr<ScriptSource>* holder, int grant,
                                        int pkt_ms, int reopen_grant, bool hang) {
    return [holder, grant, pkt_ms, reopen_grant, hang](const std::string&) {
        return mk_factory_holder(holder, grant, pkt_ms, reopen_grant, hang);
    };
}

CameraChannel::DecoderFactory mk_decoder() {
    return [] { return std::make_unique<FileDecoder>(); };
}

CameraChannel::Params fast_params() {
    CameraChannel::Params p;
    p.pool_depth = 4;
    p.frame_w = kW;
    p.frame_h = kH;
    p.watchdog_tick_ms = 50;
    p.watchdog_timeout_ms = 200;
    p.reconnect_backoff_ms = 50;
    return p;
}

}  // namespace

int main() {
    // ---- 1a. INIT→LOADING：start 后、首帧前 ----
    // （首帧间隔 30ms，LOADING 窗口极短，用 hang 源保证稳定观察）
    {
        std::shared_ptr<ScriptSource> src;
        CameraChannel ch(1, "fake://", mk_factory(&src, 0, 30, 100, true), mk_decoder(),
                         fast_params());
        CHECK(ch.state() == CameraChannel::State::INIT);
        ch.start();
        CHECK(ch.state() == CameraChannel::State::LOADING);
        // 无帧 → 不迁移 RUNNING；看门狗会在超时后重启（此处不断言，用例 3 专测）
        ch.stop();
        CHECK(ch.state() == CameraChannel::State::STOPPED);
    }

    // ---- 1b. LOADING→RUNNING（首帧）+ 帧流 + 不变量 + stop 收尾 ----
    {
        std::shared_ptr<ScriptSource> src;
        CameraChannel ch(2, "fake://", mk_factory(&src, -1, 30, -1, false), mk_decoder(),
                         fast_params());
        std::atomic<int> sink_n{0};
        std::atomic<uint64_t> last_seq{0};
        ch.set_frame_sink([&](std::shared_ptr<DmaBuffer> buf) {
            if (buf->frame_seq > 0) {
                uint64_t prev = last_seq.load();
                while (buf->frame_seq > prev &&
                       !last_seq.compare_exchange_weak(prev, buf->frame_seq)) {
                }
            }
            sink_n.fetch_add(1);
        });
        ch.start();
        for (int i = 0; i < 200 && ch.state() != CameraChannel::State::RUNNING; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(ch.state() == CameraChannel::State::RUNNING);  // 首帧迁移
        for (int i = 0; i < 200 && sink_n.load() < 3; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(sink_n.load() >= 3);                 // 采样出 ~3 帧（3fps 档）
        CHECK(last_seq.load() >= 3);               // frame_seq 单调
        CHECK(ch.sampler().fps() == 3);            // 默认档

        ch.stop();
        CHECK(ch.state() == CameraChannel::State::STOPPED);

        // EdgeMeter 不变量（静态守恒）：必须在 stop 后检查——运行中快照与帧处理
        // 存在瞬时在途窗口（pool 计数与 samp 计数非原子），逐点相等只在静息态成立
        auto d = snapshot(ch.dec_meter());
        auto pl = snapshot(ch.pool_meter());
        auto sm = snapshot(ch.samp_meter());
        auto q = snapshot(ch.q_ai().meter());
        CHECK(d.in == pl.in);
        CHECK(pl.in - pl.drop == sm.in);
        CHECK(sm.in == sm.out + sm.sampled);
        CHECK(q.in == q.out + q.drop + ch.q_ai().size());
        // 在途帧归零由 stop() 的 join+drain 保证（sink 闭包内无 frame 持有——生命周期纪律）
    }

    // ---- 2. RECONFIG：<500ms 且另一通道 meter 零变化 ----
    {
        std::shared_ptr<ScriptSource> s1, s2;
        CameraChannel ch1(1, "fake://", mk_factory(&s1, -1, 30, -1, false), mk_decoder(),
                          fast_params());
        CameraChannel ch2(2, "fake://", mk_factory(&s2, -1, 30, -1, false), mk_decoder(),
                          fast_params());
        ch1.start();
        ch2.start();
        for (int i = 0; i < 200 && (ch1.state() != CameraChannel::State::RUNNING ||
                                    ch2.state() != CameraChannel::State::RUNNING);
             ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(ch1.state() == CameraChannel::State::RUNNING);
        CHECK(ch2.state() == CameraChannel::State::RUNNING);

        // ch2 通路零波及的判别：reconfigure 前后 ch2 解码流持续增长（未被停止/重启）。
        // 逐点快照相等会与 30ms 帧节奏竞态，"流量延续"才是通道故障域隔离的实质
        const uint64_t m2_before = snapshot(ch2.dec_meter()).in;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<CameraChannel::AlgoCfg> algos;
        CameraChannel::AlgoCfg a;
        a.algo_id = "yolo";
        a.threshold = 0.6f;
        a.detect_fps = 5;
        algos.push_back(a);
        ch1.reconfigure(algos);
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(ch1.state() == CameraChannel::State::RUNNING);  // RECONFIG→RUNNING
        CHECK_MSG(dt < 500, "reconfigure took %lld ms", static_cast<long long>(dt));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(snapshot(ch2.dec_meter()).in > m2_before);  // ch2 流量延续
        ch1.stop();
        ch2.stop();
    }

    // ---- 3. 看门狗：无帧 >200ms（注入 5s 级别的短化参数）→ 重启恰好 1 次，state 不变 ----
    {
        std::shared_ptr<ScriptSource> src;
        // hang 源：0 帧后僵死（read 阻塞），close 解救 → 重连后恢复 100 帧
        CameraChannel ch(3, "fake://", mk_factory(&src, 3, 30, 100, true), mk_decoder(),
                         fast_params());
        ch.start();
        // 等僵死发生（3 帧 × 30ms 后 read 挂起）
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(src->opens() == 1);
        // 看门狗：last_frame 停 ~90ms 处，timeout=200ms → ~290ms 触发 close
        //         → src 线程 backoff 50ms → open（第 2 次）→ 恢复出帧
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK_MSG(src->opens() == 2, "opens=%d (expect exactly one restart)", src->opens());
        CHECK(ch.state() == CameraChannel::State::RUNNING);  // 状态不降级
        // 恢复出帧 → last_frame 刷新 → 不再重启（稳定）
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(src->opens() == 2);
        ch.stop();
    }

    if (g_fail == 0) printf("camera_channel PASSED\n");
    return g_fail;
}
